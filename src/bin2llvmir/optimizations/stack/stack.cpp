/**
* @file src/bin2llvmir/optimizations/stack/stack.cpp
* @brief Reconstruct stack.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
*/

#include <limits>

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
	LoadInst* base = nullptr;
	int64_t baseCoefficient = 0;
	int64_t constant = 0;
	bool hasUnknown = false;
	bool valid = true;

	bool isConstant() const
	{
		return valid && base == nullptr && !hasUnknown;
	}
};

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
		LoadInst* load)
{
	if (!config->isRegister(load->getPointerOperand()))
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

	for (auto& f : *_module)
	{
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
			if (anchorOffset.isUndefined())
			{
				continue;
			}
			int64_t targetOffset64 = int64_t(anchorOffset.getValue())
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

			retdec::utils::Maybe<int> nextOffset;
			for (Instruction& entryInstruction : function.getEntryBlock())
			{
				auto* candidate = dyn_cast<AllocaInst>(&entryInstruction);
				if (candidate == nullptr)
				{
					continue;
				}
				auto offset = _config->getStackVariableOffset(candidate);
				if (offset.isDefined() && offset > targetOffset
						&& (nextOffset.isUndefined() || offset < nextOffset))
				{
					nextOffset = offset;
				}
			}
			if (nextOffset.isUndefined())
			{
				continue;
			}

			uint64_t extent = uint64_t(nextOffset.getValue() - targetOffset);
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
			int64_t adjustment = int64_t(anchorOffset.getValue()) - targetOffset;
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
		if (!stackPtr)
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
		auto* conv = IrModifier::convertValueToType(
				s->getValueOperand(),
				a->getType()->getElementType(),
				inst);
		new StoreInst(conv, a, inst);
		_toRemove.insert(s);
	}
	else if (l && l->getPointerOperand() == val)
	{
		auto* nl = new LoadInst(a, "", l);
		auto* conv = IrModifier::convertValueToType(nl, l->getType(), l);
		l->replaceAllUsesWith(conv);
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
