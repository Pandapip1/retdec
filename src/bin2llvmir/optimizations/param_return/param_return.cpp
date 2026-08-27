/**
* @file src/bin2llvmir/optimizations/param_return/param_return.cpp
* @brief Detect functions' parameters and returns.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
*
* Original implementation:
*   name -- position of format string, position of variadic arg start
*   printf/scanf -- 0, 1
*   __printf_chk -- 1, 2
*   __fprintf_chk -- 2, 3
*   fprintf/fscanf/wsprintfA/wsprintf/sprintf/sscanf -- 1, 2
*   snprintf -- 2, 3
*   __snprintf_chk -- 4, 5
*   ioctl/open/FmtStr -- not handled -- erase arguments
*   wprintf/wscanf -- 0, 1
*   error -- 2, 3
*   error_at_line -- 4, 5
*   other -- not handled -- copy arguments
*/

#include <cassert>
#include <iomanip>
#include <iostream>
#include <limits>

#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>

#include "retdec/utils/container.h"
#include "retdec/utils/string.h"
#include "retdec/bin2llvmir/optimizations/param_return/param_return.h"
#define debug_enabled false
#include "retdec/bin2llvmir/utils/llvm.h"
#include "retdec/bin2llvmir/providers/asm_instruction.h"
#include "retdec/bin2llvmir/utils/ir_modifier.h"

using namespace retdec::utils;
using namespace llvm;

namespace retdec {
namespace bin2llvmir {

llvm::Value* getRoot(ReachingDefinitionsAnalysis& RDA, llvm::Value* i, bool first = true)
{
	static std::set<llvm::Value*> seen;
	if (first)
	{
		seen.clear();
	}
	if (seen.count(i))
	{
		return i;
	}
	seen.insert(i);

	i = llvm_utils::skipCasts(i);
	if (auto* ii = dyn_cast<Instruction>(i))
	{
		if (auto* u = RDA.getUse(ii))
		{
			if (u->defs.size() == 1)
			{
				auto* d = (*u->defs.begin())->def;
				if (auto* s = dyn_cast<StoreInst>(d))
				{
					return getRoot(RDA, s->getValueOperand(), false);
				}
				else
				{
					return d;
				}
			}
			else if (auto* l = dyn_cast<LoadInst>(ii))
			{
				return getRoot(RDA, l->getPointerOperand(), false);
			}
			else
			{
				return i;
			}
		}
		else if (auto* l = dyn_cast<LoadInst>(ii))
		{
			return getRoot(RDA, l->getPointerOperand(), false);
		}
		else
		{
			return i;
		}
	}

	return i;
}

//
//=============================================================================
//  ParamReturn
//=============================================================================
//

char ParamReturn::ID = 0;

static RegisterPass<ParamReturn> X(
		"param-return",
		"Function parameters and returns optimization",
		false, // Only looks at CFG
		false // Analysis Pass
);

ParamReturn::ParamReturn() :
		ModulePass(ID)
{

}

bool ParamReturn::runOnModule(Module& m)
{
	_module = &m;
	_config = ConfigProvider::getConfig(_module);
	_abi = AbiProvider::getAbi(_module);
	_image = FileImageProvider::getFileImage(_module);
	_dbgf = DebugFormatProvider::getDebugFormat(_module);
	_lti = LtiProvider::getLti(_module);
	return run();
}

bool ParamReturn::runOnModuleCustom(
		llvm::Module& m,
		Config* c,
		Abi* abi,
		FileImage* img,
		DebugFormat* dbgf,
		Lti* lti)
{
	_module = &m;
	_config = c;
	_abi = abi;
	_image = img;
	_dbgf = dbgf;
	_lti = lti;
	return run();
}

bool ParamReturn::run()
{
	if (_config == nullptr)
	{
		LOG << "[ABORT] config file is not available\n";
		return false;
	}

	_RDA.runOnModule(*_module, AbiProvider::getAbi(_module));

//dumpModuleToFile(_module);

	collectAllCalls();
	dumpInfo();
	filterCalls();
	dumpInfo();
	applyToIr();

	_RDA.clear();

//dumpModuleToFile(_module);
//exit(1);

	return false;
}

/**
 * Collect possible arguments' stores for all calls we want to analyze.
 * At the moment, we analyze only indirect or declared function calls with no
 * arguments inside one basic block.
 */
void ParamReturn::collectAllCalls()
{
	for (auto& f : _module->getFunctionList())
	{
		if (f.isIntrinsic())
		{
			continue;
		}

		_fnc2calls.emplace(
				std::make_pair(
						&f,
						DataFlowEntry(
								_module,
								_RDA,
								_config,
								_abi,
								_image,
								_dbgf,
								_lti,
								&f)));
	}

	for (auto& f : _module->getFunctionList())
	for (auto& b : f)
	for (auto& i : b)
	{
		auto* call = dyn_cast<CallInst>(&i);
		if (call == nullptr || call->getNumArgOperands() != 0)
		{
			continue;
		}

		auto* calledVal = call->getCalledValue();
		auto* calledFnc = call->getCalledFunction();

		if (calledFnc && calledFnc->isIntrinsic())
		{
			continue;
		}

		auto fIt = _fnc2calls.find(calledVal);
		if (fIt == _fnc2calls.end())
		{
			fIt = _fnc2calls.emplace(std::make_pair(
					calledVal,
					DataFlowEntry(
							_module,
							_RDA,
							_config,
							_abi,
							_image,
							_dbgf,
							_lti,
							calledVal))).first;
		}

		fIt->second.addCall(call);
	}
}

void ParamReturn::filterCalls()
{
	for (auto& p : _fnc2calls)
	{
		p.second.filter();
	}
}

void ParamReturn::applyToIr()
{
	for (auto& p : _fnc2calls)
	{
		p.second.applyToIr();
	}

	for (auto& p : _fnc2calls)
	{
		p.second.connectWrappers();
	}
}

/**
 * Dump all the info collected and processed so far.
 */
void ParamReturn::dumpInfo()
{
	LOG << std::endl << "_fnc2calls:" << std::endl;
	for (auto& p : _fnc2calls)
	{
		p.second.dump();
	}
}

//
//=============================================================================
//  CallEntry
//=============================================================================
//

CallEntry::CallEntry(llvm::CallInst* c) :
		call(c)
{

}

bool CallEntry::isStackArgumentStore(
		Config* _config,
		llvm::StoreInst* store) const
{
	return _config->isStackVariable(store->getPointerOperand())
			|| unresolvedStackArgStores.count(store) != 0;
}

bool registerCanBeParameterAccordingToAbi(Config* _config, llvm::Value* val)
{
	if (!_config->isRegister(val))
	{
		return true;
	}

	if (_config->getConfig().architecture.isX86())
	{
		return false;
	}
	else if (_config->getConfig().architecture.isPpc())
	{
		static std::set<std::string> names = {"r3", "r4", "r5", "r6", "r7", "r8", "r9"};
		if (names.find(val->getName()) == names.end())
		{
			return false;
		}
	}
	else if (_config->getConfig().architecture.isArmOrThumb())
	{
		static std::set<std::string> names = {"r0", "r1", "r2", "r3"};
		if (names.find(val->getName()) == names.end())
		{
			return false;
		}
	}
	else if (_config->getConfig().architecture.isMipsOrPic32())
	{
		static std::set<std::string> names = {"a0", "a1", "a2", "a3"};
		if (names.find(val->getName()) == names.end())
		{
			return false;
		}
	}

	return true;
}

/**
 * Remove all registers that are not used to pass argument according to ABI.
 */
void CallEntry::filterRegisters(Config* _config)
{
	auto it = possibleArgStores.begin();
	while (it != possibleArgStores.end())
	{
		auto* op = (*it)->getPointerOperand();
		if (!registerCanBeParameterAccordingToAbi(_config, op))
		{
			it = possibleArgStores.erase(it);
		}
		else
		{
			++it;
		}
	}
}

void DataFlowEntry::filterRegistersArgLoads()
{
	auto it = argLoads.begin();
	while (it != argLoads.end())
	{
		auto* op = (*it)->getPointerOperand();
		if (!registerCanBeParameterAccordingToAbi(_config, op))
		{
			it = argLoads.erase(it);
		}
		else
		{
			auto aOff = _config->getStackVariableOffset(op);
			if (aOff.isDefined() && aOff < 0)
			{
				it = argLoads.erase(it);
			}
			else
			{
				++it;
			}
		}
	}
}

/**
 * Stack with the lowest (highest negative) offset is the first call argument.
 */
void CallEntry::filterSort(Config* _config)
{
	auto& stores = possibleArgStores;

	// Stack reconstruction cannot always turn a pushed argument into an alloca.
	// In particular, this happens when ESP is loop-carried and its absolute
	// offset is not known. addCallArgs() still recognizes such pushes from the
	// paired stack-pointer update. Its backwards scan already encounters x86
	// pushes in argument order, while sorting an unresolved store as if it were
	// a register would move it to an arbitrary position.
	bool preserveRecoveredX86Order =
			_config->getConfig().architecture.isX86()
			&& !unresolvedStackArgStores.empty();
	if (!preserveRecoveredX86Order)
	{
		std::stable_sort(
				stores.begin(),
				stores.end(),
				[_config](StoreInst* a, StoreInst* b) -> bool
		{
			auto aOff = _config->getStackVariableOffset(a->getPointerOperand());
			auto bOff = _config->getStackVariableOffset(b->getPointerOperand());

			if (aOff.isUndefined() && bOff.isUndefined())
			{
				return _config->getConfigRegisterNumber(a->getPointerOperand()) <
						_config->getConfigRegisterNumber(b->getPointerOperand());
			}
			else if (aOff.isUndefined() && bOff.isDefined())
			{
				return true;
			}
			else if (aOff.isDefined() && bOff.isUndefined())
			{
				return false;
			}
			else
			{
				return aOff < bOff;
			}
		});
	}

	if (_config->getConfig().architecture.isX86())
	{
		struct OutgoingRange
		{
			int64_t begin;
			int64_t end;
		};
		std::vector<OutgoingRange> occupied;
		auto it = stores.begin();
		while (it != stores.end())
		{
			auto off = _config->getStackVariableOffset(
					(*it)->getPointerOperand());
			if (off.isUndefined())
			{
				++it;
				continue;
			}

			uint64_t size = (*it)->getModule()->getDataLayout()
					.getTypeStoreSize((*it)->getValueOperand()->getType());
			auto instruction = AsmInstruction(*it);
			auto* capstone = instruction.isValid()
					? instruction.getCapstoneInsn() : nullptr;
			if (capstone != nullptr && capstone->id == X86_INS_PUSH)
			{
				size = _config->getConfig().architecture.getByteSize();
			}

			int64_t begin = off.getValue();
			int64_t end = begin + size;
			bool overwritten = false;
			for (const auto& range : occupied)
			{
				if (range.begin <= begin && begin < range.end)
				{
					overwritten = true;
					break;
				}
			}
			if (overwritten)
			{
				unresolvedStackArgStores.erase(*it);
				it = stores.erase(it);
			}
			else
			{
				occupied.push_back({begin, end});
				++it;
			}
		}
	}
}

void DataFlowEntry::filterSortArgLoads()
{
	std::stable_sort(
			argLoads.begin(),
			argLoads.end(),
			[this](LoadInst* a, LoadInst* b) -> bool
	{
		auto aOff = _config->getStackVariableOffset(a->getPointerOperand());
		auto bOff = _config->getStackVariableOffset(b->getPointerOperand());

		if (aOff.isUndefined() && bOff.isUndefined())
		{
			return _config->getConfigRegisterNumber(a->getPointerOperand()) <
					_config->getConfigRegisterNumber(b->getPointerOperand());
		}
		else if (aOff.isUndefined() && bOff.isDefined())
		{
			return true;
		}
		else if (aOff.isDefined() && bOff.isUndefined())
		{
			return false;
		}
		else
		{
			return aOff < bOff;
		}
	});

	// The same x86 stack argument may be read through several overlapping
	// views.  A common example is a double read at [ebp+8] followed by an
	// exponent-word read at [ebp+14].  StackAnalysis represents those accesses
	// by separate allocas, but they describe one incoming object.  Keep the
	// widest first view and let the later stack-frame pass preserve the aliasing
	// between its sub-accesses.
	if (_config->getConfig().architecture.isX86())
	{
		struct StackArgumentRange
		{
			int64_t begin;
			int64_t end;
			Value* storage;
		};
		std::vector<StackArgumentRange> occupied;
		auto it = argLoads.begin();
		while (it != argLoads.end())
		{
			auto off = _config->getStackVariableOffset(
					(*it)->getPointerOperand());
			auto* type = (*it)->getType();
			if (off.isUndefined() || !type->isSized())
			{
				++it;
				continue;
			}

			uint64_t accessSize =
					_module->getDataLayout().getTypeStoreSize(type);
			uint64_t truncatedSize = 0;
			bool onlyTruncatedUses = !(*it)->use_empty();
			for (auto* user : (*it)->users())
			{
				auto* truncation = dyn_cast<TruncInst>(user);
				if (truncation != nullptr && truncation->getType()->isSized())
				{
					truncatedSize = std::max(
							truncatedSize,
							_module->getDataLayout().getTypeStoreSize(
									truncation->getType()));
					continue;
				}

				// Decoded partial-register code commonly moves a full stack word to
				// EAX and then uses AX.  Follow that one register-definition hop so
				// the effective stack access is the consumed low word, rather than
				// the deliberately over-reading machine load.
				auto* store = dyn_cast<StoreInst>(user);
				if (store == nullptr
						|| !_config->isRegister(store->getPointerOperand()))
				{
					onlyTruncatedUses = false;
					break;
				}
				auto* definition = _RDA.getDef(store);
				if (definition == nullptr || definition->uses.empty())
				{
					onlyTruncatedUses = false;
					break;
				}
				for (auto* use : definition->uses)
				{
					auto* registerLoad = dyn_cast<LoadInst>(use->use);
					if (registerLoad == nullptr || registerLoad->use_empty())
					{
						onlyTruncatedUses = false;
						break;
					}
					for (auto* propagatedUser : registerLoad->users())
					{
						auto* propagatedTruncation =
								dyn_cast<TruncInst>(propagatedUser);
						if (propagatedTruncation == nullptr
								|| !propagatedTruncation->getType()->isSized())
						{
							onlyTruncatedUses = false;
							break;
						}
						truncatedSize = std::max(
								truncatedSize,
								_module->getDataLayout().getTypeStoreSize(
										propagatedTruncation->getType()));
					}
					if (!onlyTruncatedUses)
					{
						break;
					}
				}
				if (!onlyTruncatedUses)
				{
					break;
				}
			}
			if (onlyTruncatedUses && truncatedSize != 0)
			{
				accessSize = truncatedSize;
			}

			int64_t begin = off.getValue();
			int64_t end = begin + accessSize;
			bool overlaps = false;
			Value* containingStorage = nullptr;
			int64_t containingBegin = 0;
			for (const auto& range : occupied)
			{
				if (range.begin <= begin && begin < range.end)
				{
					overlaps = true;
					containingStorage = range.storage;
					containingBegin = range.begin;
					break;
				}
			}
			if (overlaps)
			{
				overlappingArgAccesses[*it] = {
						containingStorage, begin - containingBegin};
				it = argLoads.erase(it);
			}
			else
			{
				occupied.push_back({
						begin, end, (*it)->getPointerOperand()});
				++it;
			}
		}

		// Rebase every recovered access into a retained incoming argument, not
		// only the first undefined load that identified the argument.  A later
		// load can have a local store as one reaching definition (for example a
		// loop that updates a double's exponent word), but its initial value still
		// aliases the incoming argument bytes.  Independent allocas would lose
		// both that initial value and subsequent partial writes.
		auto* function = getFunction();
		if (function != nullptr)
		{
			for (auto& block : *function)
			for (auto& instruction : block)
			{
				Value* pointer = nullptr;
				if (auto* load = dyn_cast<LoadInst>(&instruction))
				{
					pointer = load->getPointerOperand();
				}
				else if (auto* store = dyn_cast<StoreInst>(&instruction))
				{
					pointer = store->getPointerOperand();
				}
				if (pointer == nullptr)
				{
					continue;
				}

				auto offset = _config->getStackVariableOffset(pointer);
				if (offset.isUndefined())
				{
					continue;
				}
				for (const auto& range : occupied)
				{
					if (pointer != range.storage
							&& range.begin <= offset.getValue()
							&& offset.getValue() < range.end)
					{
						overlappingArgAccesses[&instruction] = {
								range.storage, offset.getValue() - range.begin};
						break;
					}
				}
			}
		}
	}
}

/**
 * Arguments are stored into stack variables which go one after another,
 * there can be no big stack offset gaps.
 */
void CallEntry::filterLeaveOnlyContinuousStackOffsets(Config* _config)
{
	if (_config->getConfig().architecture.isX86()
			&& !possibleArgStores.empty()
			&& std::all_of(
					possibleArgStores.begin(),
					possibleArgStores.end(),
					[this](StoreInst* store)
	{
		auto instruction = AsmInstruction(store);
		auto* capstone = instruction.isValid()
				? instruction.getCapstoneInsn() : nullptr;
		return capstone != nullptr && capstone->id == X86_INS_PUSH;
	}))
	{
		// A decoded PUSH is an outgoing argument even when StackAnalysis maps
		// its destination to a source-related or otherwise discontinuous stack
		// alloca.  The native instruction order (and, when available, caller
		// cleanup) is a stronger boundary than reconstructed alloca offsets.
		return;
	}

	retdec::utils::Maybe<int> prevOff;
	auto it = possibleArgStores.begin();
	while (it != possibleArgStores.end())
	{
		auto* s = *it;
		auto off = _config->getStackVariableOffset(s->getPointerOperand());
		auto* val = llvm_utils::skipCasts(s->getValueOperand());

		int gap = 8;
//		int gap = 4;
		if (val->getType()->isFloatingPointTy())
		{
			gap = 8;
		}

		if (off.isUndefined())
		{
			++it;
			continue;
		}
		if (prevOff.isUndefined())
		{
			prevOff = off;
		}
		else if (std::abs(prevOff - off) > gap)
		{
			it = possibleArgStores.erase(it);
			continue;
		}
		else
		{
			prevOff = off;
		}

		++it;
	}
}

void CallEntry::filterLeaveOnlyNeededStackOffsets(Config* _config)
{
	int regNum = 0;
	auto it = possibleArgStores.begin();
	while (it != possibleArgStores.end())
	{
		auto* s = *it;
		auto* op = s->getPointerOperand();
		auto off = _config->getStackVariableOffset(op);

		if (_config->isRegister(op))
		{
			++regNum;
			++it;
			continue;
		}
		else if (off.isDefined())
		{
			if (_config->getConfig().architecture.isX86())
			{
				// nothing
			}
			else if (_config->getConfig().architecture.isPpc())
			{
				if (regNum == 7)
				{
					// nothing
				}
				else
				{
					it = possibleArgStores.erase(it);
					continue;
				}
			}
			else if (_config->getConfig().architecture.isArmOrThumb()
					|| _config->getConfig().architecture.isMipsOrPic32())
			{
				if (regNum == 4)
				{
					// nothing
				}
				else
				{
					it = possibleArgStores.erase(it);
					continue;
				}
			}
			else
			{
				// nothing
			}
		}

		++it;
	}
}

void CallEntry::extractFormatString(ReachingDefinitionsAnalysis& _RDA)
{
	for (auto* s : possibleArgStores)
	{
		auto* v = getRoot(_RDA, s->getValueOperand());
		auto* gv = dyn_cast_or_null<GlobalVariable>(v);

		if (gv == nullptr || !gv->hasInitializer())
		{
			continue;
		}

		auto* init = dyn_cast_or_null<ConstantDataArray>(gv->getInitializer());
		if (init == nullptr)
		{
			if (auto* i = dyn_cast<ConstantExpr>(gv->getInitializer()))
			{
				if (auto* igv = dyn_cast<GlobalVariable>(i->getOperand(0)))
				{
					init = dyn_cast_or_null<ConstantDataArray>(igv->getInitializer());
				}
			}
		}

		if (init == nullptr || !init->isString())
		{
			continue;
		}

		formatStr = init->getAsString();
		break;
	}
}

//
//=============================================================================
//  ReturnEntry
//=============================================================================
//

ReturnEntry::ReturnEntry(llvm::ReturnInst* r) :
		ret(r)
{

}

//
//=============================================================================
//  DataFlowEntry
//=============================================================================
//

DataFlowEntry::DataFlowEntry(
		llvm::Module* m,
		ReachingDefinitionsAnalysis& rda,
		Config* c,
		Abi* abi,
		FileImage* img,
		DebugFormat* dbg,
		Lti* lti,
		llvm::Value* v)
		:
		_module(m),
		_RDA(rda),
		_config(c),
		_abi(abi),
		_image(img),
		_lti(lti),
		called(v)
{
	if (auto* f = getFunction())
	{
		configFnc = c->getConfigFunction(f);
		if (dbg)
		{
			dbgFnc = dbg->getFunction(c->getFunctionAddress(f));
		}

		if (!f->empty())
		{
			addArgLoads();
			addRetStores();
		}
	}

	setTypeFromExtraInfo();
}

bool DataFlowEntry::isFunctionEntry() const
{
	return getFunction() != nullptr;
}

bool DataFlowEntry::isValueEntry() const
{
	return called && !isFunctionEntry();
}

llvm::Value* DataFlowEntry::getValue() const
{
	return called;
}

llvm::Function* DataFlowEntry::getFunction() const
{
	return dyn_cast_or_null<Function>(called);
}

void DataFlowEntry::dump() const
{
	LOG << "\n\t>|" << called->getName().str() << std::endl;
	LOG << "\t>|fnc call : " << isFunctionEntry() << std::endl;
	LOG << "\t>|val call : " << isValueEntry() << std::endl;
	LOG << "\t>|variadic : " << isVarArg << std::endl;
	LOG << "\t>|config f : " << (configFnc != nullptr) << std::endl;
	LOG << "\t>|debug f  : " << (dbgFnc != nullptr) << std::endl;
	LOG << "\t>|wrapp c  : " << llvmObjToString(wrappedCall) << std::endl;
	LOG << "\t>|type set : " << typeSet << std::endl;
	LOG << "\t>|ret type : " << llvmObjToString(retType) << std::endl;
	LOG << "\t>|arg types:" << std::endl;
	for (auto* t : argTypes)
	{
		LOG << "\t\t>|" << llvmObjToString(t) << std::endl;
	}
	LOG << "\t>|arg names:" << std::endl;
	for (auto& n : argNames)
	{
		LOG << "\t\t>|" << n << std::endl;
	}

	LOG << "\t>|calls:" << std::endl;
	for (auto& e : calls)
	{
		LOG << "\t\t>|" << llvmObjToString(e.call) << std::endl;
		LOG << "\t\t\targ stores:" << std::endl;
		for (auto* s : e.possibleArgStores)
		{
			LOG << "\t\t\t>|" << llvmObjToString(s) << std::endl;
		}
		LOG << "\t\t\tret loads:" << std::endl;
		for (auto* l : e.possibleRetLoads)
		{
			LOG << "\t\t\t>|" << llvmObjToString(l) << std::endl;
		}
		if (!e.formatStr.empty())
		{
			LOG << "\t\t\t>|format str: " << e.formatStr << std::endl;
		}
	}

	LOG << "\t>|arg loads:" << std::endl;
	for (auto* l : argLoads)
	{
		LOG << "\t\t\t>|" << llvmObjToString(l) << std::endl;
	}

	LOG << "\t>|return stores:" << std::endl;
	for (auto& e : retStores)
	{
		LOG << "\t\t>|" << llvmObjToString(e.ret) << std::endl;
		for (auto* s : e.possibleRetStores)
		{
			LOG << "\t\t\t>|" << llvmObjToString(s) << std::endl;
		}
	}
}

void DataFlowEntry::addArgLoads()
{
	auto* f = getFunction();
	if (f == nullptr)
	{
		return;
	}

	std::set<Value*> added;
	for (auto it = inst_begin(f), end = inst_end(f); it != end; ++it)
	{
		if (auto* l = dyn_cast<LoadInst>(&*it))
		{
			auto* ptr = l->getPointerOperand();
			if (!_config->isStackVariable(ptr) && !_config->isRegister(ptr))
			{
				continue;
			}
			if (_config->isFlagRegister(ptr))
			{
				continue;
			}

			auto* use = _RDA.getUse(l);
			if (use == nullptr)
			{
				continue;
			}

			if ((use->defs.empty() || use->isUndef())
					&& added.find(ptr) == added.end())
			{
				argLoads.push_back(l);
				added.insert(ptr);
			}
		}
	}
}

void DataFlowEntry::addRetStores()
{
	auto* f = getFunction();
	if (f == nullptr)
	{
		return;
	}

	for (auto it = inst_begin(f), end = inst_end(f); it != end; ++it)
	{
		if (auto* r = dyn_cast<ReturnInst>(&*it))
		{
			ReturnEntry re(r);

			NonIterableSet<BasicBlock*> seenBbs;
			NonIterableSet<Value*> disqualifiedValues;
			auto* b = r->getParent();
			seenBbs.insert(b);
			Instruction* prev = r;
			while (true)
			{
				if (prev == &b->front())
				{
					auto* spb = b->getSinglePredecessor();
					if (spb && !spb->empty() && seenBbs.hasNot(spb))
					{
						b = spb;
						prev = &b->back();
						seenBbs.insert(b);
					}
					else
					{
						break;
					}
				}
				else
				{
					prev = prev->getPrevNode();
				}
				if (prev == nullptr)
				{
					break;
				}

				if (isa<CallInst>(prev) || isa<ReturnInst>(prev))
				{
					break;
				}
				else if (auto* store = dyn_cast<StoreInst>(prev))
				{
					auto* ptr = store->getPointerOperand();

					if (disqualifiedValues.hasNot(ptr)
							&& !_config->isFlagRegister(ptr)
							&& (_config->isStackVariable(ptr) || _config->isRegister(ptr)))
					{
						re.possibleRetStores.push_back(store);
						disqualifiedValues.insert(ptr);
					}
				}
				else if (auto* load = dyn_cast<LoadInst>(prev))
				{
					auto* ptr = load->getPointerOperand();
					disqualifiedValues.insert(ptr);
				}
			}

			retStores.push_back(re);
		}
	}
}

void DataFlowEntry::addCall(llvm::CallInst* call)
{
	// Pattern:
	// bc pc   // call prantf()
	// align 4
	// prantf():
	// ...
	// Call has no args because it is stub, if we let it, it will destroy
	// all arg from all other calls.
	//
	// TODO:
	// => ignore - this is an ugly hack, solve somehow better.
	//
	if (_config->getConfig().architecture.isArmOrThumb())
	{
		if (auto ai = AsmInstruction(call))
		{
			auto* cs = ai.getCapstoneInsn();
			if ((cs->id == ARM_INS_B || cs->id == ARM_INS_BX)
					&& cs->detail->arm.op_count == 1
					&& cs->detail->arm.operands[0].type == ARM_OP_REG
					&& cs->detail->arm.operands[0].reg == ARM_REG_PC)
			{
				return;
			}
		}
	}

	CallEntry ce(call);

	addCallArgs(call, ce);
	addCallReturns(call, ce);

	calls.push_back(ce);
}

void DataFlowEntry::addCallArgs(llvm::CallInst* call, CallEntry& ce)
{
	NonIterableSet<Value*> disqualifiedValues;
	unsigned maxUsedRegNum = 0;
	Value* pendingStackPointer = nullptr;
	auto getX86CleanupBytes = [this](
			CallInst* cleanedCall,
			std::vector<StoreInst*>* cleanupMarkers) -> unsigned
	{
		if (!_config->getConfig().architecture.isX86())
		{
			return 0;
		}

		unsigned bytes = 0;
		// Prefer the machine cleanup sequence.  Optimized 32-bit MSVC code often
		// emits one POP ECX per cdecl argument instead of ADD ESP, imm.  Looking at
		// the first reconstructed ESP += 4 alone truncates a multi-argument call to
		// one argument.  Consecutive native POPs are one cleanup sequence and each
		// consumes one machine stack slot.  Result-testing instructions may be
		// scheduled between the CALL and its cleanup, so scan forward within the
		// same native basic block until the first stack-affecting instruction.
		auto callAsm = AsmInstruction(cleanedCall);
		auto* callBlock = callAsm.getBasicBlock();
		auto cleanupAsm = AsmInstruction(cleanedCall).getNext();
		unsigned skippedInstructions = 0;
		while (cleanupAsm.isValid())
		{
			auto* instruction = cleanupAsm.getCapstoneInsn();
			if (instruction == nullptr
					|| cleanupAsm.getBasicBlock() != callBlock)
			{
				break;
			}
			if (instruction->id == X86_INS_POP)
			{
				if (cleanupMarkers != nullptr)
				{
					cleanupMarkers->push_back(
							cleanupAsm.getLlvmToAsmInstruction());
				}
				unsigned slotSize =
						_config->getConfig().architecture.getByteSize();
				if (slotSize == 0
						|| slotSize > std::numeric_limits<unsigned>::max()
								- bytes)
				{
					break;
				}
				bytes += slotSize;
				cleanupAsm = cleanupAsm.getNext();
				continue;
			}
			if (instruction->id == X86_INS_ADD && instruction->detail != nullptr)
			{
				auto& x86 = instruction->detail->x86;
				if (x86.op_count == 2
						&& x86.operands[0].type == X86_OP_REG
						&& (x86.operands[0].reg == X86_REG_SP
								|| x86.operands[0].reg == X86_REG_ESP
								|| x86.operands[0].reg == X86_REG_RSP)
						&& x86.operands[1].type == X86_OP_IMM
						&& x86.operands[1].imm > 0
						&& static_cast<uint64_t>(x86.operands[1].imm)
								<= std::numeric_limits<unsigned>::max() - bytes)
				{
					bytes += static_cast<unsigned>(x86.operands[1].imm);
					if (cleanupMarkers != nullptr)
					{
						cleanupMarkers->push_back(
								cleanupAsm.getLlvmToAsmInstruction());
					}
				}
			}

			if (bytes != 0)
			{
				break;
			}

			bool stackPointerOperand = false;
			if (instruction->detail != nullptr)
			{
				auto& x86 = instruction->detail->x86;
				for (unsigned operand = 0; operand < x86.op_count; ++operand)
				{
					auto& op = x86.operands[operand];
					stackPointerOperand |= op.type == X86_OP_REG
							&& (op.reg == X86_REG_SP
									|| op.reg == X86_REG_ESP
									|| op.reg == X86_REG_RSP);
					stackPointerOperand |= op.type == X86_OP_MEM
							&& (op.mem.base == X86_REG_SP
									|| op.mem.base == X86_REG_ESP
									|| op.mem.base == X86_REG_RSP
									|| op.mem.index == X86_REG_SP
									|| op.mem.index == X86_REG_ESP
									|| op.mem.index == X86_REG_RSP);
				}
			}
			bool controlFlow = instruction->id == X86_INS_CALL
					|| instruction->id == X86_INS_LCALL
					|| instruction->id == X86_INS_JMP
					|| instruction->id == X86_INS_LJMP
					|| instruction->id == X86_INS_RET
					|| instruction->id == X86_INS_RETF
					|| instruction->id == X86_INS_RETFQ;
			if (instruction->detail != nullptr)
			{
				for (unsigned group = 0;
						group < instruction->detail->groups_count; ++group)
				{
					auto id = instruction->detail->groups[group];
					controlFlow |= id == CS_GRP_JUMP
							|| id == CS_GRP_CALL || id == CS_GRP_RET;
				}
			}
			if (stackPointerOperand || controlFlow
					|| instruction->id == X86_INS_PUSH
					|| instruction->id == X86_INS_LEAVE
					|| ++skippedInstructions > 8)
			{
				break;
			}
			cleanupAsm = cleanupAsm.getNext();
		}

		// If native instruction metadata is unavailable, recover a conventional
		// LLVM ESP restoration.  This fallback is intentionally only used when the
		// complete machine sequence above supplied no bound.
		for (auto* next = bytes == 0 ? cleanedCall->getNextNode() : nullptr;
				next != nullptr; next = next->getNextNode())
		{
			if (auto* nextCall = dyn_cast<CallInst>(next))
			{
				auto* called = nextCall->getCalledFunction();
				if (called == nullptr || !called->isIntrinsic())
				{
					break;
				}
			}
			auto* store = dyn_cast<StoreInst>(next);
			if (store == nullptr
					|| !_config->isStackPointerRegister(
							store->getPointerOperand()))
			{
				continue;
			}
			auto* stackPointer = store->getPointerOperand();
			auto* addition = dyn_cast<BinaryOperator>(
					llvm_utils::skipCasts(store->getValueOperand()));
			if (addition == nullptr || addition->getOpcode() != Instruction::Add)
			{
				continue;
			}
			for (unsigned operand = 0; operand < 2; ++operand)
			{
				auto* amount = dyn_cast<ConstantInt>(
						addition->getOperand(operand));
				auto* load = dyn_cast<LoadInst>(llvm_utils::skipCasts(
						addition->getOperand(1 - operand)));
				if (amount != nullptr && amount->getSExtValue() > 0
						&& load != nullptr
						&& load->getPointerOperand() == stackPointer)
				{
					bytes = amount->getZExtValue();
					break;
				}
			}
			if (bytes != 0)
			{
				break;
			}
		}

		return bytes;
	};

	unsigned cleanupBytes = getX86CleanupBytes(
			call, &ce.obsoleteStackCleanupMarkers);
	unsigned nestedCleanupBytes = 0;
	uint64_t recoveredStackBytes = 0;
	uint64_t overwrittenReservationBytes = 0;
	std::set<StoreInst*> provenStackArgStores;
	AsmInstruction overwrittenReservationAsm;
	unsigned x86SlotSize = _config->getConfig().architecture.isX86()
			? _config->getConfig().architecture.getByteSize() : 0;
	auto* b = call->getParent();
	Instruction* prev = call;
	std::set<BasicBlock*> seen;
	seen.insert(b);
	while (true)
	{
		if (prev == &b->front())
		{
			auto* spb = b->getSinglePredecessor();
			if (spb && !spb->empty() && spb != b && seen.count(spb) == 0)
			{
				b = spb;
				prev = &b->back();
				seen.insert(b);
			}
			else
			{
				break;
			}
		}
		else
		{
			prev = prev->getPrevNode();
		}
		if (prev == nullptr)
		{
			break;
		}
		if (overwrittenReservationBytes != 0
				&& overwrittenReservationAsm.isValid())
		{
			AsmInstruction currentAsm(prev);
			if (currentAsm.isValid()
					&& currentAsm.getLlvmToAsmInstruction()
							!= overwrittenReservationAsm.getLlvmToAsmInstruction())
			{
				auto* currentCapstone = currentAsm.getCapstoneInsn();
				bool isX87ValueProducer = currentCapstone != nullptr
						&& (currentCapstone->id == X86_INS_FLD
								|| currentCapstone->id == X86_INS_FLD1
								|| currentCapstone->id == X86_INS_FLDZ
								|| currentCapstone->id == X86_INS_FLDPI
								|| currentCapstone->id == X86_INS_FLDL2E
								|| currentCapstone->id == X86_INS_FLDL2T
								|| currentCapstone->id == X86_INS_FLDLG2
								|| currentCapstone->id == X86_INS_FLDLN2);
				if (currentCapstone == nullptr
						|| (currentCapstone->id != X86_INS_PUSH
								&& !isX87ValueProducer)
						|| currentAsm.getEndAddress()
								!= overwrittenReservationAsm.getAddress())
				{
					overwrittenReservationBytes = 0;
					overwrittenReservationAsm = AsmInstruction();
				}
				else if (isX87ValueProducer)
				{
					// PUSH/PUSH may reserve the QWORD before FLD/FLDZ computes
					// the value eventually stored by FSTP [ESP].  Preserve the
					// adjacency chain without consuming reservation bytes.
					overwrittenReservationAsm = currentAsm;
				}
			}
		}

		if (auto* previousCall = dyn_cast<CallInst>(prev))
		{
			overwrittenReservationBytes = 0;
			overwrittenReservationAsm = AsmInstruction();
			auto* calledFnc = previousCall->getCalledFunction();
			if (calledFnc == nullptr || !calledFnc->isIntrinsic())
			{
				// An outer cdecl call may prepare one of its arguments by calling a
				// helper, then resume pushing the remaining outer arguments.  Cross
				// such a helper only when its own caller cleanup proves exactly how
				// many intervening pushes belong to it.  Those pushes are skipped
				// below; the outer scan remains bounded by its independent cleanup.
				unsigned nestedBytes = getX86CleanupBytes(previousCall, nullptr);
				if (!_config->getConfig().architecture.isX86_32()
						|| cleanupBytes == 0 || x86SlotSize == 0
						|| nestedBytes == 0 || nestedBytes % x86SlotSize != 0
						|| nestedBytes > std::numeric_limits<unsigned>::max()
								- nestedCleanupBytes)
				{
					break;
				}
				nestedCleanupBytes += nestedBytes;
				pendingStackPointer = nullptr;
				continue;
			}
		}
		else if (auto* store = dyn_cast<StoreInst>(prev))
		{
			auto* val = store->getValueOperand();
			auto* ptr = store->getPointerOperand();
			bool isUnresolvedX86Push = false;
			bool isUnresolvedX86StackBlock = false;
			bool isDecodedX86Push = false;

			// A decoded x86 push is represented by two stores:
			//
			//   store argument, inttoptr(new_sp)
			//   store new_sp, esp
			//
			// StackAnalysis normally replaces the first destination with a stack
			// alloca. If ESP is loop-carried, however, new_sp is not reducible to
			// an absolute offset and the destination stays dynamic. We walk
			// backwards, so remember the ESP update and associate it with the
			// preceding store through the same (possibly cast) value.
			if (_config->getConfig().architecture.isX86()
					&& _config->isStackPointerRegister(ptr))
			{
				pendingStackPointer = llvm_utils::skipCasts(val);
			}
			else if (pendingStackPointer != nullptr)
			{
				isUnresolvedX86Push =
						llvm_utils::skipCasts(ptr) == pendingStackPointer;
				pendingStackPointer = nullptr;
			}
			auto asmInstruction = AsmInstruction(store);
			auto* capstone = asmInstruction.isValid()
					? asmInstruction.getCapstoneInsn() : nullptr;
			isDecodedX86Push = capstone != nullptr
					&& capstone->id == X86_INS_PUSH;
			bool isDecodedX86WideStackStore = capstone != nullptr
					&& (capstone->id == X86_INS_FST
							|| capstone->id == X86_INS_FSTP)
					&& _config->isStackVariable(ptr)
					&& val->getType()->isSized()
					&& x86SlotSize != 0
					&& _module->getDataLayout().getTypeStoreSize(
							val->getType()) > x86SlotSize;
			// StackAnalysis can fold away the ESP update of an otherwise decoded
			// PUSH while leaving its memory store at a runtime ESP-derived address.
			// Machine metadata plus the exact caller cleanup still proves that this
			// store is an outgoing stack argument.  Treat it like the paired form so
			// applyToIr() consumes the pushed SSA value instead of synthetic memory.
			if (isDecodedX86Push && cleanupBytes != 0
					&& !AsmInstruction::isLlvmToAsmInstruction(store)
					&& !_config->isStackPointerRegister(ptr)
					&& !_config->isStackVariable(ptr)
					&& !_config->isRegister(ptr))
			{
				isUnresolvedX86Push = true;
			}

			// Compilers pass x87 values by reserving a block with SUB ESP and
			// storing directly through the resulting current ESP.  There is no
			// PUSH-like store back to ESP for addCallArgs() to pair with.  A matching
			// caller cleanup proves that this current-SP store is outgoing argument
			// storage; consume its SSA value directly just like a recovered push.
			if (!isDecodedX86Push && !isUnresolvedX86Push
					&& _config->getConfig().architecture.isX86()
					&& cleanupBytes != 0 && val->getType()->isSized())
			{
				auto* address = llvm_utils::skipCasts(ptr);
				auto* spLoad = dyn_cast<LoadInst>(address);
				isUnresolvedX86StackBlock = spLoad != nullptr
						&& _config->isStackPointerRegister(
								spLoad->getPointerOperand())
						&& _module->getDataLayout().getTypeStoreSize(
								val->getType()) <= cleanupBytes;
			}

			// Process reservation PUSHes before the usual duplicate-destination
			// filtering.  StackAnalysis commonly maps the last PUSH and the
			// overwriting FSTP to the same alloca, which intentionally disqualifies
			// the PUSH as an argument but must still consume its reservation bytes.
			if (overwrittenReservationBytes != 0 && isDecodedX86Push
					&& !AsmInstruction::isLlvmToAsmInstruction(store)
					&& !_config->isStackPointerRegister(ptr)
					&& (isUnresolvedX86Push || _config->isStackVariable(ptr)))
			{
				if (x86SlotSize > overwrittenReservationBytes)
				{
					break;
				}
				overwrittenReservationBytes -= x86SlotSize;
				overwrittenReservationAsm = asmInstruction;
				continue;
			}

			if (!isUnresolvedX86Push && !isUnresolvedX86StackBlock
					&& !_config->isStackVariable(ptr)
					&& !_config->isRegister(ptr))
			{
				disqualifiedValues.insert(ptr);
			}

			if (auto* l = dyn_cast<LoadInst>(val))
			{
				if (l->getPointerOperand()->getName() == "ebp"
						|| l->getPointerOperand()->getName() == "rbp")
				{
					// Saving EBP in a prologue is not a call argument, but EBP is
					// also an ordinary value that native code may explicitly PUSH.
					// Exact caller cleanup plus the decoded/paired PUSH proves the
					// latter case and must override the prologue heuristic.
					if (cleanupBytes == 0
							|| (!isDecodedX86Push && !isUnresolvedX86Push))
					{
						disqualifiedValues.insert(ptr);
					}
				}
				if (_config->isRegister(ptr)
						&& _config->isRegister(l->getPointerOperand())
						&& ptr != l->getPointerOperand())
				{
					disqualifiedValues.insert(l->getPointerOperand());
				}
			}

			if (disqualifiedValues.hasNot(ptr)
					&& !_config->isFlagRegister(ptr)
					&& (isa<AllocaInst>(ptr)
							|| _config->isRegister(ptr)
							|| isUnresolvedX86Push
							|| isUnresolvedX86StackBlock))
			{
				uint64_t stackBytes = 0;
				bool isStackArgument = _config->isStackVariable(ptr)
						|| isUnresolvedX86Push || isUnresolvedX86StackBlock;
				bool hasDecodedStackEffect = isDecodedX86Push
						|| isUnresolvedX86Push || isUnresolvedX86StackBlock
						|| isDecodedX86WideStackStore;
				if (isStackArgument && hasDecodedStackEffect && x86SlotSize != 0)
				{
					uint64_t valueBytes = val->getType()->isSized()
							? _module->getDataLayout().getTypeStoreSize(
									val->getType()) : x86SlotSize;
					stackBytes = isDecodedX86Push || isUnresolvedX86Push
							? x86SlotSize
							: std::max<uint64_t>(
									x86SlotSize,
									((valueBytes + x86SlotSize - 1) / x86SlotSize)
											* x86SlotSize);
				}

				// A common x87 cdecl sequence reserves a QWORD with two dummy
				// PUSHes and then overwrites both slots with FSTP QWORD [ESP].  The
				// wide current-SP store is the argument; the earlier PUSH stores are
				// only reservation mechanics and must not become phantom scalar args.
				// Walking backwards encounters the wide store first, so remember how
				// many immediately preceding decoded push bytes it covers.
				if ((isUnresolvedX86StackBlock
						|| isDecodedX86WideStackStore) && stackBytes != 0)
				{
					overwrittenReservationBytes += stackBytes;
					overwrittenReservationAsm = asmInstruction;
				}
				else if (overwrittenReservationBytes != 0
						&& isStackArgument && !hasDecodedStackEffect)
				{
					overwrittenReservationBytes = 0;
					overwrittenReservationAsm = AsmInstruction();
				}

				if (nestedCleanupBytes != 0)
				{
					if (stackBytes != 0)
					{
						if (stackBytes > nestedCleanupBytes)
						{
							break;
						}
						nestedCleanupBytes -= stackBytes;
					}
					continue;
				}

				ce.possibleArgStores.push_back(store);
				if (isStackArgument && hasDecodedStackEffect)
				{
					provenStackArgStores.insert(store);
				}
				if ((isUnresolvedX86Push || isUnresolvedX86StackBlock)
						&& !_config->isStackVariable(ptr))
				{
					ce.unresolvedStackArgStores.insert(store);
				}
				disqualifiedValues.insert(ptr);
				disqualifiedValues.insert(store);

				if (_config->isGeneralPurposeRegister(ptr)
						|| _config->isFloatingPointRegister(ptr))
				{
					auto rn = _config->getConfigRegisterNumber(ptr);
					if (rn.isDefined() && rn > maxUsedRegNum)
					{
						maxUsedRegNum = rn;
					}
				}

				if (stackBytes != 0)
				{
					recoveredStackBytes += stackBytes;
					if (cleanupBytes != 0
							&& recoveredStackBytes >= cleanupBytes)
					{
						// Multiple cdecl arguments can be cleaned with adjacent POPs
						// that collapse to a single reconstructed cleanup operation.
						// A contiguous native PUSH immediately before the current PUSH
						// is still unambiguously part of this call's argument block, so
						// continue through the complete PUSH chain before applying the
						// possibly truncated cleanup bound.
						auto previousAsm = asmInstruction.getPrev();
						auto* previousCapstone = previousAsm.isValid()
								? previousAsm.getCapstoneInsn() : nullptr;
						bool hasAdjacentPush = isDecodedX86Push
								&& previousCapstone != nullptr
								&& previousCapstone->id == X86_INS_PUSH
								&& previousAsm.getEndAddress()
										== asmInstruction.getAddress();
						if (!hasAdjacentPush)
						{
							break;
						}
					}
				}
			}
		}
	}

	// A caller-side ESP adjustment gives an exact upper bound on the byte width
	// of x86 stack arguments. Prefer stores with decoded stack effects before
	// filling the remaining bound with uncertain stack-variable stores. An x87
	// temporary store may be interleaved between real PUSHes; consuming cleanup
	// capacity in encounter order would keep that local and discard an earlier
	// genuine argument. Count rounded byte widths because a QWORD occupies two
	// i386 slots.
	if (_config->getConfig().architecture.isX86())
	{
		unsigned slotSize = _config->getConfig().architecture.getByteSize();
		if (cleanupBytes != 0 && slotSize != 0)
		{
			auto getCandidateBytes = [&](StoreInst* store) -> uint64_t
			{
				if (!ce.isStackArgumentStore(_config, store))
				{
					return 0;
				}
				auto instruction = AsmInstruction(store);
				auto* capstone = instruction.isValid()
						? instruction.getCapstoneInsn() : nullptr;
				uint64_t valueBytes = capstone != nullptr
							&& capstone->id == X86_INS_PUSH
						? slotSize
						: _module->getDataLayout().getTypeStoreSize(
								store->getValueOperand()->getType());
				return std::max<uint64_t>(
						slotSize,
						((valueBytes + slotSize - 1) / slotSize) * slotSize);
			};

			uint64_t provenBytes = 0;
			for (auto* store : ce.possibleArgStores)
			{
				if (provenStackArgStores.count(store) != 0)
				{
					provenBytes += getCandidateBytes(store);
				}
			}
			uint64_t uncertainBytes = 0;
			uint64_t uncertainLimit = provenBytes < cleanupBytes
					? cleanupBytes - provenBytes : 0;
			auto it = ce.possibleArgStores.begin();
			while (it != ce.possibleArgStores.end())
			{
				auto* store = *it;
				uint64_t candidateBytes = getCandidateBytes(store);
				bool isProven = provenStackArgStores.count(store) != 0;
				if (!isProven && candidateBytes != 0
						&& uncertainBytes + candidateBytes > uncertainLimit)
				{
					ce.unresolvedStackArgStores.erase(store);
					it = ce.possibleArgStores.erase(it);
				}
				else
				{
					if (!isProven)
					{
						uncertainBytes += candidateBytes;
					}
					++it;
				}
			}
		}
	}
}

void DataFlowEntry::addCallReturns(llvm::CallInst* call, CallEntry& ce)
{
	NonIterableSet<Value*> disqualifiedValues;
	auto* b = call->getParent();
	Instruction* next = call;
	std::set<BasicBlock*> seen;
	seen.insert(b);
	while (true)
	{
		if (next == &b->back())
		{
			auto* ssb = b->getSingleSuccessor();
			if (ssb && !ssb->empty() && ssb != b && seen.count(ssb) == 0)
			{
				b = ssb;
				next = &b->front();
				seen.insert(b);
			}
			else
			{
				break;
			}
		}
		else
		{
			next = next->getNextNode();
		}
		if (next == nullptr)
		{
			break;
		}

		if (auto* call = dyn_cast<CallInst>(next))
		{
			auto* calledFnc = call->getCalledFunction();
			if (calledFnc == nullptr || !calledFnc->isIntrinsic())
			{
				break;
			}
		}
		else if (auto* store = dyn_cast<StoreInst>(next))
		{
			auto* ptr = store->getPointerOperand();
			disqualifiedValues.insert(ptr);
		}
		else if (auto* load = dyn_cast<LoadInst>(next))
		{
			auto* ptr = load->getPointerOperand();

			if (disqualifiedValues.hasNot(ptr)
					&& !_config->isFlagRegister(ptr)
					&& (_config->isStackVariable(ptr) || _config->isRegister(ptr)))
			{
				ce.possibleRetLoads.push_back(load);
				disqualifiedValues.insert(ptr);
			}
		}
	}
}

void DataFlowEntry::filter()
{
	if (!isVarArg)
	{
		callsFilterCommonRegisters();
	}

	filterRegistersArgLoads();
	filterSortArgLoads();

	for (CallEntry& e : calls)
	{
		e.filterRegisters(_config);
		e.filterSort(_config);
		e.filterLeaveOnlyContinuousStackOffsets(_config);
		e.filterLeaveOnlyNeededStackOffsets(_config);

		if (isVarArg)
		{
			e.extractFormatString(_RDA);
		}
	}

	if (!isVarArg)
	{
		callsFilterSameNumberOfStacks();
	}

	if (typeSet)
	{
		for (CallEntry& e : calls)
		{
			auto tIt = argTypes.begin();
			auto sIt = e.possibleArgStores.begin();

			while (tIt != argTypes.end() && sIt != e.possibleArgStores.end())
			{
				Type* t = *tIt;
				auto nextIt = sIt;
				++nextIt;
				if (t->isDoubleTy()
						&& nextIt != e.possibleArgStores.end()
						&& _config->isRegister((*nextIt)->getPointerOperand()))
				{
					e.possibleArgStores.erase(nextIt);
				}

				++tIt;
				++sIt;
			}
		}
	}
	else
	{
		if (_config->getConfig().architecture.isArmOrThumb())
		{
			static std::vector<std::string> armNames =
					{"r0", "r1", "r2", "r3"};

			for (CallEntry& e : calls)
			{
				std::size_t idx = 0;
				auto sIt = e.possibleArgStores.begin();
				while (sIt != e.possibleArgStores.end() && idx < armNames.size())
				{
					StoreInst* s = *sIt;
					if (s->getPointerOperand()->getName() != armNames[idx])
					{
						e.possibleArgStores.erase(sIt, e.possibleArgStores.end());
						break;
					}

					++sIt;
					++idx;
				}
			}
		}
	}

	setTypeFromUseContext();
}

void DataFlowEntry::callsFilterCommonRegisters()
{
	if (calls.empty())
	{
		return;
	}

	std::set<Value*> commonRegs;

	for (auto* s : calls.front().possibleArgStores)
	{
		Value* r = s->getPointerOperand();
		if (_config->isRegister(r))
		{
			commonRegs.insert(r);
		}
	}

	for (auto& e : calls)
	{
		// TODO: sometimes, we do not find all arg stores.
		// this is a hack, we should manufacture loads even if we do not have
		// stores but know there are some arguments (debug, ...).
		if (e.possibleArgStores.empty())
		{
			continue;
		}

		std::set<Value*> regs;
		for (auto* s : e.possibleArgStores)
		{
			Value* r = s->getPointerOperand();
			if (_config->isRegister(r))
			{
				regs.insert(r);
			}
		}

		std::set<Value*> intersect;
		std::set_intersection(
				commonRegs.begin(),
				commonRegs.end(),
				regs.begin(),
				regs.end(),
				std::inserter(intersect, intersect.begin()));
		commonRegs = std::move(intersect);
	}

	// If common contains r3, then it should also contain r2, r1, and r0.
	// Example for MIPS: if contains a2, it should a1 and a0.
	//
	static std::vector<std::string> regNames;
	if (regNames.empty())
	{
		if (_config->getConfig().architecture.isMipsOrPic32())
		{
			if (_config->getConfig().tools.isPspGcc())
			{
				regNames = {"a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3"};
			}
			else
			{
				regNames = {"a0", "a1", "a2", "a3"};
			}
		}
		else if (_config->getConfig().architecture.isArmOrThumb())
		{
			regNames = {"r0", "r1", "r2", "r3"};
		}
		else if (_config->getConfig().architecture.isPpc())
		{
			regNames = {"r3", "r4", "r5", "r6", "r7", "r8", "r9"};
		}
	}
	for (auto it = regNames.rbegin(); it != regNames.rend(); ++it)
	{
		auto* r = _config->getLlvmRegister(*it);
		if (commonRegs.count(r))
		{
			++it;
			while (it != regNames.rend())
			{
				r = _config->getLlvmRegister(*it);
				commonRegs.insert(r);
				++it;
			}
			break;
		}
	}

	for (auto& e : calls)
	{
		auto it = e.possibleArgStores.begin();
		for ( ; it != e.possibleArgStores.end(); )
		{
			Value* r = (*it)->getPointerOperand();
			if (_config->isRegister(r)
					&& commonRegs.find(r) == commonRegs.end())
			{
				it = e.possibleArgStores.erase(it);
			}
			else
			{
				++it;
			}
		}
	}
}

void DataFlowEntry::callsFilterSameNumberOfStacks()
{
	if (calls.empty())
	{
		return;
	}

	std::size_t loads = 0;
	for (auto* l : argLoads)
	{
		if (_config->isStackVariable(l->getPointerOperand()))
		{
			++loads;
		}
	}

	std::size_t stacks = std::numeric_limits<std::size_t>::max();
	for (auto& ce : calls)
	{
		std::size_t ss = 0;
		for (auto* s : ce.possibleArgStores)
		{
			if (ce.isStackArgumentStore(_config, s))
			{
				++ss;
			}
		}

		// TODO: all but one have 2 params, receiving have 2 params, one has zero.
		//
		if (ss < stacks && ss != 0 && ss >= loads)
		{
			stacks = ss;
		}
	}
	if (typeSet && stacks < argTypes.size())
	{
		stacks = argTypes.size();
	}

	for (auto& ce : calls)
	{
		std::size_t cntr = 0;
		auto it = ce.possibleArgStores.begin();
		while (it != ce.possibleArgStores.end())
		{
			auto* s = *it;
			if (!ce.isStackArgumentStore(_config, s))
			{
				++it;
				continue;
			}

			++cntr;
			if (cntr > stacks)
			{
				it = ce.possibleArgStores.erase(it);
			}
			else
			{
				++it;
			}
		}
	}
}

void DataFlowEntry::applyToIr()
{
	if (isVarArg)
	{
		applyToIrVariadic();
	}
	else
	{
		applyToIrOrdinary();
	}
}

void DataFlowEntry::applyToIrOrdinary()
{
	if (Function* fnc = getFunction())
	{
		if (fnc->arg_size() > 0)
		{
			return;
		}

		// Make contained/overlapping incoming stack views read the canonical
		// argument object before LocalVars replaces undefined alloca loads.  This
		// preserves exact byte aliasing (for example, a word at byte six of a
		// double argument) without inventing a second ABI parameter.
		for (const auto& alias : overlappingArgAccesses)
		{
			auto* access = alias.first;
			auto* storage = alias.second.first;
			auto offset = alias.second.second;
			auto* bytePtr = new BitCastInst(
					storage,
					Type::getInt8PtrTy(_module->getContext()),
					"",
					access);
			auto* adjusted = GetElementPtrInst::Create(
					Type::getInt8Ty(_module->getContext()),
					bytePtr,
					ConstantInt::get(
							Type::getInt32Ty(_module->getContext()), offset),
					"",
					access);
			Value* pointer = nullptr;
			if (auto* load = dyn_cast<LoadInst>(access))
			{
				pointer = load->getPointerOperand();
			}
			else if (auto* store = dyn_cast<StoreInst>(access))
			{
				pointer = store->getPointerOperand();
			}
			assert(pointer != nullptr);
			auto* typed = new BitCastInst(
					adjusted,
					pointer->getType(),
					"",
					access);
			if (auto* load = dyn_cast<LoadInst>(access))
			{
				load->setOperand(0, typed);
			}
			else
			{
				cast<StoreInst>(access)->setOperand(1, typed);
			}
		}

		std::map<llvm::CallInst*, std::vector<llvm::Value*>> calls2vals;
		std::set<llvm::StoreInst*> recoveredPushStores;
		for (auto& e : calls)
		{
			std::vector<Value*> loads;
			auto* call = e.call;
			for (auto* s : e.possibleArgStores)
			{
				auto fIt = specialArgStorage.find(loads.size());
				while (fIt != specialArgStorage.end())
				{
					auto* sl = new LoadInst(fIt->second, "", call);
					loads.push_back(sl);
					fIt = specialArgStorage.find(loads.size());
				}

				if (e.unresolvedStackArgStores.count(s) != 0)
				{
					// The store is the decoded memory side of an x86 push whose
					// loop-carried/current ESP address StackAnalysis could not turn
					// into an alloca.  Its SSA value is the exact argument; loading
					// it back through emulated ESP leaves native lifts dependent on
					// an invalid synthetic stack address.
					loads.push_back(s->getValueOperand());
					recoveredPushStores.insert(s);
				}
				else
				{
					auto* l = new LoadInst(s->getPointerOperand(), "", call);
					loads.push_back(l);
				}
			}

			calls2vals[call] = loads;
		}

		llvm::Value* retVal = nullptr;
		std::map<ReturnInst*, Value*> rets2vals;
		if (_config->getConfig().architecture.isX86())
		{
			if (retType->isFloatingPointTy())
			{
				retVal = _config->getLlvmRegister("st7");
			}
			else if (_config->getConfig().architecture.isX86_32())
			{
				retVal = _config->getLlvmRegister("eax");
			}
			else if (_config->getConfig().architecture.isX86_64())
			{
				retVal = _config->getLlvmRegister("rax");
			}
		}
		else if (_config->getConfig().architecture.isMipsOrPic32())
		{
			retVal = _config->getLlvmRegister("v0");
		}
		else if (_config->getConfig().architecture.isArmOrThumb())
		{
			retVal = _config->getLlvmRegister("r0");
		}
		else if (_config->getConfig().architecture.isPpc())
		{
			retVal = _config->getLlvmRegister("r3");
		}
		if (retVal)
		{
			for (auto& e : retStores)
			{
				auto* l = new LoadInst(retVal, "", e.ret);
				rets2vals[e.ret] = l;
			}
		}

		std::vector<llvm::Value*> argStores;
		if (_config->getConfig().architecture.isX86_32())
		{
			// Cdecl stack offsets are ABI positions, not a compact list of only
			// those parameters used by the callee.  Preserve holes so a used third
			// argument at [ebp+16] remains arg3 even when [ebp+12] is never read.
			argStores.resize(argTypes.size(), nullptr);
			uint64_t slotSize =
					_config->getConfig().architecture.getByteSize();
			int64_t offset = slotSize;
			std::size_t argument = 0;
			for (auto* load : argLoads)
			{
				auto loadOffset = _config->getStackVariableOffset(
						load->getPointerOperand());
				while (loadOffset.isDefined() && argument < argTypes.size()
						&& offset < loadOffset.getValue())
				{
					uint64_t size = _module->getDataLayout().getTypeStoreSize(
							argTypes[argument]);
					offset += std::max<uint64_t>(
							slotSize, ((size + slotSize - 1) / slotSize) * slotSize);
					++argument;
				}
				if (loadOffset.isDefined() && argument < argStores.size()
						&& offset == loadOffset.getValue())
				{
					argStores[argument] = load->getPointerOperand();
					uint64_t size = _module->getDataLayout().getTypeStoreSize(
							argTypes[argument]);
					offset += std::max<uint64_t>(
							slotSize, ((size + slotSize - 1) / slotSize) * slotSize);
					++argument;
				}
			}
		}
		else
		{
			for (LoadInst* l : argLoads)
			{
				auto fIt = specialArgStorage.find(argStores.size());
				while (fIt != specialArgStorage.end())
				{
					argStores.push_back(fIt->second);
					fIt = specialArgStorage.find(argStores.size());
				}

				argStores.push_back(l->getPointerOperand());
			}
		}

		if (_config->getConfig().architecture.isX86_32()
				&& !fnc->isDeclaration())
		{
			// A recovered cdecl signature describes physical DWORD slots in the
			// incoming stack frame.  Materialize every parameter with its recovered
			// ABI width, even when an earlier machine-word access created a narrower
			// alloca at that offset.  Otherwise a double argument whose address is
			// passed to another routine receives only its low DWORD.  Do not leave an
			// unused parameter as a hole either: a wider view can span adjacent slots.
			// StackFrameCoalescing subsequently puts these objects back into one
			// byte-addressable frame.
			IrModifier modifier(_module, _config);
			uint64_t slotSize =
					_config->getConfig().architecture.getByteSize();
			int64_t offset = slotSize;
			for (std::size_t argument = 0;
					argument < argStores.size(); ++argument)
			{
				argStores[argument] = modifier.getStackVariable(
						fnc, offset, argTypes[argument]).first;
				uint64_t size = _module->getDataLayout().getTypeStoreSize(
						argTypes[argument]);
				offset += std::max<uint64_t>(
						slotSize,
						((size + slotSize - 1) / slotSize) * slotSize);
			}
		}

		static std::vector<std::string> ppcNames =
				{"r3", "r4", "r5", "r6", "r7", "r8", "r9"};
		static std::vector<std::string> armNames =
				{"r0", "r1", "r2", "r3"};
		static std::vector<std::string> mipsNames =
				{"a0", "a1", "a2", "a3"};
		if (_config->getConfig().tools.isPspGcc())
		{
			mipsNames = {"a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3"};
		}
		for (auto& p : calls2vals)
		{
			retdec::utils::Maybe<int> stackOff;
			if (_config->getConfig().architecture.isX86())
			{
				if (!p.second.empty())
				{
					auto* l = dyn_cast<LoadInst>(p.second.back());
					if (l != nullptr
							&& _config->isStackVariable(l->getPointerOperand()))
					{
						stackOff = _config->getStackVariableOffset(l->getPointerOperand());
						stackOff = stackOff + 4;
					}
				}

				if (stackOff.isUndefined())
				{
					AsmInstruction ai(p.first);
					while (ai.isValid())
					{
						for (auto& i : ai)
						{
							if (auto* s = dyn_cast<StoreInst>(&i))
							{
								if (_config->isStackVariable(s->getPointerOperand()))
								{
									stackOff = _config->getStackVariableOffset(s->getPointerOperand());
									break;
								}
							}
						}
						if (stackOff.isDefined())
						{
							break;
						}
						ai = ai.getPrev();
					}
				}
			}

			std::size_t idx = 0;
			for (auto* t : argTypes)
			{
				(void) t;
				if (p.second.size() <= idx)
				{
					if (_config->getConfig().architecture.isArmOrThumb())
					{
						if (idx < armNames.size())
						{
							auto* r = _config->getLlvmRegister(armNames[idx]);
							auto* l = new LoadInst(r, "", p.first);
							p.second.push_back(l);
						}
					}
					else if (_config->getConfig().architecture.isMipsOrPic32())
					{
						if (idx < mipsNames.size())
						{
							auto* r = _config->getLlvmRegister(mipsNames[idx]);
							auto* l = new LoadInst(r, "", p.first);
							p.second.push_back(l);
						}
					}
					else if (_config->getConfig().architecture.isPpc())
					{
						if (idx < ppcNames.size())
						{
							auto* r = _config->getLlvmRegister(ppcNames[idx]);
							auto* l = new LoadInst(r, "", p.first);
							p.second.push_back(l);
						}
					}
					else if (_config->getConfig().architecture.isX86()
							&& stackOff.isDefined())
					{
						auto* s = _config->getLlvmStackVariable(p.first->getFunction(), stackOff);
						if (s)
						{
							auto* l = new LoadInst(s, "", p.first);
							p.second.push_back(l);
							stackOff = stackOff + 4;
						}
						else
						{
							stackOff.setUndefined();
						}
					}
				}
				++idx;
			}
		}

		auto* oldType = fnc->getType();
		IrModifier irm(_module, _config);
		auto* newFnc = irm.modifyFunction(
				fnc,
				retType,
				argTypes,
				isVarArg,
				rets2vals,
				calls2vals,
				retVal,
				argStores,
				argNames).first;

		// Once a decoded x86 call consumes recovered SSA arguments directly,
		// its compiler-generated ADD ESP / POP cleanup must not execute against
		// the module-global synthetic stack pointer.  Remove only machine regions
		// that addCallArgs() already proved to be caller cleanup.
		for (auto& entry : calls)
		{
			if (entry.possibleArgStores.empty()
					&& entry.unresolvedStackArgStores.empty())
			{
				continue;
			}
			for (auto* marker : entry.obsoleteStackCleanupMarkers)
			{
				if (marker->getParent() != nullptr)
				{
					AsmInstruction(marker).eraseInstructions();
				}
			}
		}

		// The recovered call now consumes each pushed SSA value directly.  Keep
		// neither the synthetic memory write nor its potentially invalid ESP-based
		// destination live in the retargeted program.
		for (auto* store : recoveredPushStores)
		{
			if (store->getParent() != nullptr)
			{
				store->eraseFromParent();
			}
		}

		LOG << "modify fnc: " << newFnc->getName().str() << " = "
				<< llvmObjToString(oldType) << " -> "
				<< llvmObjToString(newFnc->getType()) << std::endl;

		called = newFnc;
	}
	else
	{
		for (auto& e : calls)
		{
			auto* call = e.call;
			LOG << "\tmodify call: " << llvmObjToString(call) << std::endl;

			std::vector<Value*> loads;
			std::vector<StoreInst*> recoveredPushStores;
			for (auto* s : e.possibleArgStores)
			{
				if (e.unresolvedStackArgStores.count(s) != 0)
				{
					loads.push_back(s->getValueOperand());
					recoveredPushStores.push_back(s);
				}
				else
				{
					auto* l = new LoadInst(s->getPointerOperand(), "", call);
					loads.push_back(l);
					LOG << "\t\t" << llvmObjToString(l) << std::endl;
				}
			}

			auto* ret = fnc ? fnc->getReturnType() : call->getType();
			IrModifier::modifyCallInst(call, ret, loads);
			for (auto* store : recoveredPushStores)
			{
				if (store->getParent() != nullptr)
				{
					store->eraseFromParent();
				}
			}
		}
	}
}

void DataFlowEntry::applyToIrVariadic()
{
	std::vector<std::string> ppcNames =
			{"r3", "r4", "r5", "r6", "r7", "r8", "r9"};
	std::vector<std::string> armNames =
			{"r0", "r1", "r2", "r3"};
	std::vector<std::string> mipsNames =
			{"a0", "a1", "a2", "a3"};
	if (_config->getConfig().tools.isPspGcc())
	{
		mipsNames = {"a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3"};
	}

	std::vector<std::string> mipsFpNames =
			{"fd12", "fd13", "fd14", "fd15", "fd16", "fd17", "fd18", "fd19", "fd20"};

	llvm::Value* retVal = nullptr;
	std::map<ReturnInst*, Value*> rets2vals;
	std::vector<llvm::Value*> argStores;
	std::map<llvm::CallInst*, std::vector<llvm::Value*>> calls2vals;

	for (CallEntry& ce : calls)
	{
		auto* fnc = ce.call->getFunction();
		auto* calledFnc = ce.call->getCalledFunction();

		LOG << llvmObjToString(ce.call) << std::endl;
		LOG << "\tformat : " << ce.formatStr << std::endl;

		// get lowest stack offset
		//
		int stackOff = std::numeric_limits<int>::max();
		for (StoreInst* s : ce.possibleArgStores)
		{
			if (_config->isStackVariable(s->getPointerOperand()))
			{
				auto so = _config->getStackVariableOffset(s->getPointerOperand());
				if (so < stackOff)
				{
					stackOff = so;
				}
			}
		}
		LOG << "\tlowest : " << std::dec << stackOff << std::endl;

		//
		//
		auto* wrapCall = isSimpleWrapper(calledFnc);
		auto* wrapFnc = wrapCall ? wrapCall->getCalledFunction() : calledFnc;
		std::vector<llvm::Type*> ttypes = llvm_utils::parseFormatString(
				_module,
				ce.formatStr,
				wrapFnc);

		if (_config->getConfig().architecture.isPic32())
		{
			for (size_t i = 0; i < ttypes.size(); ++i)
			{
				if (ttypes[i]->isDoubleTy())
				{
					ttypes[i] = Type::getFloatTy(_module->getContext());
				}
			}
		}

		//
		//
		int off = stackOff;
		std::vector<Value*> args;

		size_t faIdx = 0;
		size_t aIdx = 0;

		std::vector<llvm::Type*> types = argTypes;
		types.insert(types.end(), ttypes.begin(), ttypes.end());

		for (Type* t : types)
		{
			LOG << "\ttype : " << llvmObjToString(t) << std::endl;
			int sz = static_cast<int>(_abi->getTypeByteSize(t));
			sz = sz > 4 ? 8 : 4;

			if (_config->getConfig().architecture.isX86())
			{
				auto* st = _config->getLlvmStackVariable(fnc, off);
				if (st)
				{
					args.push_back(st);
				}

				off += sz;
			}
			else if (_config->getConfig().architecture.isPpc())
			{
				if (aIdx < ppcNames.size())
				{
					auto* r = _module->getNamedGlobal(ppcNames[aIdx]);
					if (r)
					{
						args.push_back(r);
					}
				}
				else
				{
					auto* st = _config->getLlvmStackVariable(fnc, off);
					if (st)
					{
						args.push_back(st);
					}

					off += sz;
				}
			}
			else if (_config->getConfig().architecture.isArmOrThumb())
			{
				if (aIdx < armNames.size())
				{
					auto* r = _module->getNamedGlobal(armNames[aIdx]);
					if (r)
					{
						args.push_back(r);
					}

					if (sz > 4)
					{
						++aIdx;
					}
				}
				else
				{
					auto* st = _config->getLlvmStackVariable(fnc, off);
					if (st)
					{
						args.push_back(st);
					}

					off += sz;
				}
			}
			else if (_config->getConfig().architecture.isPic32())
			{
				if (aIdx < mipsNames.size())
				{
					auto* r = _module->getNamedGlobal(mipsNames[aIdx]);
					if (r)
					{
						args.push_back(r);
					}
				}
				else
				{
					auto* st = _config->getLlvmStackVariable(fnc, off);
					if (st)
					{
						args.push_back(st);
					}

					off += sz;
				}
			}
			else if (_config->getConfig().architecture.isMips())
			{
				bool useStack = false;
				if (t->isFloatingPointTy())
				{
					--aIdx;

					if (faIdx < mipsFpNames.size())
					{
						auto* r = _module->getNamedGlobal(mipsFpNames[faIdx]);
						if (r)
						{
							args.push_back(r);
						}

						if (sz > 4)
						{
							++faIdx;
						}
					}
					else
					{
						useStack = true;
					}

					++faIdx;
				}
				else
				{
					if (aIdx < mipsNames.size())
					{
						auto* r = _module->getNamedGlobal(mipsNames[aIdx]);
						if (r)
						{
							args.push_back(r);
						}
					}
					else
					{
						useStack = true;
					}
				}

				if (useStack)
				{
					auto* st = _config->getLlvmStackVariable(fnc, off);
					if (st)
					{
						args.push_back(st);
					}

					off += sz;
				}
			}

			++aIdx;
		}

		//
		//
		unsigned idx = 0;
		std::vector<Value*> loads;
		for (auto* a : args)
		{
			Value* l = new LoadInst(a, "", ce.call);
			LOG << "\t\t" << llvmObjToString(l) << std::endl;

			if (types.size() > idx)
			{
				l = IrModifier::convertValueToType(l, types[idx], ce.call);
			}

			loads.push_back(l);
			++idx;
		}

		if (!loads.empty())
		{
			calls2vals[ce.call] = loads;
		}
	}

	if (_config->getConfig().architecture.isX86())
	{
		if (retType->isFloatingPointTy())
		{
			retVal = _config->getLlvmRegister("st7");
		}
		else if (_config->getConfig().architecture.isX86_32())
		{
			retVal = _config->getLlvmRegister("eax");
		}
		else if (_config->getConfig().architecture.isX86_64())
		{
			retVal = _config->getLlvmRegister("rax");
		}
	}
	else if (_config->getConfig().architecture.isMipsOrPic32())
	{
		retVal = _config->getLlvmRegister("v0");
	}
	else if (_config->getConfig().architecture.isArmOrThumb())
	{
		retVal = _config->getLlvmRegister("r0");
	}
	else if (_config->getConfig().architecture.isPpc())
	{
		retVal = _config->getLlvmRegister("r3");
	}
	if (retVal)
	{
		for (auto& e : retStores)
		{
			auto* l = new LoadInst(retVal, "", e.ret);
			rets2vals[e.ret] = l;
		}
	}

	auto* fnc = getFunction();
	auto* oldType = fnc->getType();

	IrModifier irm(_module, _config);
	auto* newFnc = irm.modifyFunction(
			fnc,
			retType,
			argTypes,
			isVarArg,
			rets2vals,
			calls2vals,
			retVal,
			argStores,
			argNames).first;

	LOG << "modify fnc: " << newFnc->getName().str() << " = "
			<< llvmObjToString(oldType) << " -> "
			<< llvmObjToString(newFnc->getType()) << std::endl;

	called = newFnc;
}

void DataFlowEntry::connectWrappers()
{
	auto* fnc = getFunction();
	if (fnc == nullptr || wrappedCall == nullptr)
	{
		return;
	}

	wrappedCall = nullptr;
	for (inst_iterator I = inst_begin(fnc), E = inst_end(fnc); I != E; ++I)
	{
		if (auto* c = dyn_cast<CallInst>(&*I))
		{
			auto* cf = c->getCalledFunction();
			if (cf && !cf->isIntrinsic()) // && cf->isDeclaration())
			{
				wrappedCall = c;
				break;
			}
		}
	}

	if (wrappedCall == nullptr)
	{
		return;
	}

	if (wrappedCall->getNumArgOperands() != fnc->getArgumentList().size())
	{
		// TODO: enable assert and inspect these cases.
		return;
	}
	assert(wrappedCall->getNumArgOperands() == fnc->getArgumentList().size());

	unsigned i = 0;
	for (auto& a : fnc->getArgumentList())
	{
		auto* conv = IrModifier::convertValueToType(&a, wrappedCall->getArgOperand(i)->getType(), wrappedCall);
		wrappedCall->setArgOperand(i++, conv);
	}

	//
	//
	std::set<CallInst*> calls;
	for (auto* u : fnc->users())
	{
		if (auto* c = dyn_cast<CallInst>(u))
		{
			// inline all wrapped functions
			// TODO: only really simple fncs, or from .plt, etc.?
//			if (fnc->isVarArg())
			{
				calls.insert(c);
			}
		}
	}

	auto* wrappedFnc = wrappedCall->getCalledFunction();
	assert(wrappedFnc);
	for (auto* c : calls)
	{
		// todo: should not happen?
		if (c->getType()->isVoidTy() && !wrappedFnc->getReturnType()->isVoidTy())
		{
			continue;
		}

		std::vector<Value*> args;
		unsigned numParams = wrappedFnc->getFunctionType()->getNumParams();
		unsigned i = 0;
		for (auto& a : c->arg_operands())
		{
			if (i >= numParams) // var args fncs
			{
				assert(wrappedFnc->isVarArg());
				args.push_back(a);
			}
			else
			{
				auto* conv = IrModifier::convertValueToType(a, wrappedFnc->getFunctionType()->getParamType(i++), c);
				args.push_back(conv);
			}
		}
		auto* nc = CallInst::Create(wrappedFnc, args, "", c);
		auto* resConv = IrModifier::convertValueToTypeAfter(nc, c->getType(), nc);
		c->replaceAllUsesWith(resConv);
		c->eraseFromParent();
	}
}

llvm::CallInst* DataFlowEntry::isSimpleWrapper(llvm::Function* fnc)
{
	auto ai = AsmInstruction(fnc);
	if (ai.isInvalid())
	{
		return nullptr;
	}

	bool single = true;
	auto next = ai.getNext();
	while (next.isValid())
	{
		if (!next.empty() && !isa<TerminatorInst>(next.front()))
		{
			single = false;
			break;
		}
		next = next.getNext();
	}

	// Pattern
	// .text:00008A38                 LDR     R0, =aCCc       ; "C::cc()"
	// .text:00008A3C                 B       puts
	// .text:00008A40 off_8A40        DCD aCCc
	// TODO: make better wrapper detection. In wrapper, wrapped function params
	// should not be set like in this example.
	//
	if (ai && next)
	{
		if (_image->getConstantDefault(next.getEndAddress()))
		{
			auto* l = ai.getInstructionFirst<LoadInst>();
			auto* s = ai.getInstructionFirst<StoreInst>();
			auto* c = next.getInstructionFirst<CallInst>();
			if (l && s && c && isa<GlobalVariable>(l->getPointerOperand())
					&& s->getPointerOperand()->getName() == "r0")
			{
				auto gvA = _config->getGlobalAddress(cast<GlobalVariable>(l->getPointerOperand()));
				if (gvA == next.getEndAddress())
				{
					return nullptr;
				}
			}
		}
	}

	if (single)
	{
		for (auto& i : ai)
		{
			if (auto* c = dyn_cast<CallInst>(&i))
			{
				auto* cf = c->getCalledFunction();
				if (cf && !cf->isIntrinsic()) // && cf->isDeclaration())
				{
					return c;
				}
			}
		}
	}

	unsigned aiNum = 0;
	bool isSmall = true;
	next = ai;
	while (next.isValid())
	{
		++aiNum;
		next = next.getNext();
		if (aiNum > 4)
		{
			isSmall = false;
			break;
		}
	}
	auto* s = _image->getImage()->getSegmentFromAddress(ai.getAddress());
	if ((s && s->getName() == ".plt") || isSmall)
	{
		for (inst_iterator it = inst_begin(fnc), rIt = inst_end(fnc);
				it != rIt; ++it)
		{
			if (auto* l = dyn_cast<LoadInst>(&*it))
			{
				std::string n = l->getPointerOperand()->getName();
				if (n == "lr" || n == "sp")
				{
					return nullptr;
				}
			}
			else if (auto* s = dyn_cast<StoreInst>(&*it))
			{
				std::string n = s->getPointerOperand()->getName();
				if (n == "lr" || n == "sp")
				{
					return nullptr;
				}
			}
			else if (auto* c = dyn_cast<CallInst>(&*it))
			{
				auto* cf = c->getCalledFunction();
				if (cf && !cf->isIntrinsic() && cf->isDeclaration())
				{
					return c;
				}
			}
		}
	}

	return nullptr;
}

void DataFlowEntry::setTypeFromExtraInfo()
{
	auto* fnc = getFunction();
	if (fnc == nullptr)
	{
		return;
	}

	// Main
	//
	if (fnc->getName() == "main")
	{
		argTypes.push_back(Type::getInt32Ty(_module->getContext()));
		argTypes.push_back(PointerType::get(
				PointerType::get(
						Type::getInt8Ty(_module->getContext()),
						0),
				0));
		argNames.push_back("argc");
		argNames.push_back("argv");
		retType = Type::getInt32Ty(_module->getContext());
		typeSet = true;
		return;
	}

	// LTI info.
	//
	auto* cf = _config->getConfigFunction(fnc);
	if (cf && (cf->isDynamicallyLinked() || cf->isStaticallyLinked()))
	{
		auto fp = _lti->getPairFunctionFree(cf->getName());
		if (fp.first)
		{
			for (auto& a : fp.first->args())
			{
				argTypes.push_back(a.getType());
				argNames.push_back(a.getName());
			}
			if (fp.first->isVarArg())
			{
				isVarArg = true;
			}
			retType = fp.first->getReturnType();
			typeSet = true;

			std::string declr = fp.second->getDeclaration();
			if (!declr.empty())
			{
				cf->setDeclarationString(declr);
			}

			// TODO: we could rename function if LTI name differs.
			// e.g. scanf vs _scanf.
			//
//			if (fp.first->getName() != fnc->getName())
//			{
//				IrModifier irmodif(_module, _config);
//				irmodif.renameFunction(fnc, fp.first->getName());
//			}

			return;
		}
	}

	// Debug info.
	//
	if (dbgFnc)
	{
		for (auto& a : dbgFnc->parameters)
		{
			auto* t = llvm_utils::stringToLlvmTypeDefault(_module, a.type.getLlvmIr());
			argTypes.push_back(t);
			argNames.push_back(a.getName());
		}
		if (dbgFnc->isVariadic())
		{
			isVarArg = true;
		}
		retType = llvm_utils::stringToLlvmTypeDefault(
				_module,
				dbgFnc->returnType.getLlvmIr());
		typeSet = true;
		return;
	}

	if (_config->getConfig().isIda() && configFnc)
	{
		for (auto& a : configFnc->parameters)
		{
			auto* t = llvm_utils::stringToLlvmTypeDefault(_module, a.type.getLlvmIr());
			argTypes.push_back(t);
			argNames.push_back(a.getName());

			if (_config->getConfig().architecture.isX86())
			{
				std::string regName;
				if (a.getStorage().isRegister(regName))
				{
					if (auto* reg = _config->getLlvmRegister(regName))
					{
						specialArgStorage[argTypes.size()-1] = reg;
					}
				}
			}
		}
		if (configFnc->isVariadic())
		{
			isVarArg = true;
		}
		retType = llvm_utils::stringToLlvmTypeDefault(
				_module,
				configFnc->returnType.getLlvmIr());
		if (!argTypes.empty())
		{
			typeSet = true;
		}
		return;
	}

	// Wrappers.
	//
	if ((wrappedCall = isSimpleWrapper(fnc)))
	{
		auto* wf = wrappedCall->getCalledFunction();
		auto* ltiFnc = _lti->getLlvmFunctionFree(wf->getName());
		if (ltiFnc)
		{
			for (auto& a : ltiFnc->args())
			{
				argTypes.push_back(a.getType());
				argNames.push_back(a.getName());
			}
			if (ltiFnc->isVarArg())
			{
				isVarArg = true;
			}
			retType = ltiFnc->getReturnType();
			typeSet = true;
			return;
		}
		else
		{
			wrappedCall = nullptr;
		}
	}
}

void DataFlowEntry::setTypeFromUseContext()
{
	if (!typeSet)
	{
		setReturnType();
		setArgumentTypes();
		typeSet = true;
	}
}

void DataFlowEntry::setReturnType()
{
	llvm::Value* retVal = nullptr;
	if (_config->getConfig().architecture.isX86())
	{
		bool hasEax = false;
		bool hasRax = false;
		bool hasSt0 = false;
		for (auto& re : retStores)
		{
			for (StoreInst* s : re.possibleRetStores)
			{
				if (s->getPointerOperand()->getName() == "eax")
				{
					hasEax = true;
					break;
				}
				else if (s->getPointerOperand()->getName() == "rax")
				{
					hasRax = true;
					break;
				}
				else if (s->getPointerOperand()->getName() == "st7")
				{
					hasSt0 = true;
				}
			}
		}
		if (!hasEax && !hasRax && hasSt0)
		{
			retVal = _config->getLlvmRegister("st7");
		}
		else if (_config->getLlvmRegister("rax"))
		{
			retVal = _config->getLlvmRegister("rax");
		}
		else
		{
			retVal = _config->getLlvmRegister("eax");
		}
	}
	else if (_config->getConfig().architecture.isMipsOrPic32())
	{
		retVal = _config->getLlvmRegister("v0");
	}
	else if (_config->getConfig().architecture.isArmOrThumb())
	{
		retVal = _config->getLlvmRegister("r0");
	}
	else if (_config->getConfig().architecture.isPpc())
	{
		retVal = _config->getLlvmRegister("r3");
	}

	retType = retVal ?
			retVal->getType()->getPointerElementType() :
			Type::getVoidTy(_module->getContext());
}

void DataFlowEntry::setArgumentTypes()
{
	if (calls.empty())
	{
		bool positionalX86StackArguments =
				_config->getConfig().architecture.isX86_32();
		const uint64_t slotSize = positionalX86StackArguments
				? _config->getConfig().architecture.getByteSize() : 0;
		int64_t nextOffset = slotSize;
		for (auto* load : argLoads)
		{
			auto loadOffset = _config->getStackVariableOffset(
					load->getPointerOperand());
			if (loadOffset.isUndefined()
					|| loadOffset.getValue() < nextOffset
					|| (loadOffset.getValue() - int64_t(slotSize))
							% int64_t(slotSize) != 0)
			{
				positionalX86StackArguments = false;
				break;
			}

			auto* type = load->getType();
			type = type->isFloatingPointTy()
					? type : Abi::getDefaultType(_module);
			uint64_t size = _module->getDataLayout().getTypeStoreSize(type);
			nextOffset = loadOffset.getValue() + std::max<uint64_t>(
					slotSize,
					((size + slotSize - 1) / slotSize) * slotSize);
		}

		if (positionalX86StackArguments)
		{
			nextOffset = slotSize;
			for (auto* load : argLoads)
			{
				auto loadOffset = _config->getStackVariableOffset(
						load->getPointerOperand());
				while (nextOffset < loadOffset.getValue())
				{
					argTypes.push_back(Abi::getDefaultType(_module));
					nextOffset += slotSize;
				}

				auto* type = load->getType();
				type = type->isFloatingPointTy()
						? type : Abi::getDefaultType(_module);
				argTypes.push_back(type);
				uint64_t size = _module->getDataLayout().getTypeStoreSize(type);
				nextOffset += std::max<uint64_t>(
						slotSize,
						((size + slotSize - 1) / slotSize) * slotSize);
			}
		}
		else
		{
			for (auto* load : argLoads)
			{
				auto* type = load->getType();
				argTypes.push_back(type->isFloatingPointTy()
						? type : Abi::getDefaultType(_module));
			}
		}
	}
	else
	{
		CallEntry* ce = &calls.front();
		for (auto& c : calls)
		{
			// A split basic block or an incompletely decoded predecessor can hide
			// one outgoing stack store at an individual call site.  The filtering
			// above establishes a common upper bound, so infer the function type
			// from the most complete surviving call rather than whichever call was
			// encountered first.
			if (c.possibleArgStores.size() > ce->possibleArgStores.size())
			{
				ce = &c;
			}
		}

		for (auto* store : ce->possibleArgStores)
		{
			auto* type = store->getValueOperand()->getType();
			auto instruction = AsmInstruction(store);
			auto* capstone = instruction.isValid()
					? instruction.getCapstoneInsn() : nullptr;
			bool convertedPush = capstone != nullptr
					&& capstone->id == X86_INS_PUSH;
			argTypes.push_back(type->isFloatingPointTy() && !convertedPush
					? type : Abi::getDefaultType(_module));
		}
	}
}

} // namespace bin2llvmir
} // namespace retdec
