/**
* @file src/bin2llvmir/optimizations/stack_pointer_ops/stack_pointer_ops.cpp
* @brief Remove the remaining stack pointer operations.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
*/

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <iostream>
#include <limits>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>

#include "retdec/bin2llvmir/utils/llvm.h"
#include "retdec/utils/string.h"
#include "retdec/bin2llvmir/optimizations/stack_pointer_ops/stack_pointer_ops.h"
#include "retdec/bin2llvmir/utils/debug.h"
#include "retdec/bin2llvmir/utils/ir_modifier.h"

using namespace retdec::utils;
using namespace llvm;

#define debug_enabled false

namespace retdec {
namespace bin2llvmir {

namespace {

const char* STACK_FRAME_ATTRIBUTE = "retdec.stack.frame";
const char* STACK_FRAME_METADATA = "retdec.stack.variable";

/**
 * Put all recovered stack objects into one byte-addressable allocation after
 * parameter and type recovery have finished using independent allocas.
 */
bool coalesceStackFrame(Module* module, Config* config, Function& function)
{
	struct StackObject
	{
		AllocaInst* alloca;
		int offset;
	};

	std::vector<StackObject> objects;
	std::vector<AllocaInst*> allocas;
	for (Instruction& instruction : function.getEntryBlock())
	{
		if (auto* alloca = dyn_cast<AllocaInst>(&instruction))
		{
			allocas.push_back(alloca);
		}
	}

	// Simple type recovery may select a type wider than the machine stack
	// object recorded by the decoder.  That is harmless while each recovered
	// local has an independent alloca, but it changes the program once adjacent
	// objects are put back into their real, overlapping frame.  Restore the
	// configured element width before coalescing; IrModifier keeps the inferred
	// SSA type by inserting conversions around the original-width memory access.
	IrModifier modifier(module, config);
	for (auto*& alloca : allocas)
	{
		auto* stackObject = config->getConfigStackVariable(alloca);
		if (stackObject == nullptr || !stackObject->type.isDefined())
		{
			continue;
		}
		auto* configuredType = llvm_utils::stringToLlvmType(
				module->getContext(), stackObject->type.getLlvmIr());
		if (configuredType != nullptr && configuredType->isPointerTy())
		{
			configuredType = configuredType->getPointerElementType();
		}
		if (configuredType == nullptr || !configuredType->isSized()
				|| !alloca->getAllocatedType()->isSized())
		{
			continue;
		}
		if (module->getDataLayout().getTypeAllocSize(configuredType)
				< module->getDataLayout().getTypeAllocSize(
						alloca->getAllocatedType()))
		{
			alloca = cast<AllocaInst>(modifier.changeObjectType(
					nullptr, alloca, configuredType));
		}
	}

	int64_t minimumOffset = std::numeric_limits<int64_t>::max();
	int64_t maximumEnd = std::numeric_limits<int64_t>::min();
	unsigned maximumAlignment = 1;
	for (auto* alloca : allocas)
	{
		if (alloca->getName() == "stack_frame")
		{
			return false;
		}
		auto offset = config->getStackVariableOffset(alloca);
		auto* elements = dyn_cast<ConstantInt>(alloca->getArraySize());
		if (offset.isUndefined() || elements == nullptr)
		{
			continue;
		}
		uint64_t elementSize = module->getDataLayout().getTypeAllocSize(
				alloca->getAllocatedType());
		uint64_t elementCount = elements->getZExtValue();
		if (elementSize == 0
				|| elementCount > uint64_t(std::numeric_limits<int>::max()) / elementSize)
		{
			continue;
		}
		uint64_t size = elementSize * elementCount;
		int64_t end = int64_t(offset.getValue()) + int64_t(size);
		objects.push_back({alloca, offset.getValue()});
		minimumOffset = std::min(minimumOffset, int64_t(offset.getValue()));
		maximumEnd = std::max(maximumEnd, end);
		maximumAlignment = std::max(
				maximumAlignment,
				std::max(
						alloca->getAlignment(),
						module->getDataLayout().getABITypeAlignment(
								alloca->getAllocatedType())));
	}
	if (objects.empty())
	{
		return false;
	}

	int64_t remainder = minimumOffset % maximumAlignment;
	if (remainder < 0)
	{
		remainder += maximumAlignment;
	}
	int64_t frameStart = minimumOffset - remainder;
	uint64_t frameSize = uint64_t(maximumEnd - frameStart);
	auto* frameType = ArrayType::get(Type::getInt8Ty(module->getContext()), frameSize);
	auto* insertionPoint = &*function.getEntryBlock().getFirstInsertionPt();
	auto* frame = new AllocaInst(frameType, "stack_frame", insertionPoint);
	frame->setAlignment(maximumAlignment);

	IRBuilder<> builder(insertionPoint);
	auto* zero = ConstantInt::get(Type::getInt32Ty(module->getContext()), 0);
	for (const auto& object : objects)
	{
		auto* index = ConstantInt::get(
				Type::getInt32Ty(module->getContext()),
				uint64_t(int64_t(object.offset) - frameStart));
		Value* address = builder.CreateInBoundsGEP(
				frame,
				{zero, index},
				object.alloca->getName() + ".frame.addr");
		Value* alias = builder.CreateBitCast(
				address,
				object.alloca->getType(),
				object.alloca->getName() + ".frame");
		if (auto* aliasInstruction = dyn_cast<Instruction>(alias))
		{
			aliasInstruction->setMetadata(
					STACK_FRAME_METADATA,
					MDNode::get(
							module->getContext(),
							MDString::get(
									module->getContext(),
									object.alloca->getName())));
		}
		object.alloca->replaceAllUsesWith(alias);
	}
	return true;
}

} // anonymous namespace

char StackPointerOpsRemove::ID = 0;
char StackFrameCoalescing::ID = 0;

static RegisterPass<StackPointerOpsRemove> X(
		"stack-ptr-op-remove",
		"Stack pointer operations optimization",
		false, // Only looks at CFG
		false // Analysis Pass
);

static RegisterPass<StackFrameCoalescing> Y(
		"stack-frame",
		"Coalesce overlapping recovered stack objects",
		false,
		false
);

StackFrameCoalescing::StackFrameCoalescing() :
		ModulePass(ID)
{

}

bool StackFrameCoalescing::runOnModule(Module& M)
{
	_module = &M;
	_config = ConfigProvider::getConfig(&M);
	return run();
}

bool StackFrameCoalescing::runOnModuleCustom(Module& M, Config* c)
{
	_module = &M;
	_config = c;
	return run();
}

bool StackFrameCoalescing::run()
{
	if (_config == nullptr)
	{
		return false;
	}
	bool changed = false;
	for (Function& function : *_module)
	{
		if (!function.empty() && function.hasFnAttribute(STACK_FRAME_ATTRIBUTE))
		{
			changed |= coalesceStackFrame(_module, _config, function);
		}
	}
	return changed;
}

StackPointerOpsRemove::StackPointerOpsRemove() :
		ModulePass(ID)
{

}

bool StackPointerOpsRemove::runOnModule(Module& M)
{
	_module = &M;
	_config = ConfigProvider::getConfig(&M);
	return run();
}

bool StackPointerOpsRemove::runOnModuleCustom(llvm::Module& M, Config* c)
{
	_module = &M;
	_config = c;
	return run();
}

/**
 * @return @c True if module @a _module was modified in any way,
 *         @c false otherwise.
 */
bool StackPointerOpsRemove::run()
{
	bool changed = false;
	changed |= removeStackPointerStores();
	changed |= removePreservationStores();

	return changed;
}

bool StackPointerOpsRemove::removeStackPointerStores()
{
	if (_config == nullptr)
	{
		LOG << "[ABORT] config file is not available\n";
		return false;
	}

	bool changed = false;
	for (auto& F : _module->getFunctionList())
	for (auto& B : F)
	{
		auto it = B.begin();
		while (it != B.end())
		{
			// We need to move to the next instruction before optimizing
			// (potentially removing) the current instruction. Otherwise,
			// the iterator would become invalid.
			//
			auto* inst = &(*it);
			++it;

			if (StoreInst* s = dyn_cast<StoreInst>(inst))
			{
				auto* reg = s->getPointerOperand();
				if (!_config->isStackPointerRegister(reg))
				{
					continue;
				}

				LOG << "erase: " << llvmObjToString(inst) << std::endl;
				inst->eraseFromParent();
				changed = true;
			}
		}
	}

	return changed;
}

/**
 * Finds those allocas that are only used to store some value from ebp and then
 * this value is stored back to ebp.
 */
bool StackPointerOpsRemove::removePreservationStores()
{
	bool changed = false;

	for (auto& f : _module->getFunctionList())
	{
		for (inst_iterator I = inst_begin(f), E = inst_end(f); I != E; ++I)
		{
			auto* a = dyn_cast<AllocaInst>(&*I);
			if (a == nullptr)
			{
				continue;
			}

			bool remove = true;
			Value* storedVal = nullptr;
			std::set<llvm::Instruction*> toRemove;
			for (auto* u : a->users())
			{
				if (auto* s = dyn_cast<StoreInst>(u))
				{
					auto* l = dyn_cast<LoadInst>(s->getValueOperand());
					if (l && storedVal == nullptr
							&& (l->getPointerOperand()->getName() == "ebp" || l->getPointerOperand()->getName() == "rbp"))
					{
						storedVal = l->getPointerOperand();
						toRemove.insert(s);
					}
					else if (l && l->getPointerOperand() == storedVal)
					{
						toRemove.insert(s);
					}
					else
					{
						remove = false;
						break;
					}
				}
				else if (isa<LoadInst>(u) || isa<CastInst>(u))
				{
					for (auto* uu : u->users())
					{
						auto* s = dyn_cast<StoreInst>(uu);
						if (s && storedVal == nullptr
								&& (s->getPointerOperand()->getName() == "ebp" || s->getPointerOperand()->getName() == "rbp"))
						{
							storedVal = s->getPointerOperand();
							toRemove.insert(s);
						}
						else if (s && s->getPointerOperand() == storedVal)
						{
							toRemove.insert(s);
						}
						else
						{
							remove = false;
							break;
						}
					}

					if (!remove)
					{
						break;
					}
				}
				else
				{
					remove = false;
					break;
				}
			}

			if (remove)
			{
				for (auto* i : toRemove)
				{
					i->eraseFromParent();
					changed = true;
				}
			}
		}
	}

	return changed;
}

} // namespace bin2llvmir
} // namespace retdec
