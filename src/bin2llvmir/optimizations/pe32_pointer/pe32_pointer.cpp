/**
 * @file src/bin2llvmir/optimizations/pe32_pointer/pe32_pointer.cpp
 * @brief Preserve the width of pointer-valued PE32 memory cells.
 * @copyright (c) 2026 Avast Software, licensed under the MIT license
 */

#include <algorithm>
#include <cstdint>
#include <vector>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

#include "retdec/bin2llvmir/optimizations/pe32_pointer/pe32_pointer.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

char Pe32PointerLegalization::ID = 0;
char Pe32PointerBridge::ID = 0;

static RegisterPass<Pe32PointerLegalization> X(
		"retdec-pe32-pointer-cells",
		"preserve PE32 guest pointer cell width",
		false,
		false);

static RegisterPass<Pe32PointerBridge> Y(
		"retdec-pe32-pointer-bridge",
		"translate pointers between a PE32 guest and a native host",
		false,
		false);

Pe32PointerLegalization::Pe32PointerLegalization() :
		ModulePass(ID)
{
}

Pe32PointerBridge::Pe32PointerBridge() :
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

bool Pe32PointerBridge::runOnModule(Module& module)
{
	return runOnModuleCustom(module, ConfigProvider::getConfig(&module));
}

bool Pe32PointerBridge::runOnModuleCustom(Module& module, Config* config)
{
	_module = &module;
	_config = config;
	return run();
}

namespace {

Function* getOrCreateBridgeFunction(
		Module* module,
		StringRef name,
		FunctionType* type)
{
	if (auto* existing = module->getFunction(name))
	{
		return existing->getFunctionType() == type ? existing : nullptr;
	}
	return Function::Create(
			type, GlobalValue::ExternalLinkage, name, module);
}

struct NativeObjectExtent
{
	Value* base;
	uint64_t size;
};

NativeObjectExtent getNativeObjectExtent(Module* module, Value* pointer)
{
	auto& layout = module->getDataLayout();
	Value* base = GetUnderlyingObject(pointer, layout);
	uint64_t size = 1;
	if (auto* alloca = dyn_cast<AllocaInst>(base))
	{
		auto* count = dyn_cast<ConstantInt>(alloca->getArraySize());
		if (count != nullptr && alloca->getAllocatedType()->isSized())
		{
			size = layout.getTypeAllocSize(alloca->getAllocatedType());
			if (count->getZExtValue() <= UINT32_MAX / std::max(size, uint64_t{1}))
			{
				size *= count->getZExtValue();
			}
			else
			{
				size = 1;
			}
		}
	}
	else if (auto* global = dyn_cast<GlobalVariable>(base))
	{
		if (global->getValueType()->isSized())
		{
			size = layout.getTypeAllocSize(global->getValueType());
		}
	}
	return {base, std::min(size, uint64_t{UINT32_MAX})};
}

Value* pointerAsBytePointer(IRBuilder<>& builder, Value* pointer)
{
	auto* bytePointer = Type::getInt8PtrTy(pointer->getContext());
	return pointer->getType() == bytePointer
			? pointer
			: builder.CreateBitCast(pointer, bytePointer);
}

} // anonymous namespace

bool Pe32PointerBridge::run()
{
	if (_config == nullptr
			|| !_config->getConfig().fileFormat.isPe()
			|| !_config->getConfig().architecture.isX86()
			|| _config->getConfig().architecture.getBitSize() != 32)
	{
		return false;
	}

	auto& context = _module->getContext();
	auto* int32 = Type::getInt32Ty(context);
	std::vector<PtrToIntInst*> hostEscapes;
	std::vector<IntToPtrInst*> guestDereferences;
	std::vector<AddrSpaceCastInst*> addressSpaceCasts;
	for (Function& function : *_module)
	{
		if (function.getName() == "__retdec_pe32_host_to_guest"
				|| function.getName() == "__retdec_pe32_guest_to_host")
		{
			continue;
		}
		for (Instruction& instruction : instructions(function))
		{
			if (auto* cast = dyn_cast<PtrToIntInst>(&instruction))
			{
				auto* sourceType = llvm::cast<PointerType>(
						cast->getPointerOperand()->getType());
				if (sourceType->getAddressSpace() == 0
						&& cast->getType() == int32)
				{
					hostEscapes.push_back(cast);
				}
			}
			else if (auto* cast = dyn_cast<IntToPtrInst>(&instruction))
			{
				auto* destinationType = llvm::cast<PointerType>(cast->getType());
				if (destinationType->getAddressSpace() == 0
						&& cast->getOperand(0)->getType() == int32)
				{
					guestDereferences.push_back(cast);
				}
			}
			else if (auto* cast = dyn_cast<AddrSpaceCastInst>(&instruction))
			{
				auto* sourceType = llvm::cast<PointerType>(cast->getSrcTy());
				auto* destinationType = llvm::cast<PointerType>(cast->getDestTy());
				if ((sourceType->getAddressSpace() == 0
							&& destinationType->getAddressSpace()
									== Pe32PointerLegalization::GuestPointerAddressSpace)
						|| (sourceType->getAddressSpace()
									== Pe32PointerLegalization::GuestPointerAddressSpace
							&& destinationType->getAddressSpace() == 0))
				{
					addressSpaceCasts.push_back(cast);
				}
			}
		}
	}
	if (hostEscapes.empty()
			&& guestDereferences.empty()
			&& addressSpaceCasts.empty())
	{
		return false;
	}

	auto* bytePointer = Type::getInt8PtrTy(context);
	auto* hostToGuest = getOrCreateBridgeFunction(
			_module,
			"__retdec_pe32_host_to_guest",
			FunctionType::get(
					int32, {bytePointer, bytePointer, int32}, false));
	auto* guestToHost = getOrCreateBridgeFunction(
			_module,
			"__retdec_pe32_guest_to_host",
			FunctionType::get(bytePointer, {int32}, false));
	if (hostToGuest == nullptr || guestToHost == nullptr)
	{
		return false;
	}

	for (PtrToIntInst* escape : hostEscapes)
	{
		IRBuilder<> builder(escape);
		Value* pointer = escape->getPointerOperand();
		auto extent = getNativeObjectExtent(_module, pointer);
		auto* call = builder.CreateCall(
				hostToGuest,
				{pointerAsBytePointer(builder, pointer),
				 pointerAsBytePointer(builder, extent.base),
				 ConstantInt::get(int32, extent.size)},
				escape->getName() + ".guest");
		call->setDebugLoc(escape->getDebugLoc());
		escape->replaceAllUsesWith(call);
		escape->eraseFromParent();
	}

	for (IntToPtrInst* dereference : guestDereferences)
	{
		IRBuilder<> builder(dereference);
		auto* call = builder.CreateCall(
				guestToHost,
				{dereference->getOperand(0)},
				dereference->getName() + ".host");
		call->setDebugLoc(dereference->getDebugLoc());
		Value* pointer = builder.CreateBitCast(
				call, dereference->getType(), dereference->getName());
		dereference->replaceAllUsesWith(pointer);
		dereference->eraseFromParent();
	}

	for (AddrSpaceCastInst* cast : addressSpaceCasts)
	{
		IRBuilder<> builder(cast);
		auto* sourceType = llvm::cast<PointerType>(cast->getSrcTy());
		Value* replacement = nullptr;
		if (sourceType->getAddressSpace() == 0)
		{
			Value* pointer = cast->getOperand(0);
			auto extent = getNativeObjectExtent(_module, pointer);
			auto* guest = builder.CreateCall(
					hostToGuest,
					{pointerAsBytePointer(builder, pointer),
					 pointerAsBytePointer(builder, extent.base),
					 ConstantInt::get(int32, extent.size)});
			replacement = builder.CreateIntToPtr(guest, cast->getType());
		}
		else
		{
			auto* guest = builder.CreatePtrToInt(cast->getOperand(0), int32);
			auto* host = builder.CreateCall(guestToHost, {guest});
			replacement = builder.CreateBitCast(host, cast->getType());
		}
		replacement->takeName(cast);
		cast->replaceAllUsesWith(replacement);
		cast->eraseFromParent();
	}

	return !hostEscapes.empty()
			|| !guestDereferences.empty()
			|| !addressSpaceCasts.empty();
}

} // namespace bin2llvmir
} // namespace retdec
