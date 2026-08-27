/**
* @file src/bin2llvmir/optimizations/x87_fpu/x87_fpu.cpp
* @brief x87 FPU analysis - replace fpu stack operations with FPU registers.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
*/

#include <llvm/IR/CFG.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Operator.h>

#include "retdec/bin2llvmir/optimizations/x87_fpu/x87_fpu.h"
#include "retdec/utils/string.h"
#include "retdec/bin2llvmir/providers/asm_instruction.h"
#include "retdec/bin2llvmir/utils/debug.h"
#define debug_enabled false
#include "retdec/bin2llvmir/utils/ir_modifier.h"
#include "retdec/bin2llvmir/utils/llvm.h"

using namespace llvm;
using namespace retdec::bin2llvmir::llvm_utils;

namespace retdec {
namespace bin2llvmir {

char X87FpuAnalysis::ID = 0;

static RegisterPass<X87FpuAnalysis> X(
		"x87-fpu",
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

bool X87FpuAnalysis::run()
{
	if (_config == nullptr || _abi == nullptr)
	{
		return false;
	}
	if (!_abi->isX86())
	{
		return false;
	}

	top = _abi->getRegister(X87_REG_TOP);
	if (top == nullptr)
	{
		return false;
	}

	bool changed = false;
	for (Function& f : *_module)
	{
		LOG << f.getName().str() << std::endl;

		retdec::utils::NonIterableSet<BasicBlock*> seenBbs;
		std::map<Value*, int> topVals;

		for (auto& bb : f)
		{
			int topVal = 8;
			changed |= analyzeBb(seenBbs, topVals, &bb, topVal);
		}
	}
	return changed;
}

bool X87FpuAnalysis::analyzeBb(
		retdec::utils::NonIterableSet<llvm::BasicBlock*>& seenBbs,
		std::map<llvm::Value*, int>& topVals,
		llvm::BasicBlock* bb,
		int topVal)
{
	LOG << "\t" << bb->getName().str() << std::endl;
	bool changed = false;

	if (seenBbs.has(bb))
	{
		LOG << "\t\t" << "already seen" << std::endl;
		return false;
	}
	seenBbs.insert(bb);

	auto it = bb->begin();
	while (it != bb->end())
	{
		Instruction* i = &(*it);
		++it;

		auto* l = dyn_cast<LoadInst>(i);
		auto* s = dyn_cast<StoreInst>(i);
		auto* add = dyn_cast<AddOperator>(i);
		auto* sub = dyn_cast<SubOperator>(i);
		auto* callStore = _config->isLlvmX87StorePseudoFunctionCall(i);
		auto* callLoad = _config->isLlvmX87LoadPseudoFunctionCall(i);

		if (l && l->getPointerOperand() == top)
		{
			topVals[i] = topVal;

			LOG << "\t\t" << AsmInstruction(i).getAddress()
					<< " @ " << std::dec << topVal << std::endl;
		}
		else if (s
				&& s->getPointerOperand() == top
				&& topVals.find(s->getValueOperand()) != topVals.end())
		{
			auto fIt = topVals.find(s->getValueOperand());
			topVal = fIt->second;

			LOG << "\t\t" << AsmInstruction(i).getAddress()
					<< " @ " << std::dec << fIt->second << std::endl;
		}
		else if (add
				&& topVals.find(add->getOperand(0)) != topVals.end()
				&& isa<ConstantInt>(add->getOperand(1)))
		{
			auto fIt = topVals.find(add->getOperand(0));
			auto* ci = cast<ConstantInt>(add->getOperand(1));
			// Constants are i3, so 7 can be represented as -1, we need to either
			// use zext here (potentially dangerous if instructions were already
			// modified and there are true negative values), or compute values
			// in i3 arithmetics.
			int tmp = fIt->second + ci->getZExtValue();
			if (tmp > 8)
			{
				LOG << "\t\t\t" << "overflow fix " << tmp << " -> " << 8
						<< std::endl;
				tmp = 8;
			}
			topVals[i] = tmp;

			LOG << "\t\t" << AsmInstruction(i).getAddress() << std::dec
					<< " @ " << fIt->second << " + " << ci->getZExtValue()
					<< " = " << tmp << std::endl;
		}
		else if (sub
				&& topVals.find(sub->getOperand(0)) != topVals.end()
				&& isa<ConstantInt>(sub->getOperand(1)))
		{
			auto fIt = topVals.find(sub->getOperand(0));
			auto* ci = cast<ConstantInt>(sub->getOperand(1));
			// Constants are i3, so 7 can be represented as -1, we need to either
			// use zext here (potentially dangerous if instructions were already
			// modified and there are true negative values), or compute values
			// in i3 arithmetics.
			int tmp = fIt->second - ci->getZExtValue();
			if (tmp < 0)
			{
				LOG << "\t\t\t" << "undeflow fix " << tmp << " -> " << 7
						<< std::endl;
				tmp = 7;
			}
			topVals[i] = tmp;

			LOG << "\t\t" << AsmInstruction(i).getAddress() << std::dec
					<< " @ " << fIt->second << " - " << ci->getZExtValue() << " = "
					<< tmp << std::endl;
		}
		else if (callStore)
		{
			lowerStore(callStore);
			changed = true;
		}
		else if (callLoad)
		{
			lowerLoad(callLoad);
			changed = true;
		}
	}

	for (auto succIt = succ_begin(bb), e = succ_end(bb); succIt != e; ++succIt)
	{
		auto* succ = *succIt;
		changed |= analyzeBb(seenBbs, topVals, succ, topVal);
	}

	return changed;
}

void X87FpuAnalysis::lowerStore(llvm::CallInst* call)
{
	auto regBase = _config->isLlvmX87DataStorePseudoFunctionCall(call)
			? uint32_t(X86_REG_ST0)
			: uint32_t(X87_REG_TAG0);
	auto* index = call->getArgOperand(0);
	auto* value = call->getArgOperand(1);
	IRBuilder<> irb(call);

	// TOP is runtime state and may be changed by a callee.  Store through the
	// actual index instead of assigning a physical register from intraprocedural
	// TOP tracking.  Selects avoid introducing control-flow into decoded blocks.
	for (unsigned regNum = 0; regNum < 8; ++regNum)
	{
		auto* reg = _abi->getRegister(regBase + regNum);
		assert(reg);
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

void X87FpuAnalysis::lowerLoad(llvm::CallInst* call)
{
	auto regBase = _config->isLlvmX87DataLoadPseudoFunctionCall(call)
			? uint32_t(X86_REG_ST0)
			: uint32_t(X87_REG_TAG0);
	auto* index = call->getArgOperand(0);
	IRBuilder<> irb(call);
	Value* result = nullptr;

	// Select the register using the live TOP value.  This preserves x87 values
	// passed implicitly across function calls, including callees which pop TOP.
	for (unsigned regNum = 0; regNum < 8; ++regNum)
	{
		auto* reg = _abi->getRegister(regBase + regNum);
		assert(reg);
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

} // namespace bin2llvmir
} // namespace retdec
