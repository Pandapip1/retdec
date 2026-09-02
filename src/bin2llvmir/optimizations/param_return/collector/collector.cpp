/**
* @file src/bin2llvmir/optimizations/param_return/collector/collector.cpp
* @brief Collects possible arguments and returns of functions.
* @copyright (c) 2019 Avast Software, licensed under the MIT license
*/

#include <algorithm>
#include <limits>
#include <queue>
#include <set>

#include <llvm/IR/CFG.h>
#include <llvm/IR/InstIterator.h>

#include "retdec/bin2llvmir/optimizations/param_return/collector/collector.h"
#include "retdec/bin2llvmir/optimizations/param_return/collector/pic32.h"
#include "retdec/bin2llvmir/providers/asm_instruction.h"
#include "retdec/bin2llvmir/utils/llvm.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

Collector::Collector(
		const Abi* abi,
		Module* m,
		const ReachingDefinitionsAnalysis* rda) :
	_abi(abi),
	_module(m),
	_rda(rda)
{
}

void Collector::collectCallArgs(CallEntry* ce) const
{
	// Trusted fixed signatures with no parameters need no argument recovery.
	// In particular, an x86 call immediately after callee-save PUSHes must not
	// reinterpret those spills as outgoing arguments or erase them before the
	// matching POPs are reconstructed.
	if (ce->getBaseFunction()->isVoidarg()
			&& !ce->getBaseFunction()->isVariadic())
	{
		ce->setArgStores({});
		return;
	}

	if (_abi->getConfig()->getConfig().architecture.isX86_32())
	{
		collectX86CallArgs(ce);
		return;
	}

	std::vector<llvm::StoreInst*> foundStores;

	collectStoresBeforeInstruction(
		ce->getCallInstruction(),
		foundStores);

	ce->setArgStores(std::move(foundStores));
}

unsigned Collector::getX86CleanupBytes(
		CallInst* call,
		std::vector<StoreInst*>* cleanupMarkers) const
{
	unsigned bytes = 0;
	auto* config = _abi->getConfig();
	auto callAsm = AsmInstruction(call);
	auto* callBlock = callAsm.getBasicBlock();
	auto cleanupAsm = callAsm.getNext();
	unsigned skippedInstructions = 0;

	while (cleanupAsm.isValid())
	{
		auto* instruction = cleanupAsm.getCapstoneInsn();
		if (instruction == nullptr || cleanupAsm.getBasicBlock() != callBlock)
		{
			break;
		}
		if (instruction->id == X86_INS_POP)
		{
			if (cleanupMarkers != nullptr)
			{
				cleanupMarkers->push_back(cleanupAsm.getLlvmToAsmInstruction());
			}
			unsigned slotSize = config->getConfig().architecture.getByteSize();
			if (slotSize == 0
					|| slotSize > std::numeric_limits<unsigned>::max() - bytes)
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
					cleanupMarkers->push_back(cleanupAsm.getLlvmToAsmInstruction());
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
						&& (op.reg == X86_REG_SP || op.reg == X86_REG_ESP
								|| op.reg == X86_REG_RSP);
				stackPointerOperand |= op.type == X86_OP_MEM
						&& (op.mem.base == X86_REG_SP || op.mem.base == X86_REG_ESP
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
			for (unsigned group = 0; group < instruction->detail->groups_count; ++group)
			{
				auto id = instruction->detail->groups[group];
				controlFlow |= id == CS_GRP_JUMP || id == CS_GRP_CALL
						|| id == CS_GRP_RET;
			}
		}
		if (stackPointerOperand || controlFlow || instruction->id == X86_INS_PUSH
				|| instruction->id == X86_INS_LEAVE || ++skippedInstructions > 8)
		{
			break;
		}
		cleanupAsm = cleanupAsm.getNext();
	}

	// Stack analysis can fold the restored value to a frame pointer.  Retain a
	// conventional LLVM ESP += constant fallback when machine metadata supplied
	// no bound.
	for (auto* next = bytes == 0 ? call->getNextNode() : nullptr;
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
		if (store == nullptr || !_abi->isStackPointerRegister(store->getPointerOperand()))
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
			auto* amount = dyn_cast<ConstantInt>(addition->getOperand(operand));
			auto* load = dyn_cast<LoadInst>(llvm_utils::skipCasts(
					addition->getOperand(1 - operand)));
			if (amount != nullptr && amount->getSExtValue() > 0
					&& load != nullptr && load->getPointerOperand() == stackPointer)
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
}

void Collector::collectX86CallArgs(CallEntry* ce) const
{
	auto* call = ce->getCallInstruction();
	auto* config = _abi->getConfig();
	std::vector<StoreInst*> cleanupMarkers;
	unsigned cleanupBytes = getX86CleanupBytes(call, &cleanupMarkers);
	for (auto* marker : cleanupMarkers)
	{
		ce->addObsoleteStackCleanupMarker(marker);
	}

	std::vector<StoreInst*> stores;
	std::set<StoreInst*> directStores;
	std::set<StoreInst*> provenStores;
	std::vector<StoreInst*> mergedPushStores;
	Value* mergedPushValue = nullptr;
	std::set<Value*> disqualifiedValues;
	Value* pendingStackPointer = nullptr;
	unsigned nestedCleanupBytes = 0;
	uint64_t recoveredStackBytes = 0;
	uint64_t overwrittenReservationBytes = 0;
	AsmInstruction overwrittenReservationAsm;
	unsigned slotSize = config->getConfig().architecture.getByteSize();
	auto* block = call->getParent();
	Instruction* previous = call;
	std::set<BasicBlock*> seen{block};

	while (true)
	{
		if (previous == &block->front())
		{
			auto* predecessor = block->getSinglePredecessor();
			if (predecessor == nullptr || predecessor->empty()
					|| predecessor == block || seen.count(predecessor) != 0)
			{
				break;
			}
			block = predecessor;
			previous = &block->back();
			seen.insert(block);
		}
		else
		{
			previous = previous->getPrevNode();
		}
		if (previous == nullptr)
		{
			break;
		}

		if (overwrittenReservationBytes != 0 && overwrittenReservationAsm.isValid())
		{
			AsmInstruction currentAsm(previous);
			if (currentAsm.isValid()
					&& currentAsm.getLlvmToAsmInstruction()
							!= overwrittenReservationAsm.getLlvmToAsmInstruction())
			{
				auto* instruction = currentAsm.getCapstoneInsn();
				bool x87Producer = instruction != nullptr
						&& (instruction->id == X86_INS_FLD
								|| instruction->id == X86_INS_FLD1
								|| instruction->id == X86_INS_FLDZ
								|| instruction->id == X86_INS_FLDPI
								|| instruction->id == X86_INS_FLDL2E
								|| instruction->id == X86_INS_FLDL2T
								|| instruction->id == X86_INS_FLDLG2
								|| instruction->id == X86_INS_FLDLN2);
				if (instruction == nullptr
						|| (instruction->id != X86_INS_PUSH && !x87Producer)
						|| currentAsm.getEndAddress()
								!= overwrittenReservationAsm.getAddress())
				{
					overwrittenReservationBytes = 0;
					overwrittenReservationAsm = AsmInstruction();
				}
				else if (x87Producer)
				{
					overwrittenReservationAsm = currentAsm;
				}
			}
		}

		if (auto* priorCall = dyn_cast<CallInst>(previous))
		{
			overwrittenReservationBytes = 0;
			overwrittenReservationAsm = AsmInstruction();
			auto* called = priorCall->getCalledFunction();
			if (called == nullptr || !called->isIntrinsic())
			{
				unsigned nestedBytes = getX86CleanupBytes(priorCall, nullptr);
				if (!_abi->getConfig()->getConfig().architecture.isX86_32()
						|| cleanupBytes == 0 || slotSize == 0
						|| nestedBytes == 0 || nestedBytes % slotSize != 0
						|| nestedBytes > std::numeric_limits<unsigned>::max()
								- nestedCleanupBytes)
				{
					break;
				}
				nestedCleanupBytes += nestedBytes;
				pendingStackPointer = nullptr;
				continue;
			}
			continue;
		}

		auto* store = dyn_cast<StoreInst>(previous);
		if (store == nullptr)
		{
			continue;
		}
		auto* value = store->getValueOperand();
		auto* pointer = store->getPointerOperand();
		bool unresolvedPush = false;
		bool unresolvedStackBlock = false;
		if (_abi->isStackPointerRegister(pointer))
		{
			pendingStackPointer = llvm_utils::skipCasts(value);
		}
		else if (pendingStackPointer != nullptr)
		{
			unresolvedPush = llvm_utils::skipCasts(pointer) == pendingStackPointer;
			pendingStackPointer = nullptr;
		}

		AsmInstruction asmInstruction(store);
		auto* capstone = asmInstruction.isValid()
				? asmInstruction.getCapstoneInsn() : nullptr;
		bool decodedPush = capstone != nullptr && capstone->id == X86_INS_PUSH;
		bool decodedWideStackStore = capstone != nullptr
				&& (capstone->id == X86_INS_FST || capstone->id == X86_INS_FSTP)
				&& _abi->isStackVariable(pointer) && value->getType()->isSized()
				&& slotSize != 0
				&& _module->getDataLayout().getTypeStoreSize(value->getType()) > slotSize;
		if (decodedPush && cleanupBytes != 0
				&& !AsmInstruction::isLlvmToAsmInstruction(store)
				&& !_abi->isStackPointerRegister(pointer)
				&& !_abi->isStackVariable(pointer) && !_abi->isRegister(pointer))
		{
			unresolvedPush = true;
		}
		if (!decodedPush && !unresolvedPush && cleanupBytes != 0
				&& value->getType()->isSized())
		{
			auto* address = llvm_utils::skipCasts(pointer);
			auto* stackLoad = dyn_cast<LoadInst>(address);
			unresolvedStackBlock = stackLoad != nullptr
					&& _abi->isStackPointerRegister(stackLoad->getPointerOperand())
					&& _module->getDataLayout().getTypeStoreSize(value->getType())
							<= cleanupBytes;
		}

		if (overwrittenReservationBytes != 0 && decodedPush
				&& !AsmInstruction::isLlvmToAsmInstruction(store)
				&& !_abi->isStackPointerRegister(pointer)
				&& (unresolvedPush || _abi->isStackVariable(pointer)))
		{
			if (slotSize > overwrittenReservationBytes)
			{
				break;
			}
			overwrittenReservationBytes -= slotSize;
			overwrittenReservationAsm = asmInstruction;
			continue;
		}

		if (!unresolvedPush && !unresolvedStackBlock
				&& !_abi->isStackVariable(pointer) && !_abi->isRegister(pointer))
		{
			disqualifiedValues.insert(pointer);
		}
		if (auto* load = dyn_cast<LoadInst>(value))
		{
			if (load->getPointerOperand()->getName() == "ebp"
					|| load->getPointerOperand()->getName() == "rbp")
			{
				if (cleanupBytes == 0 || (!decodedPush && !unresolvedPush))
				{
					disqualifiedValues.insert(pointer);
				}
			}
			if (_abi->isRegister(pointer)
					&& _abi->isRegister(load->getPointerOperand())
					&& pointer != load->getPointerOperand())
			{
				disqualifiedValues.insert(load->getPointerOperand());
			}
		}

		if (disqualifiedValues.count(pointer) != 0 || _abi->isFlagRegister(pointer)
				|| (!isa<AllocaInst>(pointer) && !_abi->isRegister(pointer)
						&& !unresolvedPush && !unresolvedStackBlock))
		{
			continue;
		}

		uint64_t stackBytes = 0;
		bool stackArgument = _abi->isStackVariable(pointer)
				|| unresolvedPush || unresolvedStackBlock;
		bool decodedStackEffect = decodedPush || unresolvedPush
				|| unresolvedStackBlock || decodedWideStackStore;
		if (stackArgument && decodedStackEffect && slotSize != 0)
		{
			uint64_t valueBytes = value->getType()->isSized()
					? _module->getDataLayout().getTypeStoreSize(value->getType())
					: slotSize;
			stackBytes = decodedPush || unresolvedPush ? slotSize
					: std::max<uint64_t>(slotSize,
							((valueBytes + slotSize - 1) / slotSize) * slotSize);
		}
		if ((unresolvedStackBlock || decodedWideStackStore) && stackBytes != 0)
		{
			overwrittenReservationBytes += stackBytes;
			overwrittenReservationAsm = asmInstruction;
		}
		else if (overwrittenReservationBytes != 0
				&& stackArgument && !decodedStackEffect)
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

		stores.push_back(store);
		if (stackArgument && decodedStackEffect)
		{
			provenStores.insert(store);
		}
		if ((unresolvedPush || unresolvedStackBlock) && !_abi->isStackVariable(pointer))
		{
			directStores.insert(store);
		}
		disqualifiedValues.insert(pointer);
		disqualifiedValues.insert(store);

		if (stackBytes != 0)
		{
			recoveredStackBytes += stackBytes;
			if (cleanupBytes != 0 && recoveredStackBytes >= cleanupBytes)
			{
				auto previousAsm = asmInstruction.getPrev();
				auto* previousCapstone = previousAsm.isValid()
						? previousAsm.getCapstoneInsn() : nullptr;
				bool adjacentPush = decodedPush && previousCapstone != nullptr
						&& previousCapstone->id == X86_INS_PUSH
						&& previousAsm.getEndAddress() == asmInstruction.getAddress();
				if (!adjacentPush)
				{
					break;
				}
			}
		}
	}

	// A fixed-arity x86 call may share its oldest PUSH across several incoming
	// CFG edges while the remaining PUSHes live in the call block. Follow that
	// join only when exactly one physical word is missing and every predecessor
	// ends by writing one machine word for a decoded PUSH. Stack analysis may
	// have localized some of those destinations while leaving others as raw ESP
	// expressions, so destination identity is not a valid cross-edge invariant;
	// the terminal PUSH role on every edge is. This is deliberately fail-closed:
	// intervening calls, non-word stores, wide parameters, or more than one
	// missing word remain unresolved.
	const auto& fixedTypes = ce->getBaseFunction()->argTypes();
	bool singleWordTypes = slotSize != 0 && !fixedTypes.empty()
			&& std::all_of(
					fixedTypes.begin(), fixedTypes.end(),
					[this, slotSize](Type* type)
					{
						return type != nullptr && type->isSized()
								&& _module->getDataLayout().getTypeStoreSize(type)
										<= slotSize;
					});
	if (singleWordTypes && provenStores.size() + 1 == fixedTypes.size()
			&& pred_size(block) > 1)
	{
		Type* commonType = nullptr;
		std::vector<std::pair<BasicBlock*, StoreInst*>> incomingPushes;
		for (BasicBlock* predecessor : predecessors(block))
		{
			StoreInst* pushStore = nullptr;
			for (auto it = predecessor->rbegin(); it != predecessor->rend(); ++it)
			{
				if (auto* priorCall = dyn_cast<CallInst>(&*it))
				{
					auto* callee = priorCall->getCalledFunction();
					if (callee == nullptr || !callee->isIntrinsic())
					{
						break;
					}
				}
				auto* store = dyn_cast<StoreInst>(&*it);
				if (store == nullptr
						|| AsmInstruction::isLlvmToAsmInstruction(store))
				{
					continue;
				}
				AsmInstruction decoded(store);
				auto* capstone = decoded.isValid()
						? decoded.getCapstoneInsn() : nullptr;
				if (capstone == nullptr || capstone->id != X86_INS_PUSH)
				{
					// An unconditional transfer may carry only the recovered ESP
					// value into the join. Any other decoded instruction after the
					// candidate PUSH makes the predecessor non-terminal.
					if (capstone != nullptr && capstone->id != X86_INS_JMP
							&& capstone->id != X86_INS_NOP)
					{
						break;
					}
					continue;
				}
				// A decoded PUSH writes both the outgoing stack word and ESP. The
				// latter commonly follows the argument store in lifted IR, so skip
				// it while looking backwards for the actual pushed value.
				if (_abi->isStackPointerRegister(store->getPointerOperand()))
				{
					continue;
				}
				Type* valueType = store->getValueOperand()->getType();
				bool machineWord = valueType->isSized()
						&& _module->getDataLayout().getTypeStoreSize(valueType)
								== slotSize;
				bool implicitStackDestination =
						_abi->isStackVariable(store->getPointerOperand())
						|| (!_abi->isRegister(store->getPointerOperand())
								&& !isa<GlobalVariable>(store->getPointerOperand()));
				if (machineWord && implicitStackDestination)
				{
					pushStore = store;
				}
				break;
			}
			if (pushStore == nullptr)
			{
				incomingPushes.clear();
				break;
			}
			Type* valueType = pushStore->getValueOperand()->getType();
			if (commonType != nullptr && commonType != valueType)
			{
				incomingPushes.clear();
				break;
			}
			commonType = valueType;
			incomingPushes.emplace_back(predecessor, pushStore);
		}

		if (!incomingPushes.empty())
		{
			Value* firstValue = incomingPushes.front().second->getValueOperand();
			bool sameValue = std::all_of(
					incomingPushes.begin(), incomingPushes.end(),
					[firstValue](const auto& incoming)
					{
						return incoming.second->getValueOperand() == firstValue;
					});
			if (sameValue)
			{
				mergedPushValue = firstValue;
			}
			else
			{
				auto* insertion = &*block->getFirstInsertionPt();
				auto* phi = PHINode::Create(
						commonType, incomingPushes.size(),
						"merged.stack.argument", insertion);
				for (const auto& incoming : incomingPushes)
				{
					phi->addIncoming(
							incoming.second->getValueOperand(), incoming.first);
				}
				mergedPushValue = phi;
			}
			for (const auto& incoming : incomingPushes)
			{
				mergedPushStores.push_back(incoming.second);
			}
		}
	}

	if (cleanupBytes != 0 && slotSize != 0)
	{
		auto candidateBytes = [&](StoreInst* store) -> uint64_t
		{
			bool stackArgument = _abi->isStackVariable(store->getPointerOperand())
					|| directStores.count(store) != 0;
			if (!stackArgument)
			{
				return 0;
			}
			auto instruction = AsmInstruction(store);
			auto* decoded = instruction.isValid()
					? instruction.getCapstoneInsn() : nullptr;
			uint64_t valueBytes = decoded != nullptr && decoded->id == X86_INS_PUSH
					? slotSize : _module->getDataLayout().getTypeStoreSize(
							store->getValueOperand()->getType());
			return std::max<uint64_t>(slotSize,
					((valueBytes + slotSize - 1) / slotSize) * slotSize);
		};
		uint64_t provenBytes = 0;
		for (auto* store : stores)
		{
			if (provenStores.count(store) != 0)
			{
				provenBytes += candidateBytes(store);
			}
		}
		uint64_t uncertainBytes = 0;
		uint64_t uncertainLimit = provenBytes < cleanupBytes
				? cleanupBytes - provenBytes : 0;
		auto it = stores.begin();
		while (it != stores.end())
		{
			auto* store = *it;
			uint64_t bytes = candidateBytes(store);
			bool proven = provenStores.count(store) != 0;
			if (!proven && bytes != 0 && uncertainBytes + bytes > uncertainLimit)
			{
				directStores.erase(store);
				it = stores.erase(it);
			}
			else
			{
				if (!proven)
				{
					uncertainBytes += bytes;
				}
				++it;
			}
		}
	}

	bool hasProvenStack = false;
	bool allProvenStackStoresAreDecodedPushes = true;
	for (auto* store : stores)
	{
		if (!_abi->isStackVariable(store->getPointerOperand())
				&& directStores.count(store) == 0)
		{
			continue;
		}
		if (provenStores.count(store) == 0)
		{
			continue;
		}
		hasProvenStack = true;
		auto instruction = AsmInstruction(store);
		auto* decoded = instruction.isValid() ? instruction.getCapstoneInsn() : nullptr;
		allProvenStackStoresAreDecodedPushes &= decoded != nullptr
				&& decoded->id == X86_INS_PUSH;
	}
	// Callee-cleanup calls have no caller-side cleanup bound, so collection may
	// also see older configured stack locals.  Those unrelated stores must not
	// make us sort the decoded PUSH prefix by frame offset: PUSH traversal order
	// is the native argument order even when older stack stores follow it.
	ce->preserveNativeStackOrder(!directStores.empty()
			|| mergedPushValue != nullptr
			|| (hasProvenStack && allProvenStackStoresAreDecodedPushes));
	if (ce->preservesNativeStackOrder() && hasProvenStack)
	{
		// A callee-cleanup call has no caller-side cleanup bound.  A write to
		// an unrelated local may consequently appear between the decoded PUSH
		// of an argument and the call (for example, initializing the object
		// whose address was just pushed).  Backward traversal encounters that
		// speculative local first.  Keep decoded outgoing stack writes in their
		// native order, but rank them ahead of merely possible stack locals so a
		// fixed callee signature consumes the actual PUSH values.
		std::stable_partition(
				stores.begin(), stores.end(),
				[&provenStores](StoreInst* store)
				{
					return provenStores.count(store) != 0;
				});
	}
	for (auto* store : directStores)
	{
		ce->addDirectArgStore(store);
	}
	for (auto* store : provenStores)
	{
		// Decoded outgoing stack writes are erased after rebuilding the call.
		// Use their stored values directly, including when stack analysis has
		// already rewritten the native slots to configured stack variables.
		ce->addDirectArgStore(store);
		// Every proven native stack write in the caller-cleanup window becomes
		// obsolete once the call is rebuilt with LLVM arguments.  Keep this
		// independent of the filtered argument list: fixed-arity callees may
		// intentionally discard additional native stack words.
		ce->addObsoleteStackArgStore(store);
		if (std::find(stores.begin(), stores.end(), store) != stores.end())
		{
			ce->addProvenStackArgStore(store);
		}
	}
	ce->setArgStores(std::move(stores));
	if (mergedPushValue != nullptr)
	{
		ce->addDirectArgument(mergedPushValue);
		for (auto* store : mergedPushStores)
		{
			ce->addObsoleteStackArgStore(store);
		}
	}
}

void Collector::collectCallRets(CallEntry* ce) const
{
	std::vector<llvm::LoadInst*> foundLoads;

	collectLoadsAfterInstruction(
		ce->getCallInstruction(),
		foundLoads);

	ce->setRetLoads(std::move(foundLoads));
}

void Collector::collectDefArgs(DataFlowEntry* dataflow) const
{
	if (!dataflow->hasDefinition())
	{
		return;
	}

	auto* f = dataflow->getFunction();

	std::set<Value*> added;
	for (auto it = inst_begin(f), end = inst_end(f); it != end; ++it)
	{
		if (auto* l = dyn_cast<LoadInst>(&*it))
		{
			auto* ptr = l->getPointerOperand();
			if (!_abi->isGeneralPurposeRegister(ptr) && !_abi->isStackVariable(ptr))
			{
				continue;
			}

			auto* use = _rda->getUse(l);
			if (use == nullptr)
			{
				continue;
			}

			if ((use->defs.empty() || use->isUndef())
					&& added.find(ptr) == added.end())
			{
				dataflow->addArg(ptr);
				added.insert(ptr);
			}
		}
	}
}

void Collector::collectDefRets(DataFlowEntry* dataflow) const
{
	if (!dataflow->hasDefinition())
	{
		return;
	}

	auto* f = dataflow->getFunction();

	for (auto it = inst_begin(f), end = inst_end(f); it != end; ++it)
	{
		if (auto* r = dyn_cast<ReturnInst>(&*it))
		{
			ReturnEntry* re = dataflow->createRetEntry(r);
			collectRetStores(re);
		}
	}
}

void Collector::collectRetStores(ReturnEntry* re) const
{
	std::vector<llvm::StoreInst*> foundStores;

// TODO:
// This method should be used only after
// speed comparation of below methods.
//
// In this implementation of parameter
// analysis return type is estimated
// only as last option from colelcted
// values. This iss reason why quicklier
// but not reliable method is used
// instead of more reliable one.
//
//	collectStoresBeforeInstruction(
//		re->getRetInstruction(),
//		foundStores);

	collectStoresInSinglePredecessors(
		re->getRetInstruction(),
		foundStores);

	re->setRetStores(std::move(foundStores));
}

void Collector::collectStoresBeforeInstruction(
		llvm::Instruction* i,
		std::vector<llvm::StoreInst*>& stores) const
{
	if (i == nullptr)
	{
		return;
	}

	std::map<BasicBlock*, std::set<Value*>> seenBlocks;

	auto* block = i->getParent();

	// In case of recursive call of same basic block.
	std::set<Value*> afterValues;
	std::vector<StoreInst*> afterStores;
	collectStoresInInstructionBlock(
			&block->back(),
			afterValues,
			afterStores);

	seenBlocks[block] = std::move(afterValues);

	collectStoresRecursively(i->getPrevNode(), stores, seenBlocks);

	auto& values = seenBlocks[block];

	stores.insert(
		stores.end(),
		afterStores.begin(),
		afterStores.end());

	stores.erase(
		std::remove_if(
			stores.begin(),
			stores.end(),
			[values](StoreInst* s)
			{
				return values.find(
					s->getPointerOperand()) == values.end();
			}),
		stores.end());
}

void Collector::collectStoresInSinglePredecessors(
		llvm::Instruction* i,
		std::vector<llvm::StoreInst*>& stores) const
{
	if (i == nullptr)
	{
		return;
	}

	std::set<BasicBlock*> seenBbs;
	std::set<Value*> disqualifiedValues;

	auto* b = i->getParent();
	seenBbs.insert(b);
	Instruction* prev = i;

	while (true)
	{
		if (prev == &b->front())
		{
			auto* spb = b->getSinglePredecessor();
			if (spb && !spb->empty()
				&& seenBbs.find(spb) == seenBbs.end())
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

			if (disqualifiedValues.find(ptr) == disqualifiedValues.end()
				&& (_abi->isRegister(ptr) || _abi->isStackVariable(ptr)))
			{
				stores.push_back(store);
				disqualifiedValues.insert(ptr);
			}
		}
		else if (auto* load = dyn_cast<LoadInst>(prev))
		{
			auto* ptr = load->getPointerOperand();
			disqualifiedValues.insert(ptr);
		}
	}
}

void Collector::collectStoresRecursively(
			Instruction* i,
			std::vector<StoreInst*>& stores,
			std::map<BasicBlock*, std::set<Value*>>& seen) const
{
	if (i == nullptr)
	{
		return;
	}

	auto* block = i->getParent();

	std::set<Value*> values;
	if (!collectStoresInInstructionBlock(i, values, stores))
	{
		seen[block] = std::move(values);
		return;
	}

	seen.emplace(std::make_pair(block, values));
	std::set<Value*> commonValues;

	for (BasicBlock* pred : predecessors(block))
	{
		if (seen.find(pred) == seen.end())
		{
			collectStoresRecursively(
					&pred->back(),
					stores,
					seen);
		}

		auto& foundValues = seen[pred];
		if (foundValues.empty())
		{
			// Shorcut -> intersection would be empty set.
			commonValues.clear();
			break;
		}

		if (commonValues.empty())
		{
			commonValues = foundValues;
		}
		else
		{
			std::set<Value*> intersection;
			std::set_intersection(
				commonValues.begin(),
				commonValues.end(),
				foundValues.begin(),
				foundValues.end(),
				std::inserter(intersection, intersection.begin()));

			commonValues = std::move(intersection);
		}
	}

	values.insert(commonValues.begin(), commonValues.end());
	seen[block] = values;
}

bool Collector::collectStoresInInstructionBlock(
			Instruction* start,
			std::set<Value*>& values,
			std::vector<StoreInst*>& stores) const
{
	if (start == nullptr)
	{
		return false;
	}

	std::set<llvm::Value*> excluded;

	auto* block = start->getParent();

	for (auto* inst = start; true; inst = inst->getPrevNode())
	{
		if (inst == nullptr)
		{
			return false;
		}
		if (auto* call = dyn_cast<CallInst>(inst))
		{
			auto* calledFnc = call->getCalledFunction();
			if (calledFnc == nullptr || !calledFnc->isIntrinsic())
			{
				return false;
			}
		}
		else if (isa<ReturnInst>(inst))
		{
			return false;
		}
		else if (auto* store = dyn_cast<StoreInst>(inst))
		{
			auto* val = store->getValueOperand();
			auto* ptr = store->getPointerOperand();

			if (!_abi->isRegister(ptr) && !_abi->isStackVariable(ptr))
			{
				excluded.insert(ptr);
			}
			if (auto* l = dyn_cast<LoadInst>(val))
			{
				if (l->getPointerOperand() != store->getPointerOperand())
				{
					excluded.insert(l->getPointerOperand());
				}
			}

			if (excluded.find(ptr) == excluded.end())
			{
				stores.push_back(store);
				values.insert(ptr);
				excluded.insert(ptr);
				excluded.insert(val);
			}
		}
		if (inst == &block->front())
		{
			return true;
		}
	}

	return true;
}

void Collector::collectLoadsAfterInstruction(
		llvm::Instruction* start,
		std::vector<llvm::LoadInst*>& loads) const
{
	if (start == nullptr)
	{
		return;
	}

	std::queue<llvm::Instruction*> next;
	std::set<llvm::Value*> excludedValues;
	std::set<llvm::BasicBlock*> seen;

	BasicBlock* beginBB = start->getParent();
	next.push(start->getNextNode());

	while (!next.empty())
	{
		auto* i = next.front();
		next.pop();

		auto* block = i->getParent();
		seen.insert(block);

		if (collectLoadsAfterInstruction(i, loads, excludedValues))
		{
			for (auto suc : successors(block))
			{
				if (seen.find(suc) == seen.end())
				{
					next.push(&suc->front());
				}
				else if (suc == beginBB)
				{
					next.push(&beginBB->front());
					beginBB = nullptr;
				}
			}
		}
	}
}

bool Collector::collectLoadsAfterInstruction(
		llvm::Instruction* start,
		std::vector<llvm::LoadInst*>& loads,
		std::set<llvm::Value*>& excluded) const
{
	if (start == nullptr)
	{
		return false;
	}

	auto* block = start->getParent();
	for (auto* inst = start; true; inst = inst->getNextNode())
	{
		if (inst == nullptr)
		{
			return false;
		}
		if (auto* call = dyn_cast<CallInst>(inst))
		{
			auto* calledFnc = call->getCalledFunction();
			if (calledFnc == nullptr || !calledFnc->isIntrinsic())
			{
				return false;
			}
		}
		else if (isa<ReturnInst>(inst))
		{
			return false;
		}
		else if (auto* store = dyn_cast<StoreInst>(inst))
		{
			auto* ptr = store->getPointerOperand();
			excluded.insert(ptr);
		}
		else if (auto* load = dyn_cast<LoadInst>(inst))
		{
			auto* ptr = load->getPointerOperand();

			if (excluded.find(ptr) == excluded.end()
				&& ( _abi->isGeneralPurposeRegister(ptr) || _abi->isStackVariable(ptr) ))
			{
				loads.push_back(load);
			}
		}

		if (inst == &block->back())
		{
			return true;
		}
	}

	return true;
}

void Collector::collectCallSpecificTypes(CallEntry* ce) const
{
	if (!ce->getBaseFunction()->isVariadic())
	{
		return;
	}

	if (!extractFormatString(ce))
	{
		return;
	}

	auto wrappedCall = ce->getBaseFunction()->getWrappedCall();

	auto trueCall = wrappedCall ? wrappedCall : ce->getCallInstruction();

	ce->setArgTypes(
		llvm_utils::parseFormatString(
			_module,
			ce->getFormatString(),
			trueCall->getCalledFunction())
	);
}

bool Collector::extractFormatString(CallEntry* ce) const
{
	for (auto& i : ce->args())
	{
		auto inst = std::find_if(
					ce->argStores().begin(),
					ce->argStores().end(),
					[i](StoreInst *st)
					{
						return st->getPointerOperand() == i;
					});

		if (inst != ce->argStores().end())
		{
			std::string str;
			if (storesString(*inst, str))
			{
				ce->setFormatString(str);
				return true;
			}
		}
	}

	return false;
}

bool Collector::storesString(StoreInst* si, std::string& str) const
{
	auto* v = getRoot(si->getValueOperand());
	auto* gv = dyn_cast_or_null<GlobalVariable>(v);

	if (gv == nullptr || !gv->hasInitializer())
	{
		return false;
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
		return false;
	}

	str = init->getAsString();
	return true;
}

llvm::Value* Collector::getRoot(llvm::Value* i) const
{
	std::set<llvm::Value*> seen;
	return _getRoot(i, seen);
}

llvm::Value* Collector::_getRoot(llvm::Value* i, std::set<llvm::Value*>& seen) const
{
	if (seen.count(i))
	{
		return i;
	}
	seen.insert(i);

	i = llvm_utils::skipCasts(i);
	if (auto* ii = dyn_cast<Instruction>(i))
	{
		if (auto* u = _rda->getUse(ii))
		{
			if (u->defs.size() == 1)
			{
				auto* d = (*u->defs.begin())->def;
				if (auto* s = dyn_cast<StoreInst>(d))
				{
					return _getRoot(s->getValueOperand(), seen);
				}
				else
				{
					return d;
				}
			}
			else if (auto* l = dyn_cast<LoadInst>(ii))
			{
				return _getRoot(l->getPointerOperand(), seen);
			}
			else
			{
				return i;
			}
		}
		else if (auto* l = dyn_cast<LoadInst>(ii))
		{
			return _getRoot(l->getPointerOperand(), seen);
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
//  CollectorProvider
//=============================================================================
//

Collector::Ptr CollectorProvider::createCollector(
				const Abi* abi,
				Module* m,
				const ReachingDefinitionsAnalysis* rda)
{
	if (abi->isPic32())
	{
		return std::make_unique<CollectorPic32>(abi, m, rda);
	}

	return std::make_unique<Collector>(abi, m, rda);
}

}
}
