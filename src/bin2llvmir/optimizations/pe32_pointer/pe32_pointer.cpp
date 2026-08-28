/**
 * @file src/bin2llvmir/optimizations/pe32_pointer/pe32_pointer.cpp
 * @brief Preserve the width of pointer-valued PE32 memory cells.
 * @copyright (c) 2026 Avast Software, licensed under the MIT license
 */

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include "retdec/bin2llvmir/optimizations/pe32_pointer/pe32_pointer.h"
#include "retdec/bin2llvmir/providers/fileimage.h"
#include "retdec/fileformat/file_format/pe/pe_format.h"

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

bool Pe32PointerBridge::enableInPipeline(std::vector<std::string>& passes)
{
	passes.erase(
			std::remove(
					passes.begin(), passes.end(),
					"retdec-pe32-pointer-bridge"),
			passes.end());
	auto legalization = std::find(
			passes.begin(), passes.end(), "retdec-pe32-pointer-cells");
	if (legalization == passes.end())
	{
		return false;
	}
	passes.insert(std::next(legalization), "retdec-pe32-pointer-bridge");
	return true;
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
	if (pointer->getType() == bytePointer)
	{
		return pointer;
	}
	auto* pointerType = cast<PointerType>(pointer->getType());
	return pointerType->getAddressSpace() == 0
			? builder.Insert(new BitCastInst(pointer, bytePointer))
			: builder.CreateAddrSpaceCast(pointer, bytePointer);
}

bool constantNeedsPointerBridge(Constant* constant)
{
	auto* expression = dyn_cast<ConstantExpr>(constant);
	if (expression == nullptr)
	{
		return false;
	}
	if (expression->getOpcode() == Instruction::PtrToInt
			&& expression->getType()->isIntegerTy(32)
			&& expression->getOperand(0)->getType()->getPointerAddressSpace() == 0)
	{
		return true;
	}
	if (expression->getOpcode() == Instruction::IntToPtr
			&& expression->getOperand(0)->getType()->isIntegerTy(32)
			&& expression->getType()->getPointerAddressSpace() == 0)
	{
		return true;
	}
	for (Value* operand : expression->operands())
	{
		if (auto* child = dyn_cast<Constant>(operand))
		{
			if (constantNeedsPointerBridge(child))
			{
				return true;
			}
		}
	}
	return false;
}

Instruction* materializePointerConstantExpression(
		ConstantExpr* expression,
		Instruction* before)
{
	auto* materialized = expression->getAsInstruction();
	materialized->insertBefore(before);
	for (unsigned i = 0; i < materialized->getNumOperands(); ++i)
	{
		auto* child = dyn_cast<ConstantExpr>(materialized->getOperand(i));
		if (child != nullptr && constantNeedsPointerBridge(child))
		{
			materialized->setOperand(
					i, materializePointerConstantExpression(child, materialized));
		}
	}
	return materialized;
}

bool materializePointerConstantExpressions(Module* module)
{
	struct UseToMaterialize
	{
		Instruction* instruction;
		unsigned operand;
		ConstantExpr* expression;
	};
	std::vector<UseToMaterialize> uses;
	for (Function& function : *module)
	for (Instruction& instruction : instructions(function))
	for (unsigned i = 0; i < instruction.getNumOperands(); ++i)
	{
		auto* expression = dyn_cast<ConstantExpr>(instruction.getOperand(i));
		if (expression != nullptr && constantNeedsPointerBridge(expression))
		{
			uses.push_back({&instruction, i, expression});
		}
	}

	for (const auto& use : uses)
	{
		Instruction* before = use.instruction;
		if (auto* phi = dyn_cast<PHINode>(use.instruction))
		{
			before = phi->getIncomingBlock(use.operand)->getTerminator();
		}
		auto* value = materializePointerConstantExpression(
				use.expression, before);
		use.instruction->setOperand(use.operand, value);
	}
	return !uses.empty();
}

struct PointerCellInitializer
{
	GlobalVariable* cell;
	Constant* value;
};

Value* materializeGuestConstant(
		IRBuilder<>& builder,
		Module* module,
		Constant* constant,
		Function* hostToGuest)
{
	auto* expression = dyn_cast<ConstantExpr>(constant);
	if (expression == nullptr)
	{
		return constant;
	}
	if (expression->getOpcode() == Instruction::PtrToInt
			&& expression->getType()->isIntegerTy(32)
			&& expression->getOperand(0)->getType()->getPointerAddressSpace() == 0)
	{
		Value* pointer = expression->getOperand(0);
		auto extent = getNativeObjectExtent(module, pointer);
		return builder.CreateCall(
				hostToGuest,
				{pointerAsBytePointer(builder, pointer),
				 pointerAsBytePointer(builder, extent.base),
				 ConstantInt::get(Type::getInt32Ty(module->getContext()), extent.size)});
	}

	auto* materialized = expression->getAsInstruction();
	for (unsigned i = 0; i < materialized->getNumOperands(); ++i)
	{
		auto* child = dyn_cast<Constant>(materialized->getOperand(i));
		if (child != nullptr && constantNeedsPointerBridge(child))
		{
			materialized->setOperand(
					i, materializeGuestConstant(builder, module, child, hostToGuest));
		}
	}
	return builder.Insert(materialized);
}

Value* pointerCellGuestValue(
		IRBuilder<>& builder,
		Module* module,
		const PointerCellInitializer& initializer,
		Function* hostToGuest)
{
	auto* int32 = Type::getInt32Ty(module->getContext());
	if (initializer.cell->getValueType()->isIntegerTy(32))
	{
		return materializeGuestConstant(
				builder, module, initializer.value, hostToGuest);
	}

	if (constantNeedsPointerBridge(initializer.value))
	{
		Value* pointer = materializeGuestConstant(
				builder, module, initializer.value, hostToGuest);
		return pointer->getType()->isPointerTy()
				? builder.Insert(new PtrToIntInst(pointer, int32))
				: pointer;
	}

	auto extent = getNativeObjectExtent(module, initializer.value);
	return builder.CreateCall(
			hostToGuest,
			{pointerAsBytePointer(builder, initializer.value),
			 pointerAsBytePointer(builder, extent.base),
			 ConstantInt::get(int32, extent.size)});
}

uint32_t configuredGuestAddress(Config* config, GlobalVariable* global)
{
	auto address = config->getGlobalAddress(global);
	return address.isDefined() && address.getValue() <= UINT32_MAX
			? static_cast<uint32_t>(address.getValue())
			: 0;
}

uint32_t configuredGuestAddress(Config* config, Function* function)
{
	auto address = config->getFunctionAddress(function);
	return address.isDefined() && address.getValue() <= UINT32_MAX
			? static_cast<uint32_t>(address.getValue())
			: 0;
}

uint32_t configuredFunctionExtent(Config* config, Function* function)
{
	auto* configured = config->getConfigFunction(function);
	if (configured == nullptr
			|| configured->getStart().isUndefined()
			|| configured->getEnd().isUndefined()
			|| configured->getEnd().getValue() <= configured->getStart().getValue())
	{
		return 1;
	}
	return static_cast<uint32_t>(std::min(
			configured->getEnd().getValue() - configured->getStart().getValue(),
			uint64_t{UINT32_MAX}));
}

bool constantIsGuestAddress(Constant* constant, uint32_t address)
{
	auto* expression = dyn_cast<ConstantExpr>(constant);
	if (expression == nullptr)
	{
		return false;
	}
	if (expression->getOpcode() == Instruction::IntToPtr)
	{
		if (auto* value = dyn_cast<ConstantInt>(expression->getOperand(0)))
		{
			return value->getZExtValue() == address;
		}
	}
	for (Value* operand : expression->operands())
	{
		if (auto* child = dyn_cast<Constant>(operand))
		{
			if (constantIsGuestAddress(child, address))
			{
				return true;
			}
		}
	}
	return false;
}

bool isNativeEntryPoint(const Config* config, const common::Function* function)
{
	if (config == nullptr || function == nullptr || function->getStart().isUndefined())
	{
		return false;
	}

	const auto& parameters = config->getConfig().parameters;
	const auto start = function->getStart();
	if (function->isExported()
			|| parameters.getEntryPoint() == start
			|| parameters.getMainAddress() == start
			|| parameters.selectedFunctions.count(function->getName()) != 0
			|| (!function->getRealName().empty()
					&& parameters.selectedFunctions.count(function->getRealName()) != 0))
	{
		return true;
	}

	for (const auto& range : parameters.selectedRanges)
	{
		if (range.getStart() == start)
		{
			return true;
		}
	}
	return false;
}

std::set<uint32_t> pe32BaseRelocationTargets(FileImage* fileImage)
{
	std::set<uint32_t> targets;
	if (fileImage == nullptr || fileImage->getImage() == nullptr)
	{
		return targets;
	}
	auto* pe = dynamic_cast<fileformat::PeFormat*>(
			fileImage->getFileFormat());
	if (pe == nullptr)
	{
		return targets;
	}

	// IMAGE_DIRECTORY_ENTRY_BASERELOC and IMAGE_REL_BASED_HIGHLOW are stable
	// PE constants.  A HIGHLOW cell contains the preferred 32-bit guest VA
	// which must be mapped to its native definition when retargeted to ELF64.
	static constexpr uint64_t baseRelocationDirectory = 5;
	static constexpr uint16_t highLowRelocation = 3;
	uint64_t directory = 0;
	uint64_t directorySize = 0;
	if (!pe->getDataDirectoryAbsolute(
			baseRelocationDirectory, directory, directorySize)
			|| directorySize < 8 || directory > UINT64_MAX - directorySize)
	{
		return targets;
	}

	auto* image = fileImage->getImage();
	const uint64_t imageBase = image->getBaseAddress();
	const uint64_t end = directory + directorySize;
	for (uint64_t cursor = directory; cursor <= end - 8; )
	{
		uint64_t pageRva = 0;
		uint64_t blockSize = 0;
		if (!image->getXByte(cursor, 4, pageRva)
				|| !image->getXByte(cursor + 4, 4, blockSize)
				|| blockSize < 8 || blockSize > end - cursor)
		{
			break;
		}
		for (uint64_t offset = 8; offset + 2 <= blockSize; offset += 2)
		{
			uint64_t raw = 0;
			if (!image->getXByte(cursor + offset, 2, raw))
			{
				continue;
			}
			if ((raw >> 12) != highLowRelocation)
			{
				continue;
			}
			const uint64_t cell = imageBase + pageRva + (raw & 0xfff);
			uint64_t target = 0;
			if (cell >= imageBase && image->getXByte(cell, 4, target)
					&& target <= UINT32_MAX)
			{
				targets.insert(static_cast<uint32_t>(target));
			}
		}
		cursor += blockSize;
	}
	return targets;
}

std::set<Function*> functionsReferencedByPeBaseRelocations(
		Module* module,
		Config* config)
{
	std::set<Function*> functions;
	auto targets = pe32BaseRelocationTargets(
			FileImageProvider::getFileImage(module));
	if (targets.empty())
	{
		return functions;
	}
	for (Function& function : *module)
	{
		if (!function.isDeclaration()
				&& targets.count(configuredGuestAddress(config, &function)) != 0)
		{
			functions.insert(&function);
		}
	}
	return functions;
}

bool localizePrivateFunctions(Module* module, Config* config)
{
	// A lifted image's private implementation functions are not process-wide
	// ELF symbols.  Keeping recovered CRT names externally visible makes two
	// independent PE images impossible to llvm-link.  Exact selection starts
	// are native adapter entry points, while functions merely contained in a
	// selected decode range remain image-local.
	const auto& parameters = config->getConfig().parameters;
	bool hasNativeEntry = parameters.getEntryPoint().isDefined()
			|| parameters.getMainAddress().isDefined()
			|| !parameters.selectedFunctions.empty()
			|| !parameters.selectedRanges.empty();
	for (Function& function : *module)
	{
		auto* configured = config->getConfigFunction(&function);
		hasNativeEntry = hasNativeEntry
				|| (configured != nullptr && configured->isExported());
	}
	if (!hasNativeEntry)
	{
		return false;
	}

	bool changed = false;
	for (Function& function : *module)
	{
		if (function.isDeclaration() || !function.hasExternalLinkage())
		{
			continue;
		}

		auto* configured = config->getConfigFunction(&function);
		if (configured == nullptr || isNativeEntryPoint(config, configured))
		{
			continue;
		}

		function.setLinkage(GlobalValue::InternalLinkage);
		changed = true;
	}
	return changed;
}

std::string encodePeFunctionNameForElf(StringRef name)
{
	if (!name.contains('@'))
	{
		return name.str();
	}

	static constexpr char hex[] = "0123456789abcdef";
	std::string encoded = "__retdec_pe32_elf_";
	encoded.reserve(encoded.size() + name.size() * 2);
	for (unsigned char byte : name.bytes())
	{
		encoded.push_back(hex[byte >> 4]);
		encoded.push_back(hex[byte & 0xf]);
	}
	return encoded;
}

bool legalizePeFunctionNamesForElf(Module* module, Config* config)
{
	bool changed = false;
	for (Function& function : *module)
	{
		const std::string original = function.getName().str();
		std::string encoded = encodePeFunctionNameForElf(original);
		if (encoded == original)
		{
			continue;
		}

		// The full-byte encoding is injective for decorated input names.  Avoid
		// colliding with an unrelated PE symbol that already uses the reserved
		// prefix, while keeping the result stable for a fixed module order.
		const std::string base = encoded;
		for (unsigned collision = 1;
				module->getNamedValue(encoded) != nullptr;
				++collision)
		{
			encoded = base + "." + std::to_string(collision);
		}

		if (auto* configured = config->getConfigFunction(&function))
		{
			if (configured->getRealName().empty())
			{
				configured->setRealName(original);
			}
			config->renameFunction(configured, encoded);
		}
		function.setName(encoded);
		changed = true;
	}
	return changed;
}

bool sameMemoryLocation(Value* first, Value* second)
{
	return first->stripPointerCasts() == second->stripPointerCasts();
}

bool valueDependsOnArgument(
		Value* value,
		Argument* argument,
		SmallPtrSetImpl<Value*>& visited)
{
	if (value == argument)
	{
		return true;
	}
	if (!visited.insert(value).second)
	{
		return false;
	}

	auto* instruction = dyn_cast<Instruction>(value);
	if (instruction == nullptr || instruction->getFunction() != argument->getParent())
	{
		return false;
	}
	if (auto* load = dyn_cast<LoadInst>(instruction))
	{
		for (Instruction& candidate : instructions(*instruction->getFunction()))
		{
			auto* store = dyn_cast<StoreInst>(&candidate);
			if (store != nullptr
					&& sameMemoryLocation(
							store->getPointerOperand(), load->getPointerOperand())
					&& valueDependsOnArgument(
							store->getValueOperand(), argument, visited))
			{
				return true;
			}
		}
		return false;
	}
	if (isa<CallBase>(instruction) || isa<StoreInst>(instruction))
	{
		return false;
	}
	for (Value* operand : instruction->operands())
	{
		if (valueDependsOnArgument(operand, argument, visited))
		{
			return true;
		}
	}
	return false;
}

struct NativeEntryWrapper
{
	Function* function;
	std::set<unsigned> pointerArguments;
};

std::vector<NativeEntryWrapper> findNativeEntryWrappers(
		Module* module,
		Config* config,
		const std::vector<IntToPtrInst*>& guestDereferences)
{
	std::map<Function*, std::set<unsigned>> pointerArguments;
	for (IntToPtrInst* dereference : guestDereferences)
	{
		Function* function = dereference->getFunction();
		for (Argument& argument : function->args())
		{
			if (!argument.getType()->isIntegerTy(32))
			{
				continue;
			}
			SmallPtrSet<Value*, 32> visited;
			if (valueDependsOnArgument(
						dereference->getOperand(0), &argument, visited))
			{
				pointerArguments[function].insert(argument.getArgNo());
			}
		}
	}

	// Propagate semantic guest-pointer parameters through direct lifted calls.
	// This catches selected native entries that forward an integer ABI slot to
	// a private helper which performs the eventual inttoptr/dereference.
	bool discovered = true;
	while (discovered)
	{
		discovered = false;
		for (Function& caller : *module)
		{
			if (caller.isDeclaration())
			{
				continue;
			}
			for (Instruction& instruction : instructions(caller))
			{
				auto* call = dyn_cast<CallBase>(&instruction);
				Function* callee = call != nullptr
						? call->getCalledFunction() : nullptr;
				auto calleeArguments = pointerArguments.find(callee);
				if (callee == nullptr
						|| calleeArguments == pointerArguments.end())
				{
					continue;
				}
				for (unsigned calleeArgument : calleeArguments->second)
				{
					if (calleeArgument >= call->arg_size())
					{
						continue;
					}
					for (Argument& callerArgument : caller.args())
					{
						if (!callerArgument.getType()->isIntegerTy(32))
						{
							continue;
						}
						SmallPtrSet<Value*, 32> visited;
						if (valueDependsOnArgument(
									call->getArgOperand(calleeArgument),
									&callerArgument,
									visited))
						{
							discovered |= pointerArguments[&caller].insert(
									callerArgument.getArgNo()).second;
						}
					}
				}
			}
		}
	}

	std::vector<NativeEntryWrapper> wrappers;
	for (Function& function : *module)
	{
		auto* configured = config->getConfigFunction(&function);
		if (function.isDeclaration()
				|| function.isVarArg()
				|| configured == nullptr
				|| !isNativeEntryPoint(config, configured))
		{
			continue;
		}

		auto arguments = pointerArguments.find(&function);
		NativeEntryWrapper wrapper{
				&function,
				arguments != pointerArguments.end()
						? arguments->second : std::set<unsigned>{}};
		wrappers.push_back(std::move(wrapper));
	}
	return wrappers;
}

bool createNativeEntryWrappers(
		Module* module,
		const std::vector<NativeEntryWrapper>& wrappers,
		Function* hostToGuest)
{
	bool changed = false;
	auto* int32 = Type::getInt32Ty(module->getContext());
	auto* bytePointer = Type::getInt8PtrTy(module->getContext());
	for (const auto& wrapperInfo : wrappers)
	{
		Function* guestFunction = wrapperInfo.function;
		const std::string wrapperName =
				(guestFunction->getName() + ".retdec_native").str();
		if (module->getFunction(wrapperName) != nullptr)
		{
			continue;
		}

		std::vector<Type*> argumentTypes;
		for (Argument& argument : guestFunction->args())
		{
			argumentTypes.push_back(wrapperInfo.pointerArguments.count(
						argument.getArgNo()) != 0
					? bytePointer
					: argument.getType());
		}
		auto* wrapperType = FunctionType::get(
				guestFunction->getReturnType(), argumentTypes, false);
		auto* wrapper = Function::Create(
				wrapperType,
				GlobalValue::ExternalLinkage,
				wrapperName,
				module);
		wrapper->setCallingConv(guestFunction->getCallingConv());
		auto* block = BasicBlock::Create(
				module->getContext(), "entry", wrapper);
		IRBuilder<> builder(block);
		std::vector<Value*> guestArguments;
		for (Argument& argument : wrapper->args())
		{
			if (wrapperInfo.pointerArguments.count(argument.getArgNo()) == 0)
			{
				guestArguments.push_back(&argument);
				continue;
			}
			argument.setName("host_pointer");
			guestArguments.push_back(builder.CreateCall(
					hostToGuest,
					{&argument, &argument, ConstantInt::get(int32, 0)},
					"guest_pointer"));
		}
		auto* call = builder.CreateCall(guestFunction, guestArguments);
		call->setCallingConv(guestFunction->getCallingConv());
		if (guestFunction->getReturnType()->isVoidTy())
		{
			builder.CreateRetVoid();
		}
		else
		{
			builder.CreateRet(call);
		}
		changed = true;
	}
	return changed;
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
	auto relocationTargetFunctions = functionsReferencedByPeBaseRelocations(
			_module, _config);
	bool changed = localizePrivateFunctions(_module, _config);
	// Record this before generating the constructor: passing a function to the
	// registration runtime is itself address-taking and must not make every
	// decoded direct-call-only function look like a required indirect target.
	std::set<Function*> functionsNeedingMappings = relocationTargetFunctions;
	for (Function& function : *_module)
	{
		if (!function.isDeclaration() && function.hasAddressTaken())
		{
			functionsNeedingMappings.insert(&function);
		}
	}
	changed |= materializePointerConstantExpressions(_module);
	std::vector<PointerCellInitializer> pointerInitializers;
	for (GlobalVariable& global : _module->globals())
	{
		if (!global.hasInitializer())
		{
			continue;
		}
		auto* initializer = global.getInitializer();
		const bool pointerCell = global.getValueType()->isPointerTy()
				&& global.getValueType()->getPointerAddressSpace() == 0;
		const bool integerRelocationCell = global.getValueType()->isIntegerTy(32)
				&& constantNeedsPointerBridge(initializer);
		if ((pointerCell || integerRelocationCell)
				&& !initializer->isNullValue()
				&& !isa<UndefValue>(initializer))
		{
			pointerInitializers.push_back({&global, initializer});
			global.setInitializer(Constant::getNullValue(global.getValueType()));
			global.setConstant(false);
			changed = true;
		}
	}
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
	auto nativeEntryWrappers = findNativeEntryWrappers(
			_module, _config, guestDereferences);
	// A decoded indirect target may be represented only by its original PE32
	// integer address.  Preserve exact configured targets used by inttoptr,
	// even when there is consequently no symbolic LLVM use of the function.
	for (Function& function : *_module)
	{
		if (function.isDeclaration())
		{
			continue;
		}
		const uint32_t address = configuredGuestAddress(_config, &function);
		if (address == 0)
		{
			continue;
		}
		for (IntToPtrInst* dereference : guestDereferences)
		{
			auto* value = dyn_cast<ConstantInt>(dereference->getOperand(0));
			if (value != nullptr && value->getZExtValue() == address)
			{
				functionsNeedingMappings.insert(&function);
				break;
			}
		}
		for (const auto& initializer : pointerInitializers)
		{
			if (constantIsGuestAddress(initializer.value, address))
			{
				functionsNeedingMappings.insert(&function);
				break;
			}
		}
	}
	bool hasConfiguredMappings = false;
	for (GlobalVariable& global : _module->globals())
	{
		hasConfiguredMappings = hasConfiguredMappings
				|| (!global.isDeclaration()
						&& configuredGuestAddress(_config, &global) != 0);
	}
	for (Function& function : *_module)
	{
		hasConfiguredMappings = hasConfiguredMappings
				|| (functionsNeedingMappings.count(&function) != 0
						&& configuredGuestAddress(_config, &function) != 0);
	}
	// PE stdcall decorations use '@' (for example, helper@4).  GNU ELF treats
	// that byte as symbol-version syntax, so even a correctly quoted LLVM name
	// cannot be linked natively.  Rename every decorated PE function only at
	// this final retargeting boundary; calls follow the LLVM value and config
	// retains the original spelling as the real name.  Do this before the
	// no-bridge-work return so name-only PE32 modules are legal ELF inputs too.
	changed |= legalizePeFunctionNamesForElf(_module, _config);
	if (hostEscapes.empty()
			&& guestDereferences.empty()
			&& addressSpaceCasts.empty()
			&& pointerInitializers.empty()
			&& nativeEntryWrappers.empty()
			&& !hasConfiguredMappings)
	{
		return changed;
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

	auto* registerObject = getOrCreateBridgeFunction(
			_module,
			"retdec_pe32_register_host_object",
			FunctionType::get(int32, {bytePointer, int32, int32}, false));
	auto* registerFunction = getOrCreateBridgeFunction(
			_module,
			"retdec_pe32_register_host_function",
			FunctionType::get(int32, {bytePointer, int32}, false));
	if (registerObject == nullptr || registerFunction == nullptr)
	{
		return false;
	}

	auto* initializerType = FunctionType::get(Type::getVoidTy(context), false);
	auto* initializer = Function::Create(
			initializerType,
			GlobalValue::InternalLinkage,
			"__retdec_pe32_initialize_mappings",
			_module);
	auto* entry = BasicBlock::Create(context, "entry", initializer);
	IRBuilder<> initializerBuilder(entry);
	bool initializerNeeded = false;
	for (GlobalVariable& global : _module->globals())
	{
		if (global.isDeclaration())
		{
			continue;
		}
		const uint32_t guestAddress = configuredGuestAddress(_config, &global);
		if (guestAddress == 0)
		{
			continue;
		}
		auto extent = getNativeObjectExtent(_module, &global);
		initializerBuilder.CreateCall(
				registerObject,
				{pointerAsBytePointer(initializerBuilder, &global),
				 ConstantInt::get(int32, extent.size),
				 ConstantInt::get(int32, guestAddress)});
		initializerNeeded = true;
	}
	for (Function& function : *_module)
	{
		if (function.isDeclaration()
				|| &function == initializer
				|| functionsNeedingMappings.count(&function) == 0)
		{
			continue;
		}
		const uint32_t guestAddress = configuredGuestAddress(_config, &function);
		if (guestAddress == 0)
		{
			continue;
		}
		initializerBuilder.CreateCall(
				registerFunction,
				{pointerAsBytePointer(initializerBuilder, &function),
				 ConstantInt::get(int32, guestAddress)});
		initializerNeeded = true;
	}
	for (const auto& pointerInitializer : pointerInitializers)
	{
		Value* guest = pointerCellGuestValue(
				initializerBuilder, _module, pointerInitializer, hostToGuest);
		if (guest->getType() != int32)
		{
			guest = initializerBuilder.CreateIntCast(guest, int32, false);
		}
		auto* guestCell = initializerBuilder.CreateBitCast(
				pointerInitializer.cell,
				PointerType::get(int32, 0));
		auto* store = initializerBuilder.CreateStore(guest, guestCell);
		store->setAlignment(4);
		initializerNeeded = true;
	}
	initializerBuilder.CreateRetVoid();
	if (initializerNeeded)
	{
		appendToGlobalCtors(*_module, initializer, 101);
		changed = true;
	}
	else
	{
		initializer->eraseFromParent();
	}

	for (PtrToIntInst* escape : hostEscapes)
	{
		IRBuilder<> builder(escape);
		Value* pointer = escape->getPointerOperand();
		if (auto* cast = dyn_cast<AddrSpaceCastInst>(pointer))
		{
			auto* sourceType = llvm::cast<PointerType>(cast->getSrcTy());
			if (sourceType->getAddressSpace()
					== Pe32PointerLegalization::GuestPointerAddressSpace)
			{
				auto* guest = builder.CreatePtrToInt(
						cast->getOperand(0), int32, escape->getName() + ".guest");
				escape->replaceAllUsesWith(guest);
				escape->eraseFromParent();
				continue;
			}
		}
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
		if (cast->use_empty())
		{
			cast->eraseFromParent();
			continue;
		}
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
	changed |= createNativeEntryWrappers(
			_module, nativeEntryWrappers, hostToGuest);

	return changed
			|| !hostEscapes.empty()
			|| !guestDereferences.empty()
			|| !addressSpaceCasts.empty();
}

} // namespace bin2llvmir
} // namespace retdec
