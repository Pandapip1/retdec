/**
 * @file src/bin2llvmir/optimizations/pe32_pointer/pe32_pointer.cpp
 * @brief Preserve the width of pointer-valued PE32 memory cells.
 * @copyright (c) 2026 Avast Software, licensed under the MIT license
 */

#include <vector>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

#include "retdec/bin2llvmir/optimizations/pe32_pointer/pe32_pointer.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

char Pe32PointerLegalization::ID = 0;

static RegisterPass<Pe32PointerLegalization> X(
		"retdec-pe32-pointer-cells",
		"preserve PE32 guest pointer cell width",
		false,
		false);

Pe32PointerLegalization::Pe32PointerLegalization() :
		ModulePass(ID)
{
}

bool Pe32PointerLegalization::runOnModule(Module& module)
{
	return runOnModuleCustom(module, ConfigProvider::getConfig(&module));
}

bool Pe32PointerLegalization::runOnModuleCustom(
		Module& module,
		Config* config)
{
	_module = &module;
	_config = config;
	return run();
}

namespace {

template<typename T>
void copyMemoryProperties(const T& from, T& to)
{
	to.setVolatile(from.isVolatile());
	to.setAlignment(from.getAlignment());
	to.setOrdering(from.getOrdering());
	to.setSyncScopeID(from.getSyncScopeID());
	to.copyMetadata(from);
	to.setDebugLoc(from.getDebugLoc());
}

} // anonymous namespace

bool Pe32PointerLegalization::run()
{
	if (_config == nullptr
			|| !_config->getConfig().fileFormat.isPe()
			|| !_config->getConfig().architecture.isX86()
			|| _config->getConfig().architecture.getBitSize() != 32)
	{
		return false;
	}

	std::vector<LoadInst*> loads;
	std::vector<StoreInst*> stores;
	for (Function& function : *_module)
	for (Instruction& instruction : instructions(function))
	{
		if (auto* load = dyn_cast<LoadInst>(&instruction))
		{
			if (load->getType()->isPointerTy()
					&& load->getType()->getPointerAddressSpace() == 0)
			{
				loads.push_back(load);
			}
		}
		else if (auto* store = dyn_cast<StoreInst>(&instruction))
		{
			auto* valueType = store->getValueOperand()->getType();
			if (valueType->isPointerTy()
					&& valueType->getPointerAddressSpace() == 0)
			{
				stores.push_back(store);
			}
		}
	}

	for (LoadInst* load : loads)
	{
		IRBuilder<> builder(load);
		auto* pointerType = cast<PointerType>(load->getType());
		auto* guestPointerType = PointerType::get(
				pointerType->getPointerElementType(),
				GuestPointerAddressSpace);
		auto* guestCellType = PointerType::get(guestPointerType, 0);
		auto* guestCell = builder.CreateBitCast(
				load->getPointerOperand(), guestCellType,
				load->getName() + ".guest.cell");
		auto* guestLoad = builder.CreateLoad(
				guestCell, load->getName() + ".guest");
		copyMemoryProperties(*load, *guestLoad);
		auto* nativePointer = builder.CreateAddrSpaceCast(
				guestLoad, load->getType(), load->getName() + ".native");
		load->replaceAllUsesWith(nativePointer);
		load->eraseFromParent();
	}

	for (StoreInst* store : stores)
	{
		IRBuilder<> builder(store);
		auto* pointerType = cast<PointerType>(
				store->getValueOperand()->getType());
		auto* guestPointerType = PointerType::get(
				pointerType->getPointerElementType(),
				GuestPointerAddressSpace);
		auto* guestPointer = builder.CreateAddrSpaceCast(
				store->getValueOperand(), guestPointerType,
				store->getValueOperand()->getName() + ".guest");
		auto* guestCellType = PointerType::get(guestPointerType, 0);
		auto* guestCell = builder.CreateBitCast(
				store->getPointerOperand(), guestCellType,
				"guest.pointer.cell");
		auto* guestStore = builder.CreateStore(guestPointer, guestCell);
		copyMemoryProperties(*store, *guestStore);
		store->eraseFromParent();
	}

	return !loads.empty() || !stores.empty();
}

} // namespace bin2llvmir
} // namespace retdec
