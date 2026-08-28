/**
* @file src/bin2llvmir/optimizations/x87_fpu/x87_fpu.cpp
* @brief x87 FPU analysis - replace fpu stack operations with FPU registers.
* @copyright (c) 2020 Avast Software, licensed under the MIT license
*/

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Operator.h>

#include <Eigen/Core>
#include <Eigen/QR>

#include "retdec/utils/io/log.h"
#include "retdec/bin2llvmir/optimizations/x87_fpu/x87_fpu.h"
#include "retdec/utils/string.h"
#include "retdec/bin2llvmir/providers/asm_instruction.h"
#include "retdec/bin2llvmir/utils/debug.h"
#define debug_enabled false

#include "retdec/bin2llvmir/utils/ir_modifier.h"
#include "retdec/bin2llvmir/utils/llvm.h"
#include "retdec/capstone2llvmir/x86/x86.h"

using namespace llvm;
using namespace retdec::bin2llvmir::llvm_utils;
using namespace retdec::utils::io;

namespace retdec {
namespace bin2llvmir {

int augmentedRank(Eigen::MatrixXd &A, Eigen::MatrixXd &B)
{
	A.conservativeResize(Eigen::NoChange, A.cols()+1);
	A.col(A.cols()-1) = B;

	int rankAugmentedA = A.colPivHouseholderQr().rank();

	A.conservativeResize(Eigen::NoChange, A.cols()-1);

	return rankAugmentedA;
}

class FunctionAnalyzeMetadata
{
	public:

		bool analyzeSuccess = true;
		enum IndexType {
			inIndex, outIndex
		};

		llvm::Function& function;
		std::map<llvm::BasicBlock*, std::map<IndexType,unsigned >> indexes;

		std::list<llvm::BasicBlock*> terminatingBasicBlocks;
		// A * x = B
		Eigen::MatrixXd A;
		Eigen::MatrixXd B;
		Eigen::MatrixXd x;

		int numberOfEquations = 0;

		// 1. index to register, 2.pseudo instruction
		std::list<std::pair<uint32_t ,llvm::Instruction*>> pseudoCalls;
		std::map<llvm::Value*, int> topVals;
		std::map<llvm::CallInst*, int> callTopVals;

		int expectedTop = 0;
		bool expectedTopAnalyzed = false;
		std::set<llvm::Function*> calledFunctions;

	void initSystem();
	void addEquation(const std::list<std::tuple<llvm::BasicBlock&,int,IndexType >>& vars, int result);
	FunctionAnalyzeMetadata(llvm::Function &function1) : function(function1) {};

};

char X87FpuAnalysis::ID = 0;

static RegisterPass<X87FpuAnalysis> X(
		"retdec-x87-fpu",
		"x87 fpu register analysis",
		false, // Only looks at CFG
		false // Analysis Pass
);

X87FpuAnalysis::X87FpuAnalysis() :
		ModulePass(ID)
{

}

bool X87FpuAnalysis::runOnModule(llvm::Module& m)
{
	_module = &m;
	_config = ConfigProvider::getConfig(_module);
	_abi = AbiProvider::getAbi(_module);
	return run();
}

bool X87FpuAnalysis::runOnModuleCustom(
		llvm::Module& m,
		Config* c,
		Abi* a)
{
	_module = &m;
	_config = c;
	_abi = a;
	return run();
}

std::list<FunctionAnalyzeMetadata> getFunctions2Analyze(llvm::GlobalVariable* top)
{
	std::list<Function*> functions;
	std::set<Function*> seenFunctions;
	for (Value::use_iterator k = top->use_begin(); k != top->use_end(); ++k)
	{
		if (Instruction *ins= dyn_cast<Instruction>(k->getUser()))
		{
			auto* function = ins->getParent()->getParent();
			if (seenFunctions.insert(function).second)
			{
				functions.push_back(function);
			}
		}
	}
	// A caller that only invokes an x87 helper still carries the helper's TOP
	// effect even if it never reads TOP directly.  Include transitive direct
	// callers so call-site entry contexts can be solved rather than silently
	// treating every helper call as net-zero.
	for (auto it = functions.begin(); it != functions.end(); ++it)
	{
		for (User* user : (*it)->users())
		{
			auto* call = dyn_cast<CallInst>(user);
			if (call == nullptr || call->getCalledFunction() != *it)
			{
				continue;
			}
			auto* caller = call->getFunction();
			if (!caller->isDeclaration() && seenFunctions.insert(caller).second)
			{
				functions.push_back(caller);
			}
		}
	}
	functions.sort();

	std::list<FunctionAnalyzeMetadata> functionsMetadata;
	for (auto &f : functions)
	{
		unsigned index = 0;
		FunctionAnalyzeMetadata metadata(*f);
		for (Function::iterator it = f->begin(), end = f->end(); it != end; ++it)
		{
			BasicBlock* bb = it.operator->();
			Instruction& endInst = bb->getInstList().back();
			if (dyn_cast<ReturnInst>(&endInst)) //it is terminating block
			{
				metadata.terminatingBasicBlocks.push_back(bb);
			}
			metadata.indexes[bb][FunctionAnalyzeMetadata::inIndex] = index;
			metadata.indexes[bb][FunctionAnalyzeMetadata::outIndex] = index+1;
			index += 2;
		}
		functionsMetadata.push_back(metadata);
	}

	return functionsMetadata;
}

void FunctionAnalyzeMetadata::initSystem()
{
	unsigned matrixLen = 1;// + terminatingBasicBlocks.size();
	for (Function::iterator bbIt=function.begin(), bbEndIt = function.end(); bbIt != bbEndIt; ++bbIt)
	{
		BasicBlock* bb = bbIt.operator->();
		matrixLen += 1 + pred_size(bb);
	}
	A.resize(matrixLen, 2*function.size());
	B.resize(matrixLen, 1);
	A.setZero();
	B.setZero();
}

void FunctionAnalyzeMetadata::addEquation(const std::list<std::tuple<llvm::BasicBlock&,int,IndexType >>& vars, int result)
{
	B(numberOfEquations, 0) = result;
	for (auto var : vars)
	{
		A(numberOfEquations, indexes[&std::get<0>(var)][std::get<2>(var)]) = std::get<1>(var);
	}

	numberOfEquations++;
}

bool X87FpuAnalysis::checkArchAndCallConvException(llvm::Function* fun)
{
	using CallingConvention = common::CallingConvention::eCC;

	auto configFunctionMetadata = _config->getConfig().functions.getFunctionByName(fun->getName());
	if (!configFunctionMetadata)
		return false;

	auto convention = configFunctionMetadata->callingConvention.getID();

	if (_config->getConfig().architecture.isX86_16() || _config->getConfig().architecture.isX86_32())
	{
		switch (convention)
		{
			case CallingConvention::CC_CDECL:
			case CallingConvention::CC_STDCALL:
			case CallingConvention::CC_PASCAL:
			case CallingConvention::CC_FASTCALL:
			case CallingConvention::CC_THISCALL:
			case CallingConvention::CC_UNKNOWN:
			default:
				return true;
			case CallingConvention::CC_WATCOM:
				return false; //inconsisten
		}
	}
	else // x86-64bit architecture
	{
		return false;
	}
}

bool X87FpuAnalysis::run()
{
	_runtimeEntryStackFunctions.clear();
	if (_config == nullptr || _abi == nullptr)
	{
		return ANALYZE_FAIL;
	}
	if (!_abi->isX86())
	{
		return ANALYZE_FAIL;
	}

	top = _abi->getRegister(X87_REG_TOP);
	if (top == nullptr)
	{
		return ANALYZE_FAIL;
	}

	auto analyzedFunctionsMetadata = getFunctions2Analyze(top);
	// Address-taken helpers, helpers with multiple call sites, and helpers called
	// from a CFG cycle may be entered at more than one architectural TOP.  Keep
	// those runtime-indexed.  A sole acyclic direct call can still use the
	// compact entry TOP inferred by the interprocedural fixed point below.
	for (auto& funMd : analyzedFunctionsMetadata)
	{
		bool dynamicEntry = funMd.function.hasFnAttribute(
				"retdec.pe32.relocated-entry");
		unsigned directCalls = 0;
		for (User* user : funMd.function.users())
		{
			auto* call = dyn_cast<CallInst>(user);
			if (call == nullptr || call->getCalledFunction() != &funMd.function)
			{
				dynamicEntry = true;
				continue;
			}
			++directCalls;

			BasicBlock* callBlock = call->getParent();
			std::set<BasicBlock*> visited;
			SmallVector<BasicBlock*, 8> worklist;
			worklist.append(succ_begin(callBlock), succ_end(callBlock));
			while (!worklist.empty() && !dynamicEntry)
			{
				BasicBlock* block = worklist.pop_back_val();
				if (block == callBlock)
				{
					dynamicEntry = true;
					break;
				}
				if (!visited.insert(block).second)
				{
					continue;
				}
				worklist.append(succ_begin(block), succ_end(block));
			}
		}
		if (dynamicEntry || directCalls > 1)
		{
			_runtimeEntryStackFunctions.insert(&funMd.function);
		}
	}
	auto analyzeFunctions = [&]()
	{
		for (auto& funMd: analyzedFunctionsMetadata)
		{
			funMd.analyzeSuccess = true;
			funMd.numberOfEquations = 0;
			funMd.pseudoCalls.clear();
			funMd.topVals.clear();
			funMd.callTopVals.clear();
			funMd.initSystem();
			BasicBlock& enterBlock = funMd.function.begin().operator*();
			funMd.addEquation({{enterBlock, 1, funMd.inIndex}}, EMPTY_FPU_STACK);

			for (Function::iterator bbIt=funMd.function.begin(),
				bbEndIt = funMd.function.end(); bbIt != bbEndIt; ++bbIt)
			{
				BasicBlock* bb = bbIt.operator->();
				int relativeOutBbTop = 0;

				if (!analyzeBasicBlock(analyzedFunctionsMetadata, funMd, bb, relativeOutBbTop))
				{
					funMd.analyzeSuccess = false;
				}

				funMd.addEquation({{*bb, -1, funMd.inIndex},{*bb, 1, funMd.outIndex}}, relativeOutBbTop);

				for (auto it = pred_begin(bb), et=pred_end(bb); it != et; ++it)
				{
					BasicBlock *pred = it.operator*();
					funMd.addEquation({{*bb, 1, funMd.inIndex},{*pred, -1, funMd.outIndex}}, 0);
				}
			}

			if (funMd.A.rows() <= PERFORMANCE_CEIL)
			{
				const auto& pivHouseholderQr = funMd.A.colPivHouseholderQr();
				int matRank = pivHouseholderQr.rank();
				int augmentedMatRank = augmentedRank(funMd.A, funMd.B);

				if (matRank == augmentedMatRank) // there is exactly one solution
				{
					funMd.x = pivHouseholderQr.solve(funMd.B);
				}
				else
				{
					funMd.analyzeSuccess = false;
				}
			}
			else // worst scenario => due to performance ceil turn to simple no CFG analyse
			{
				int height = funMd.A.rows();
				funMd.x.resize(height, 1);
				for (int i = 0; i < height; ++i)
				{
					funMd.x(i, 0) = EMPTY_FPU_STACK;
				}
			}
		}
	};

	// First solve local TOP changes, then propagate the inferred net stack
	// effect through defined calls.  expectedTop previously stayed at its
	// default zero unless a caller happened to load ST0 after the call, so two
	// calls to a pushing helper were both analyzed as empty-stack entries.
	bool effectsConverged = false;
	for (std::size_t iteration = 0;
			iteration <= analyzedFunctionsMetadata.size(); ++iteration)
	{
		analyzeFunctions();
		bool changed = false;
		for (auto& funMd : analyzedFunctionsMetadata)
		{
			if (!funMd.analyzeSuccess || funMd.terminatingBasicBlocks.empty())
			{
				continue;
			}
			int exitTop = static_cast<int>(round(funMd.x(
					funMd.indexes[funMd.terminatingBasicBlocks.front()]
							[funMd.outIndex], 0)));
			bool consistent = true;
			for (BasicBlock* exit : funMd.terminatingBasicBlocks)
			{
				consistent &= exitTop == static_cast<int>(round(funMd.x(
						funMd.indexes[exit][funMd.outIndex], 0)));
			}
			if (!consistent)
			{
				funMd.expectedTopAnalyzed = false;
				_runtimeEntryStackFunctions.insert(&funMd.function);
				for (User* user : funMd.function.users())
				{
					if (auto* call = dyn_cast<CallInst>(user))
					{
						_runtimeEntryStackFunctions.insert(call->getFunction());
					}
				}
				continue;
			}
			int effect = exitTop - EMPTY_FPU_STACK;
			if (!funMd.expectedTopAnalyzed || funMd.expectedTop != effect)
			{
				funMd.expectedTop = effect;
				funMd.expectedTopAnalyzed = true;
				changed = true;
			}
		}
		if (!changed)
		{
			effectsConverged = true;
			break;
		}
	}
	if (!effectsConverged)
	{
		// Recursive stack-changing call graphs may have no finite net effect.
		// Do not retain a bounded-iteration guess for them; runtime-index every
		// affected stack access instead.
		for (auto& funMd : analyzedFunctionsMetadata)
		{
			funMd.expectedTopAnalyzed = false;
			_runtimeEntryStackFunctions.insert(&funMd.function);
		}
	}
	// Ensure pseudo-call metadata and call-site TOPs correspond to the final
	// propagated effects rather than the preceding fixed-point iteration.
	analyzeFunctions();

	return optimizeAnalyzedFpuInstruction(analyzedFunctionsMetadata);
}

std::list<FunctionAnalyzeMetadata>::iterator X87FpuAnalysis::getFunMd(
		std::list<FunctionAnalyzeMetadata>& analyzedFunctionsMetadata,
		llvm::Function* fun)
{
	std::list<FunctionAnalyzeMetadata>::iterator it;
	for (it = analyzedFunctionsMetadata.begin(); it != analyzedFunctionsMetadata.end(); ++it)
	{
		auto& funMd = it.operator*();
		if (&funMd.function == fun)
		{
			return it;
		}
	}

	return analyzedFunctionsMetadata.end();
}

bool X87FpuAnalysis::analyzeInstruction(
		std::list<FunctionAnalyzeMetadata>& analyzedFunctionsMetadata,
		FunctionAnalyzeMetadata& funMd,
		Instruction* i,
		int& outTop)
{
	auto *callFunction = dyn_cast<CallInst>(i);
	auto *loadFpuTop = dyn_cast<LoadInst>(i);
	auto *storeFpuTop = dyn_cast<StoreInst>(i);
	auto *add = dyn_cast<AddOperator>(i);
	auto *sub = dyn_cast<SubOperator>(i);
	auto *callStore = _config->isLlvmX87StorePseudoFunctionCall(i);
	auto *callLoad = _config->isLlvmX87LoadPseudoFunctionCall(i);

	// read actual value of fpu top
	if (loadFpuTop && loadFpuTop->getPointerOperand() == top)
	{
		funMd.topVals[i] = outTop;
	}
	// store actual value of fpu top
	else if (storeFpuTop && storeFpuTop->getPointerOperand() == top && funMd.topVals.find(storeFpuTop->getValueOperand()) != funMd.topVals.end())
	{
		outTop = funMd.topVals.find(storeFpuTop->getValueOperand())->second;
	}
	// function call -> possible change value of fpu top
	else if (callFunction && !callStore && !callLoad
			&& (!callFunction->getCalledFunction()
					|| !callFunction->getCalledFunction()->isIntrinsic()))
	{
		auto* calledFunction = callFunction->getCalledFunction();
		// A callee entered while this function has a live x87 stack cannot use
		// the analysis's conventional empty-entry TOP to select a fixed ST
		// global. Remember it so its pseudo stack accesses are lowered through
		// the architectural runtime TOP instead.
		funMd.callTopVals[callFunction] = outTop;
		auto it = calledFunction
				? getFunMd(analyzedFunctionsMetadata, calledFunction)
				: analyzedFunctionsMetadata.end();

		if (it != analyzedFunctionsMetadata.end())
		{
			auto& fun = it.operator*();
			if (fun.expectedTopAnalyzed)
			{
				outTop += fun.expectedTop;
			}
			else
			{
				outTop += expectedTopBasedOnRestOfBlock(analyzedFunctionsMetadata, *i);
			}
		}
		else // some library function e.g "roundf()"
		{
			outTop += expectedTopBasedOnRestOfBlock(analyzedFunctionsMetadata, *i);
		}
	}
	// increment fpu top
	else if (add && isa<ConstantInt>(add->getOperand(1)) && funMd.topVals.find(add->getOperand(0)) != funMd.topVals.end())
	{
		//auto *op0 = dyn_cast<Instruction>(add->getOperand(0));
		int oldTopValue = funMd.topVals.find(add->getOperand(0))->second;
		int constValue = cast<ConstantInt>(add->getOperand(1))->getZExtValue();//it should be 1
		int newTopValue = oldTopValue + constValue;
		funMd.topVals[i] = newTopValue;
	}
	// decrement fpu top
	else if (sub && isa<ConstantInt>(sub->getOperand(1)) && funMd.topVals.find(sub->getOperand(0)) != funMd.topVals.end())
	{
		//auto *op0 = dyn_cast<Instruction>(sub->getOperand(0));
		int oldTopValue = funMd.topVals.find(sub->getOperand(0))->second;
		int constValue = cast<ConstantInt>(sub->getOperand(1))->getZExtValue();//it should be 1
		int newTopValue = oldTopValue - constValue;
		funMd.topVals[i] = newTopValue;
	}
	// pseudo load/store of fpu top
	else if (callStore || callLoad)
	{
		//pseudo call will be replaced by store/load of concrete register but only if whole analyze succed
		int tmp;
		if (callStore && funMd.topVals.find(callStore->getArgOperand(0)) != funMd.topVals.end())
		{
			tmp = funMd.topVals.find(callStore->getArgOperand(0))->second;
		}
		else if (callLoad && funMd.topVals.find(callLoad->getArgOperand(0)) != funMd.topVals.end())
		{
			tmp = funMd.topVals.find(callLoad->getArgOperand(0))->second;
		}
		else
		{
			return ANALYZE_FAIL;
		}

		funMd.pseudoCalls.push_back({tmp, i});
	}

	return ANALYZE_SUCCESS;
}

int X87FpuAnalysis::expectedTopBasedOnRestOfBlock(
		std::list<FunctionAnalyzeMetadata>& analyzedFunctionsMetadata,
		llvm::Instruction& analyzedInstr)
{
	if (!checkArchAndCallConvException(analyzedInstr.getParent()->getParent()))
	{
		return NOP_FPU_STACK;
	}

	std::map<llvm::Value*, bool> topVals;
	BasicBlock* bb = analyzedInstr.getParent();
	Instruction *next = analyzedInstr.getNextNode();

	if (!next || next->getParent() != bb)
	{
		return NOP_FPU_STACK;
	}

	for (BasicBlock::iterator it = next->getIterator(), e = bb->end(); it != e; ++it)
	{
		Instruction *i = it.operator->();
		auto *loadFpuTop = dyn_cast<LoadInst>(i);
		auto *sub = dyn_cast<SubOperator>(i);
		auto *callLoad = _config->isLlvmX87LoadPseudoFunctionCall(i);
		auto *callFunction = dyn_cast<CallInst>(i);

		if (loadFpuTop && loadFpuTop->getPointerOperand() == top)
		{
			topVals[i] = true;
		}
		else if (sub && isa<ConstantInt>(sub->getOperand(1)) && topVals.find(sub->getOperand(0)) != topVals.end())
		{
			return NOP_FPU_STACK;
		}
		else if (callFunction && !callLoad)
		{
			return NOP_FPU_STACK;
		}
		else if (callLoad && topVals.find(callLoad->getArgOperand(0)) != topVals.end())
		{
			auto *callFunction = dyn_cast<CallInst>(&analyzedInstr);
			auto* calledFunction = callFunction
					? callFunction->getCalledFunction()
					: nullptr;
			if (calledFunction)
			{
				auto it = getFunMd(analyzedFunctionsMetadata, calledFunction);
				if (it != analyzedFunctionsMetadata.end())
				{
					auto& fun = it.operator*();
					fun.expectedTop = RETURN_VALUE_PASSED_THROUGH_ST0;
					fun.expectedTopAnalyzed = true;
				}
			}
			return DECREMENT_FPU_STACK;
		}
	}

	return NOP_FPU_STACK;
}

bool X87FpuAnalysis::analyzeBasicBlock(
	std::list<FunctionAnalyzeMetadata>& analyzedFunctionsMetadata,
	FunctionAnalyzeMetadata& funMd,
	llvm::BasicBlock* bb,
	int& outTop)
{
	std::map<llvm::Value*, int> topVals;
	for (BasicBlock::iterator it = bb->begin(), e = bb->end(); it != e; ++it)
	{
		Instruction* inst = it.operator->();
		if (!analyzeInstruction(analyzedFunctionsMetadata, funMd, inst, outTop))
		{
			return ANALYZE_FAIL;
		}
	}

	return ANALYZE_SUCCESS;
}

bool X87FpuAnalysis::isValidRegisterIndex(int index)
{
	return (X86_REG_ST0 <= index && index <= X86_REG_ST7);
}

bool X87FpuAnalysis::requiresRuntimeStackIndex(llvm::CallInst* call) const
{
	if (_runtimeEntryStackFunctions.count(call->getFunction()) != 0)
	{
		return true;
	}

	Value* index = call->getArgOperand(0);
	Instruction* indexInstruction = dyn_cast<Instruction>(index);

	// Look through the add/subtract used to address ST(i) relative to TOP.
	while (auto* binary = dyn_cast_or_null<BinaryOperator>(indexInstruction))
	{
		if ((binary->getOpcode() != Instruction::Add
				&& binary->getOpcode() != Instruction::Sub)
				|| !isa<ConstantInt>(binary->getOperand(1)))
		{
			break;
		}
		indexInstruction = dyn_cast<Instruction>(binary->getOperand(0));
	}

	auto* topLoad = dyn_cast_or_null<LoadInst>(indexInstruction);
	if (topLoad == nullptr || topLoad->getPointerOperand() != top)
	{
		// A value arriving through a PHI or another block cannot safely be
		// replaced by the pass's intraprocedural numeric TOP estimate.
		return true;
	}

	if (topLoad->getParent() != call->getParent())
	{
		return true;
	}

	// A callee may change TOP.  A load after such a call is live runtime state,
	// even when the matrix analysis can guess a conventional return delta.
	for (Instruction* i = topLoad->getPrevNode(); i != nullptr; i = i->getPrevNode())
	{
		if (auto* store = dyn_cast<StoreInst>(i))
		{
			if (store->getPointerOperand() == top)
			{
				return false;
			}
		}

		auto* ordinaryCall = dyn_cast<CallInst>(i);
		if (ordinaryCall == nullptr
				|| _config->isLlvmX87StorePseudoFunctionCall(ordinaryCall)
				|| _config->isLlvmX87LoadPseudoFunctionCall(ordinaryCall))
		{
			continue;
		}

		auto* called = ordinaryCall->getCalledFunction();
		if (called == nullptr || called->isDeclaration())
		{
			return true;
		}

		// Calls to defined functions are covered by the pass's interprocedural
		// stack analysis.  Keep its compact, statically selected register for
		// those calls; only opaque callees force a runtime stack lookup.
	}

	return false;
}

void X87FpuAnalysis::lowerRuntimeStore(llvm::CallInst* call, uint32_t regBase)
{
	auto* index = call->getArgOperand(0);
	auto* value = call->getArgOperand(1);
	IRBuilder<> irb(call);

	for (unsigned regNum = 0; regNum < EMPTY_FPU_STACK; ++regNum)
	{
		auto* reg = _abi->getRegister(regBase + regNum);
		assert(reg != nullptr);
		auto* oldValue = irb.CreateLoad(reg);
		auto* converted = IrModifier::convertValueToType(
				value, reg->getValueType(), call);
		auto* selected = irb.CreateSelect(
				irb.CreateICmpEQ(index, ConstantInt::get(index->getType(), regNum)),
				converted,
				oldValue);
		irb.CreateStore(selected, reg);
	}

	call->eraseFromParent();
}

void X87FpuAnalysis::lowerRuntimeLoad(llvm::CallInst* call, uint32_t regBase)
{
	auto* index = call->getArgOperand(0);
	IRBuilder<> irb(call);
	Value* result = nullptr;

	for (unsigned regNum = 0; regNum < EMPTY_FPU_STACK; ++regNum)
	{
		auto* reg = _abi->getRegister(regBase + regNum);
		assert(reg != nullptr);
		auto* loaded = irb.CreateLoad(reg);
		auto* converted = IrModifier::convertValueToType(
				loaded, call->getType(), call);
		if (result == nullptr)
		{
			result = converted;
		}
		else
		{
			result = irb.CreateSelect(
					irb.CreateICmpEQ(index, ConstantInt::get(index->getType(), regNum)),
					converted,
					result);
		}
	}

	call->replaceAllUsesWith(result);
	call->eraseFromParent();
}

bool X87FpuAnalysis::optimizeAnalyzedFpuInstruction(
		std::list<FunctionAnalyzeMetadata>& analyzedFunctionsMetadata)
{
	bool analyzeSucces = true;

	// Resolve each call site's relative TOP against its basic-block entry
	// solution before rewriting pseudo stack accesses. A callee entered with a
	// live x87 value must address ST(i) using the architectural runtime TOP;
	// assuming the conventional empty function entry can otherwise make caller
	// and callee select different physical ST globals.
	for (auto& funMd : analyzedFunctionsMetadata)
	{
		if (!funMd.analyzeSuccess)
		{
			continue;
		}
		for (const auto& callTop : funMd.callTopVals)
		{
			auto* calledFunction = callTop.first->getCalledFunction();
			if (calledFunction == nullptr)
			{
				continue;
			}
			double bbIn = funMd.x(
					funMd.indexes[callTop.first->getParent()][funMd.inIndex], 0);
			int actualTop = static_cast<int>(round(bbIn)) + callTop.second;
			if (actualTop != EMPTY_FPU_STACK)
			{
				// Both sides of a call carrying a live x87 stack must use the
				// architectural TOP.  The caller's intraprocedural CFG solution
				// may use a different numeric origin than the runtime TOP (and a
				// callee may update TOP), so a statically selected caller ST slot
				// can otherwise be one or more entries away from the value the
				// callee observes.
				_runtimeEntryStackFunctions.insert(&funMd.function);
				_runtimeEntryStackFunctions.insert(calledFunction);
			}
		}
	}

	// A runtime-indexed entry passes its unknown physical TOP origin through
	// every defined x87 callee, even when an individual call has zero relative
	// stack depth.  Close this relation transitively after the live-stack scan
	// above has added its own runtime entries.
	bool runtimeClosureChanged = true;
	while (runtimeClosureChanged)
	{
		runtimeClosureChanged = false;
		for (auto& funMd : analyzedFunctionsMetadata)
		{
			if (_runtimeEntryStackFunctions.count(&funMd.function) == 0)
			{
				continue;
			}
			for (const auto& callTop : funMd.callTopVals)
			{
				auto* calledFunction = callTop.first->getCalledFunction();
				if (calledFunction != nullptr
						&& getFunMd(analyzedFunctionsMetadata, calledFunction)
								!= analyzedFunctionsMetadata.end())
				{
					runtimeClosureChanged |= _runtimeEntryStackFunctions.insert(
							calledFunction).second;
				}
			}
		}
	}

	for (auto& funMd : analyzedFunctionsMetadata)
	{
		if (!funMd.analyzeSuccess)
		{
			analyzeSucces = false;
			continue;
		}

		for (auto& i : funMd.pseudoCalls)
		{
			uint32_t regBase = uint32_t(X86_REG_ST0);
			auto *callStore = _config->isLlvmX87StorePseudoFunctionCall(i.second);
			auto *callLoad = _config->isLlvmX87LoadPseudoFunctionCall(i.second);
			auto* pseudoCall = cast<CallInst>(i.second);

			if (requiresRuntimeStackIndex(pseudoCall))
			{
				if (callStore)
				{
					lowerRuntimeStore(pseudoCall, regBase);
				}
				else
				{
					lowerRuntimeLoad(pseudoCall, regBase);
				}
				continue;
			}

			double bbIn = funMd.x(funMd.indexes[i.second->getParent()][funMd.inIndex], 0);
			int diff = (int)i.first % EMPTY_FPU_STACK; // correction of possible stack over/under-flow
			int top = (int)round(bbIn) + diff; // value of stack at the beginnig of BB + difference at actual instr

			int registerIndex;
			GlobalVariable *reg;
			if (!isValidRegisterIndex(registerIndex = regBase + top%EMPTY_FPU_STACK) || !(reg =_abi->getRegister(registerIndex)))
			{
				analyzeSucces = false;
				continue;
			}

			if (callStore)
			{
				new StoreInst(callStore->getArgOperand(1), reg, callStore);
				callStore->eraseFromParent();
			}
			if (callLoad)
			{
				auto *lTmp = new LoadInst(reg, "", callLoad);
				auto *conv = IrModifier::convertValueToType(lTmp, callLoad->getType(), callLoad);
				callLoad->replaceAllUsesWith(conv);
				callLoad->eraseFromParent();
			}
		}
	}

	// A failed static TOP analysis must not leave frontend-only pseudo calls in
	// backend IR.  Their index operand already denotes the architectural TOP,
	// so the runtime-indexed lowering is a semantics-preserving fallback for
	// irreducible or otherwise ambiguous control flow.
	SmallVector<CallInst*, 32> remainingPseudoCalls;
	for (Function& f : *_module)
	{
		for (Instruction& i : instructions(f))
		{
			if (_config->isLlvmX87StorePseudoFunctionCall(&i)
					|| _config->isLlvmX87LoadPseudoFunctionCall(&i))
			{
				remainingPseudoCalls.push_back(cast<CallInst>(&i));
			}
		}
	}
	for (CallInst* call : remainingPseudoCalls)
	{
		if (_config->isLlvmX87StorePseudoFunctionCall(call))
		{
			lowerRuntimeStore(call, uint32_t(X86_REG_ST0));
		}
		else
		{
			lowerRuntimeLoad(call, uint32_t(X86_REG_ST0));
		}
	}
	return analyzeSucces;
}

} // namespace bin2llvmir
} // namespace retdec
