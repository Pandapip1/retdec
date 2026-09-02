/**
* @file src/bin2llvmir/optimizations/stack/stack.cpp
* @brief Reconstruct stack.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
*/

#include <deque>
#include <limits>
#include <set>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

#include "retdec/bin2llvmir/analyses/reaching_definitions.h"
#include "retdec/bin2llvmir/optimizations/stack/stack.h"
#include "retdec/bin2llvmir/providers/asm_instruction.h"
#include "retdec/bin2llvmir/utils/ir_modifier.h"
#define debug_enabled false
#include "retdec/bin2llvmir/utils/llvm.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

namespace {

struct AffineStackAddress
{
	Value* base = nullptr;
	int64_t baseCoefficient = 0;
	int64_t constant = 0;
	bool hasUnknown = false;
	bool valid = true;

	bool isConstant() const
	{
		return valid && base == nullptr && !hasUnknown;
	}
};

enum class StackDeltaKind
{
	Unreached,
	Known,
	Ambiguous
};

struct StackDelta
{
	StackDeltaKind kind = StackDeltaKind::Unreached;
	int64_t value = 0;
};

bool mergeStackDelta(StackDelta& destination, const StackDelta& source)
{
	if (source.kind == StackDeltaKind::Unreached
			|| destination.kind == StackDeltaKind::Ambiguous)
	{
		return false;
	}
	if (destination.kind == StackDeltaKind::Unreached)
	{
		destination = source;
		return true;
	}
	if (source.kind == StackDeltaKind::Ambiguous
			|| destination.value != source.value)
	{
		destination.kind = StackDeltaKind::Ambiguous;
		return true;
	}
	return false;
}

void adjustStackDelta(StackDelta& delta, int64_t adjustment)
{
	if ((adjustment > 0
				&& delta.value > std::numeric_limits<int64_t>::max() - adjustment)
			|| (adjustment < 0
					&& delta.value < std::numeric_limits<int64_t>::min() - adjustment))
	{
		delta.kind = StackDeltaKind::Ambiguous;
		return;
	}
	delta.value += adjustment;
}

bool mergeBase(
		AffineStackAddress& result,
		const AffineStackAddress& operand,
		int64_t multiplier = 1)
{
	if (operand.base == nullptr)
	{
		return true;
	}
	if (result.base != nullptr && result.base != operand.base)
	{
		return false;
	}
	result.base = operand.base;
	result.baseCoefficient += operand.baseCoefficient * multiplier;
	return true;
}

AllocaInst* getStackAnchor(
		Config* config,
		ReachingDefinitionsAnalysis& rda,
		Value* base)
{
	if (auto* address = dyn_cast<PtrToIntInst>(base))
	{
		auto* anchor = dyn_cast<AllocaInst>(
				llvm_utils::skipCasts(address->getPointerOperand()));
		return anchor != nullptr && config->isStackVariable(anchor)
				? anchor : nullptr;
	}

	auto* load = dyn_cast<LoadInst>(base);
	if (load == nullptr)
	{
		return nullptr;
	}
	if (config->getConfigRegister(load->getPointerOperand()) == nullptr)
	{
		return nullptr;
	}
	const auto& definitions = rda.defsFromUse(load);
	if (definitions.size() != 1)
	{
		return nullptr;
	}
	auto* definition = dyn_cast<StoreInst>((*definitions.begin())->def);
	if (definition == nullptr)
	{
		return nullptr;
	}
	auto* anchor = dyn_cast<AllocaInst>(
			llvm_utils::skipCasts(definition->getValueOperand()));
	return anchor != nullptr && config->isStackVariable(anchor) ? anchor : nullptr;
}

Value* cloneAddressExpression(
		Value* value,
		Value* base,
		Value* replacement,
		Instruction* before,
		std::map<Value*, Value*>& cloned)
{
	if (value == base)
	{
		return replacement;
	}
	auto found = cloned.find(value);
	if (found != cloned.end())
	{
		return found->second;
	}
	auto* instruction = dyn_cast<Instruction>(value);
	if (instruction == nullptr
			|| !(isa<CastInst>(instruction) || isa<BinaryOperator>(instruction)))
	{
		return value;
	}

	std::vector<Value*> operands;
	for (unsigned i = 0; i < instruction->getNumOperands(); ++i)
	{
		operands.push_back(cloneAddressExpression(
				instruction->getOperand(i), base, replacement, before, cloned));
	}
	auto* copy = instruction->clone();
	copy->setName(instruction->getName() + ".stack");
	copy->insertBefore(before);
	cloned[value] = copy;
	for (unsigned i = 0; i < operands.size(); ++i)
	{
		copy->setOperand(i, operands[i]);
	}
	return copy;
}

AffineStackAddress analyzeAffineStackAddress(
		Config* config,
		ReachingDefinitionsAnalysis& rda,
		Value* value)
{
	if (auto* ci = dyn_cast<ConstantInt>(value))
	{
		AffineStackAddress result;
		result.constant = ci->getSExtValue();
		return result;
	}

	if (auto* cast = dyn_cast<CastInst>(value))
	{
		if (isa<PtrToIntInst>(cast)
				&& getStackAnchor(config, rda, cast) != nullptr)
		{
			AffineStackAddress result;
			result.base = cast;
			result.baseCoefficient = 1;
			return result;
		}
		return analyzeAffineStackAddress(config, rda, cast->getOperand(0));
	}

	if (auto* load = dyn_cast<LoadInst>(value))
	{
		AffineStackAddress result;
		if (getStackAnchor(config, rda, load) != nullptr)
		{
			result.base = load;
			result.baseCoefficient = 1;
		}
		else
		{
			result.hasUnknown = true;
		}
		return result;
	}

	auto* binary = dyn_cast<BinaryOperator>(value);
	if (binary == nullptr)
	{
		AffineStackAddress result;
		result.hasUnknown = true;
		return result;
	}

	auto lhs = analyzeAffineStackAddress(config, rda, binary->getOperand(0));
	auto rhs = analyzeAffineStackAddress(config, rda, binary->getOperand(1));
	AffineStackAddress result;

	switch (binary->getOpcode())
	{
		case Instruction::Add:
		case Instruction::Sub:
		{
			int64_t rhsMultiplier = binary->getOpcode() == Instruction::Sub ? -1 : 1;
			result.valid = lhs.valid && rhs.valid
					&& mergeBase(result, lhs)
					&& mergeBase(result, rhs, rhsMultiplier);
			result.constant = lhs.constant + rhs.constant * rhsMultiplier;
			result.hasUnknown = lhs.hasUnknown || rhs.hasUnknown;
			return result;
		}
		case Instruction::Mul:
		{
			if (lhs.isConstant())
			{
				result = rhs;
				result.baseCoefficient *= lhs.constant;
				result.constant *= lhs.constant;
				return result;
			}
			if (rhs.isConstant())
			{
				result = lhs;
				result.baseCoefficient *= rhs.constant;
				result.constant *= rhs.constant;
				return result;
			}
			result.valid = lhs.base == nullptr && rhs.base == nullptr;
			result.hasUnknown = true;
			return result;
		}
		case Instruction::Shl:
		{
			if (rhs.isConstant() && rhs.constant >= 0 && rhs.constant < 63)
			{
				result = lhs;
				int64_t multiplier = int64_t{1} << rhs.constant;
				result.baseCoefficient *= multiplier;
				result.constant *= multiplier;
				return result;
			}
			result.valid = lhs.base == nullptr;
			result.hasUnknown = true;
			return result;
		}
		default:
			result.valid = lhs.base == nullptr && rhs.base == nullptr;
			result.hasUnknown = true;
			return result;
	}
}

} // anonymous namespace

char StackAnalysis::ID = 0;

static RegisterPass<StackAnalysis> X(
		"retdec-stack",
		"Stack optimization",
		false, // Only looks at CFG
		false // Analysis Pass
);

StackAnalysis::StackAnalysis() :
		ModulePass(ID)
{

}

bool StackAnalysis::runOnModule(llvm::Module& m)
{
	_module = &m;
	_config = ConfigProvider::getConfig(_module);
	_abi = AbiProvider::getAbi(_module);
	_dbgf = DebugFormatProvider::getDebugFormat(_module);
	return run();
}

bool StackAnalysis::runOnModuleCustom(
		llvm::Module& m,
		Config* c,
		Abi* abi,
		DebugFormat* dbgf)
{
	_module = &m;
	_config = c;
	_abi = abi;
	_dbgf = dbgf;
	return run();
}

bool StackAnalysis::run()
{
	if (_config == nullptr)
	{
		return false;
	}

	ReachingDefinitionsAnalysis RDA;
	RDA.runOnModule(*_module, _abi);
	_decodedStackDeltas.clear();

	for (auto& f : *_module)
	{
		computeDecodedStackDeltas(f);
		std::map<Value*, Value*> val2val;
		for (inst_iterator I = inst_begin(f), E = inst_end(f); I != E;)
		{
			Instruction& i = *I;
			++I;

			if (StoreInst *store = dyn_cast<StoreInst>(&i))
			{
				if (AsmInstruction::isLlvmToAsmInstruction(store))
				{
					continue;
				}

				handleInstruction(
						RDA,
						store,
						store->getValueOperand(),
						store->getValueOperand()->getType(),
						val2val);

				if (isa<GlobalVariable>(store->getPointerOperand()))
				{
					continue;
				}

				handleInstruction(
						RDA,
						store,
						store->getPointerOperand(),
						store->getValueOperand()->getType(),
						val2val);
			}
			else if (LoadInst* load = dyn_cast<LoadInst>(&i))
			{
				if (isa<GlobalVariable>(load->getPointerOperand()))
				{
					continue;
				}

				handleInstruction(
						RDA,
						load,
						load->getPointerOperand(),
						load->getType(),
						val2val);
			}
		}
	}

	IrModifier::eraseUnusedInstructionsRecursive(_toRemove);

	return reconstructDynamicStackAccesses();
}

/**
 * Compute each decoded instruction's stack-pointer delta from function entry.
 *
 * Symbolic reaching definitions intentionally fail at loop joins when several
 * stores reach @esp, even when every path has the same native stack effect.
 * Decoded push/pop instructions retain that machine-level invariant.  Track it
 * independently, but only through instructions whose effect is certain.  A
 * conflicting CFG merge or an unknown stack-pointer write poisons the path.
 */
void StackAnalysis::computeDecodedStackDeltas(Function& function)
{
	for (BasicBlock& block : function)
	{
		for (Instruction& instruction : block)
		{
			if (auto* marker = dyn_cast<StoreInst>(&instruction))
			{
				_decodedStackDeltas.erase(marker);
			}
		}
	}
	if (function.empty() || !_config->getConfig().architecture.isX86_32())
	{
		return;
	}

	std::map<BasicBlock*, StackDelta> entries;
	entries[&function.getEntryBlock()] = {StackDeltaKind::Known, 0};
	std::deque<BasicBlock*> worklist{&function.getEntryBlock()};
	std::set<BasicBlock*> queued{&function.getEntryBlock()};
	const int64_t slotSize = _config->getConfig().architecture.getByteSize();

	while (!worklist.empty())
	{
		BasicBlock* block = worklist.front();
		worklist.pop_front();
		queued.erase(block);
		StackDelta delta = entries[block];

		for (Instruction& instruction : *block)
		{
			auto* marker = dyn_cast<StoreInst>(&instruction);
			if (marker == nullptr
					|| !AsmInstruction::isLlvmToAsmInstruction(marker))
			{
				continue;
			}
			_decodedStackDeltas.erase(marker);
			if (delta.kind == StackDeltaKind::Known)
			{
				_decodedStackDeltas[marker] = delta.value;
			}

			AsmInstruction decoded(marker);
			cs_insn* capstone = decoded.getCapstoneInsn();
			if (delta.kind != StackDeltaKind::Known || capstone == nullptr
					|| capstone->detail == nullptr)
			{
				delta.kind = StackDeltaKind::Ambiguous;
				continue;
			}

			switch (capstone->id)
			{
				case X86_INS_PUSH:
					adjustStackDelta(delta, -slotSize);
					continue;
				case X86_INS_POP:
				{
					auto& operands = capstone->detail->x86;
					if (operands.op_count != 0
							&& operands.operands[0].type == X86_OP_REG
							&& operands.operands[0].reg == X86_REG_ESP)
					{
						delta.kind = StackDeltaKind::Ambiguous;
					}
					else
					{
						adjustStackDelta(delta, slotSize);
					}
					continue;
				}
				case X86_INS_CALL:
				{
					CallInst* call = decoded.getInstructionFirst<CallInst>();
					auto* callee = call == nullptr ? nullptr : call->getCalledFunction();
					auto* configFunction = callee == nullptr
							? nullptr : _config->getConfigFunction(callee);
					auto& operands = capstone->detail->x86;
					if (configFunction == nullptr && operands.op_count == 1
							&& operands.operands[0].type == X86_OP_IMM)
					{
						configFunction = _config->getConfigFunction(
								retdec::common::Address(operands.operands[0].imm));
					}
					bool callerClean = configFunction != nullptr
							&& (configFunction->callingConvention.isCdecl()
									|| configFunction->callingConvention.isEllipsis()
									|| configFunction->callingConvention.isVoidarg()
									|| (configFunction->callingConvention.isUnknown()
											&& _abi->getDefaultCallingConventionID()
													== CallingConvention::ID::CC_CDECL));
					if (!callerClean)
					{
						delta.kind = StackDeltaKind::Ambiguous;
					}
					continue;
				}
				default:
					break;
			}

			auto& x86 = capstone->detail->x86;
			bool writesStackPointer = false;
			for (unsigned i = 0; i < x86.op_count; ++i)
			{
				auto& operand = x86.operands[i];
				writesStackPointer |= operand.type == X86_OP_REG
						&& operand.reg == X86_REG_ESP
						&& (operand.access & CS_AC_WRITE) != 0;
			}
			if (!writesStackPointer)
			{
				continue;
			}

			if ((capstone->id == X86_INS_ADD || capstone->id == X86_INS_SUB)
					&& x86.op_count == 2
					&& x86.operands[0].type == X86_OP_REG
					&& x86.operands[0].reg == X86_REG_ESP
					&& x86.operands[1].type == X86_OP_IMM)
			{
				int64_t adjustment = x86.operands[1].imm;
				if (capstone->id == X86_INS_SUB
						&& adjustment == std::numeric_limits<int64_t>::min())
				{
					delta.kind = StackDeltaKind::Ambiguous;
				}
				else
				{
					adjustStackDelta(delta, capstone->id == X86_INS_ADD
							? adjustment : -adjustment);
				}
			}
			else
			{
				delta.kind = StackDeltaKind::Ambiguous;
			}
		}

		for (BasicBlock* successor : successors(block))
		{
			if (mergeStackDelta(entries[successor], delta)
					&& queued.insert(successor).second)
			{
				worklist.push_back(successor);
			}
		}
	}
}

std::optional<int> StackAnalysis::getDecodedIncomingStackOffset(
		Instruction* instruction) const
{
	if (!isa<LoadInst>(instruction)
			|| !_config->getConfig().architecture.isX86_32())
	{
		return std::nullopt;
	}
	AsmInstruction decoded(instruction);
	auto found = _decodedStackDeltas.find(decoded.getLlvmToAsmInstruction());
	cs_insn* capstone = decoded.getCapstoneInsn();
	if (found == _decodedStackDeltas.end() || capstone == nullptr
			|| capstone->detail == nullptr)
	{
		return std::nullopt;
	}

	const cs_x86_op* stackMemory = nullptr;
	auto& x86 = capstone->detail->x86;
	for (unsigned i = 0; i < x86.op_count; ++i)
	{
		auto& operand = x86.operands[i];
		if (operand.type != X86_OP_MEM || operand.mem.base != X86_REG_ESP
				|| operand.mem.index != X86_REG_INVALID
				|| ((operand.access & CS_AC_READ) == 0 && operand.access != 0))
		{
			continue;
		}
		if (stackMemory != nullptr)
		{
			return std::nullopt;
		}
		stackMemory = &operand;
	}
	std::optional<int64_t> displacement;
	if (stackMemory != nullptr)
	{
		displacement = stackMemory->mem.disp;
	}
	else if (capstone->id == X86_INS_POP
			&& x86.op_count == 1
			&& x86.operands[0].type == X86_OP_REG
			&& x86.operands[0].reg != X86_REG_ESP)
	{
		// Capstone represents POP's stack source implicitly. The decoded load
		// still reads [ESP] before the instruction advances ESP, so associate
		// it with the stack delta recorded at the instruction boundary. POP ESP
		// is intentionally excluded because it replaces the stack pointer with
		// the loaded value and makes subsequent stack state ambiguous.
		displacement = 0;
	}
	if (!displacement)
	{
		return std::nullopt;
	}

	int64_t offset = found->second + *displacement;
	if (offset < std::numeric_limits<int>::min()
			|| offset > std::numeric_limits<int>::max())
	{
		return std::nullopt;
	}
	int narrowedOffset = static_cast<int>(offset);
	int64_t firstIncomingOffset =
			_config->getConfig().architecture.getByteSize();
	// A positive offset beyond the return address is structurally an incoming
	// stack slot, even before parameter recovery has materialized an object for
	// it.  Non-incoming offsets still require an existing configured object.
	if (offset < firstIncomingOffset
			&& getConfigStackVariable(
					instruction->getFunction(), narrowedOffset) == nullptr)
	{
		return std::nullopt;
	}
	return narrowedOffset;
}

const retdec::common::Object* StackAnalysis::getConfigStackVariable(
		Function* function,
		int offset) const
{
	auto* configFunction = _config->getConfigFunction(function);
	if (configFunction == nullptr)
	{
		return nullptr;
	}
	for (auto& variable : configFunction->locals)
	{
		if (variable.getStorage().isStack()
				&& variable.getStorage().getStackOffset() == offset)
		{
			return &variable;
		}
	}
	return nullptr;
}

/**
 * Reconstruct non-constant stack addresses such as
 * @code [frame_pointer + index * scale + displacement] @endcode.
 *
 * Constant stack accesses have already been changed to independent allocas by
 * the first phase. Leaving an indexed access based on the emulated frame
 * register makes that access point outside an unrelated scalar alloca once the
 * IR is compiled. If its constant base coincides with a known stack object,
 * rebase the register value on that object and grow the object up to the next
 * known stack object boundary.
 */
bool StackAnalysis::reconstructDynamicStackAccesses()
{
	ReachingDefinitionsAnalysis rda;
	rda.runOnModule(*_module, _abi);

	bool changed = false;
	std::vector<std::pair<Instruction*, Value*>> accesses;
	for (Function& function : *_module)
	{
		bool functionChanged = false;
		accesses.clear();
		for (Instruction& instruction : instructions(function))
		{
			if (auto* load = dyn_cast<LoadInst>(&instruction))
			{
				if (!isa<GlobalVariable>(load->getPointerOperand())
						&& !isa<AllocaInst>(load->getPointerOperand()))
				{
					accesses.emplace_back(load, load->getPointerOperand());
				}
			}
			else if (auto* store = dyn_cast<StoreInst>(&instruction))
			{
				if (!isa<GlobalVariable>(store->getPointerOperand())
						&& !isa<AllocaInst>(store->getPointerOperand()))
				{
					accesses.emplace_back(store, store->getPointerOperand());
				}
			}
		}

		for (auto& access : accesses)
		{
			Instruction* memoryInstruction = access.first;
			Value* pointer = access.second;
			auto address = analyzeAffineStackAddress(_config, rda, pointer);
			if (!address.valid || address.base == nullptr
					|| address.baseCoefficient != 1 || !address.hasUnknown)
			{
				continue;
			}

			auto* anchor = getStackAnchor(_config, rda, address.base);
			if (anchor == nullptr)
			{
				continue;
			}

			auto anchorOffset = _config->getStackVariableOffset(anchor);
			if (!anchorOffset)
			{
				continue;
			}
			int64_t targetOffset64 = int64_t(*anchorOffset)
					+ address.constant;
			if (targetOffset64 < std::numeric_limits<int>::min()
					|| targetOffset64 > std::numeric_limits<int>::max())
			{
				continue;
			}
			int targetOffset = static_cast<int>(targetOffset64);
			auto* target = _config->getLlvmStackVariable(&function, targetOffset);
			if (target == nullptr)
			{
				continue;
			}

			std::optional<int> nextOffset;
			for (Instruction& entryInstruction : function.getEntryBlock())
			{
				auto* candidate = dyn_cast<AllocaInst>(&entryInstruction);
				if (candidate == nullptr)
				{
					continue;
				}
				auto offset = _config->getStackVariableOffset(candidate);
				if (offset && *offset > targetOffset
						&& (!nextOffset || *offset < *nextOffset))
				{
					nextOffset = *offset;
				}
			}
			if (!nextOffset)
			{
				continue;
			}

			uint64_t extent = uint64_t(*nextOffset - targetOffset);
			uint64_t elementSize = _module->getDataLayout().getTypeAllocSize(
					target->getAllocatedType());
			if (elementSize == 0)
			{
				continue;
			}
			uint64_t requiredElements = (extent + elementSize - 1) / elementSize;
			auto* currentElements = dyn_cast<ConstantInt>(target->getArraySize());
			if (currentElements == nullptr)
			{
				continue;
			}
			if (requiredElements > currentElements->getZExtValue())
			{
				target->setOperand(0, ConstantInt::get(
						currentElements->getType(), requiredElements));
			}

			IRBuilder<> builder(memoryInstruction);
			Value* targetAddress = builder.CreatePtrToInt(
					target, address.base->getType(), target->getName() + ".base");
			int64_t adjustment = int64_t(*anchorOffset) - targetOffset;
			if (adjustment != 0)
			{
				targetAddress = builder.CreateAdd(
						targetAddress,
						ConstantInt::getSigned(
								cast<IntegerType>(address.base->getType()), adjustment),
						target->getName() + ".anchor");
			}
			std::map<Value*, Value*> cloned;
			Value* reconstructedPointer = cloneAddressExpression(
					pointer,
					address.base,
					targetAddress,
					memoryInstruction,
					cloned);
			memoryInstruction->replaceUsesOfWith(pointer, reconstructedPointer);
			changed = true;
			functionChanged = true;
		}
		if (functionChanged)
		{
			// Parameter and type recovery expect independent allocas. Mark this
			// function now and lower its recovered objects into one aliased frame
			// in the later stack-frame pass.
			function.addFnAttr("retdec.stack.frame");
		}
	}

	return changed;
}

void StackAnalysis::handleInstruction(
		ReachingDefinitionsAnalysis& RDA,
		llvm::Instruction* inst,
		llvm::Value* val,
		llvm::Type* type,
		std::map<llvm::Value*, llvm::Value*>& val2val)
{
	LOG << llvmObjToString(inst) << std::endl;

	auto root = SymbolicTree::PrecomputedRdaWithValueMap(RDA, val, &val2val);
	LOG << root << std::endl;
	auto decodedOffset = getDecodedIncomingStackOffset(inst);

	if (!root.isVal2ValMapUsed())
	{
		bool stackPtr = false;
		for (SymbolicTree* n : root.getPostOrder())
		{
			if (_abi->isStackPointerRegister(n->value))
			{
				stackPtr = true;
				break;
			}
		}
		if (!stackPtr && !decodedOffset)
		{
			LOG << "===> no SP" << std::endl;
			return;
		}
	}

	auto* debugSv = getDebugStackVariable(inst->getFunction(), root);
	auto* configSv = getConfigStackVariable(inst->getFunction(), root);

	root.simplifyNode();
	LOG << root << std::endl;

	if (debugSv == nullptr)
	{
		debugSv = getDebugStackVariable(inst->getFunction(), root);
	}

	if (configSv == nullptr)
	{
		configSv = getConfigStackVariable(inst->getFunction(), root);
	}

	auto* ci = dyn_cast_or_null<ConstantInt>(root.value);
	if (ci == nullptr)
	{
		if (decodedOffset)
		{
			ci = ConstantInt::getSigned(
					Type::getInt64Ty(_module->getContext()), *decodedOffset);
			if (configSv == nullptr)
			{
				configSv = getConfigStackVariable(
						inst->getFunction(), *decodedOffset);
			}
		}
	}
	if (ci == nullptr)
	{
		return;
	}

	if (auto* s = dyn_cast<StoreInst>(inst))
	{
		if (s->getValueOperand() == val)
		{
			val2val[inst] = ci;
		}
	}

	LOG << "===> " << llvmObjToString(ci) << std::endl;
	LOG << "===> " << ci->getSExtValue() << std::endl;

	std::string name = "";
	Type* t = type;

	if (debugSv)
	{
		name = debugSv->getName();
		t = llvm_utils::stringToLlvmTypeDefault(_module, debugSv->type.getLlvmIr());
	}
	else if (configSv)
	{
		name = configSv->getName();
		t = llvm_utils::stringToLlvmTypeDefault(_module, configSv->type.getLlvmIr());
	}

	std::string realName;
	if (debugSv)
	{
		realName = debugSv->getName();
	}
	else if (configSv)
	{
		realName = configSv->getName();
	}

	IrModifier irModif(_module, _config);
	auto p = irModif.getStackVariable(
			inst->getFunction(),
			ci->getSExtValue(),
			t,
			name,
			realName,
			debugSv || configSv);

	AllocaInst* a = p.first;

	LOG << "===> " << llvmObjToString(a) << std::endl;
	LOG << "===> " << llvmObjToString(inst) << std::endl;
	LOG << std::endl;

	auto* s = dyn_cast<StoreInst>(inst);
	auto* l = dyn_cast<LoadInst>(inst);
	if (s && s->getPointerOperand() == val)
	{
		// The alloca type describes one recovered view of the stack object; it
		// must not change the width or bit semantics of another decoded access at
		// the same byte offset. Cast the address, not the stored machine value.
		auto* pointer = IrModifier::convertValueToType(
				a, s->getPointerOperand()->getType(), inst);
		new StoreInst(s->getValueOperand(), pointer, inst);
		_toRemove.insert(s);
	}
	else if (l && l->getPointerOperand() == val)
	{
		// Preserve the decoded read width for overlapping stack views (such as a
		// DWORD load from the low half of a QWORD temporary).
		auto* pointer = IrModifier::convertValueToType(
				a, l->getPointerOperand()->getType(), inst);
		auto* nl = new LoadInst(pointer, "", l);
		nl->takeName(l);
		l->replaceAllUsesWith(nl);
		_toRemove.insert(l);
	}
	else
	{
		auto* conv = IrModifier::convertValueToType(a, val->getType(), inst);
		_toRemove.insert(val);
		inst->replaceUsesOfWith(val, conv);
	}
}

std::optional<int> StackAnalysis::getBaseOffset(SymbolicTree& root)
{
	std::optional<int> baseOffset;
	if (auto* ci = dyn_cast_or_null<ConstantInt>(root.value))
	{
		baseOffset = ci->getSExtValue();
	}
	else
	{
		for (SymbolicTree* n : root.getLevelOrder())
		{
			if (isa<AddOperator>(n->value)
					&& n->ops.size() == 2
					&& isa<LoadInst>(n->ops[0].value)
					&& isa<ConstantInt>(n->ops[1].value))
			{
				auto* l = cast<LoadInst>(n->ops[0].value);
				auto* ci = cast<ConstantInt>(n->ops[1].value);
				if (_abi->isRegister(l->getPointerOperand()))
				{
					baseOffset = ci->getSExtValue();
				}
				break;
			}
		}
	}

	return baseOffset;
}

/**
 * Find a value that is being added to the stack pointer register in \p root.
 * Find a debug variable with offset equal to this value.
 */
const retdec::common::Object* StackAnalysis::getDebugStackVariable(
		llvm::Function* fnc,
		SymbolicTree& root)
{
	auto baseOffset = getBaseOffset(root);
	if (!baseOffset.has_value())
	{
		return nullptr;
	}

	if (_dbgf == nullptr)
	{
		return nullptr;
	}

	auto* debugFnc = _dbgf->getFunction(_config->getFunctionAddress(fnc));
	if (debugFnc == nullptr)
	{
		return nullptr;
	}

	for (auto& var : debugFnc->locals)
	{
		if (!var.getStorage().isStack())
		{
			continue;
		}
		if (var.getStorage().getStackOffset() == baseOffset)
		{
			return &var;
		}
	}

	return nullptr;
}

const retdec::common::Object* StackAnalysis::getConfigStackVariable(
		llvm::Function* fnc,
		SymbolicTree& root)
{
	auto baseOffset = getBaseOffset(root);
	if (!baseOffset.has_value())
	{
		return nullptr;
	}

	auto cfn = _config->getConfigFunction(fnc);
	if (cfn && _config->getLlvmStackVariable(fnc, baseOffset.value()) == nullptr)
	{
		for (auto& var: cfn->locals)
		{
			if (var.getStorage().getStackOffset() == baseOffset)
			{
				return &var;
			}
		}
	}

	return nullptr;
}

} // namespace bin2llvmir
} // namespace retdec
