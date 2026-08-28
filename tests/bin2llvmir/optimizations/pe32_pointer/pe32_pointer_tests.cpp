/**
 * @file tests/bin2llvmir/optimizations/pe32_pointer/pe32_pointer_tests.cpp
 * @brief Tests for PE32 pointer-cell legalization.
 * @copyright (c) 2026 Avast Software, licensed under the MIT license
 */

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/IPO.h>

#include <algorithm>
#include <iterator>

#include "bin2llvmir/utils/llvmir_tests.h"
#include "retdec/bin2llvmir/optimizations/pe32_pointer/pe32_pointer.h"
#include "retdec/bin2llvmir/providers/abi/x86.h"
#include "retdec/bin2llvmir/providers/fileimage.h"
#include "retdec/capstone2llvmir/x86/x86_defs.h"
#include "retdec/fileformat/file_format/pe/pe_format.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

class Pe32PointerLegalizationTests: public LlvmIrTests
{
	protected:
		void addPe32HighLowRelocation(
				Config& config,
				uint32_t cellAddress,
				uint32_t targetAddress)
		{
			// Minimal PE32 image with one .reloc section.  Its relocation block
			// describes a HIGHLOW cell whose original contents are a preferred
			// guest function VA.
			std::vector<uint8_t> bytes(0x400);
			auto put16 = [&bytes](size_t offset, uint16_t value)
			{
				bytes[offset] = value;
				bytes[offset + 1] = value >> 8;
			};
			auto put32 = [&bytes](size_t offset, uint32_t value)
			{
				for (unsigned i = 0; i < 4; ++i)
				{
					bytes[offset + i] = value >> (8 * i);
				}
			};

			bytes[0] = 'M';
			bytes[1] = 'Z';
			put32(0x3c, 0x40);
			bytes[0x40] = 'P';
			bytes[0x41] = 'E';
			put16(0x44, 0x14c);
			put16(0x46, 1);
			put16(0x54, 0xe0);
			put16(0x56, 0x102);
			put16(0x58, 0x10b);
			put32(0x68, 0x1000);
			put32(0x74, 0x400000);
			put32(0x78, 0x1000);
			put32(0x7c, 0x200);
			put32(0x90, 0x3000);
			put32(0x94, 0x200);
			put16(0xa0, 3);
			put32(0xb4, 16);
			put32(0xe0, 0x2000);
			put32(0xe4, 12);
			bytes[0x138] = '.';
			bytes[0x139] = 'r';
			bytes[0x13a] = 'e';
			bytes[0x13b] = 'l';
			bytes[0x13c] = 'o';
			bytes[0x13d] = 'c';
			put32(0x140, 0x200);
			put32(0x144, 0x2000);
			put32(0x148, 0x200);
			put32(0x14c, 0x200);
			put32(0x15c, 0x42000040);
			put32(0x200, 0x2000);
			put32(0x204, 12);
			put16(0x208, 0x3000 | (cellAddress & 0xfff));
			put16(0x20a, 0);
			put32(0x200 + (cellAddress & 0xfff), targetAddress);

			auto pe = std::make_shared<fileformat::PeFormat>(
					bytes.data(), bytes.size());
			ASSERT_TRUE(pe->isInValidState());
			ASSERT_NE(nullptr, FileImageProvider::addFileImage(
					module.get(), pe, &config));
		}

		Config createConfig(const char* fileFormat, unsigned bitSize)
		{
			auto config = Config::empty(module.get());
			config.getConfig().architecture.setIsX86();
			config.getConfig().architecture.setBitSize(bitSize);
			config.getConfig().architecture.setIsEndianLittle();
			if (std::string(fileFormat) == "pe32")
			{
				config.getConfig().fileFormat.setIsPe32();
			}
			else
			{
				config.getConfig().fileFormat.setIsPe64();
			}
			return config;
		}

		void addGlobalAddress(Config& config, StringRef name, uint32_t address)
		{
			config.getConfig().globals.insert(retdec::common::Object(
					name.str(), retdec::common::Storage::inMemory(address)));
		}

		void addFunctionRange(
				Config& config,
				StringRef name,
				uint32_t start,
				uint32_t end)
		{
			config.getConfig().functions.insert(retdec::common::Function(
					start, end, name.str()));
		}

		Pe32PointerLegalization pass;
		Pe32PointerBridge bridge;
};

TEST_F(Pe32PointerLegalizationTests,
		bridgeMakesArchitecturalCpuStateThreadLocalForNativeConcurrency)
{
	parseInput(R"(
		@eax = internal global i32 0
		@cf = internal global i1 false
		@fpu_stat_TOP = internal global i3 0
		@st0 = internal global x86_fp80 0xK00000000000000000000
		@ordinary_pe_global = global i32 0
	)");
	auto config = createConfig("pe32", 32);
	auto* abi = AbiProvider::addAbi(module.get(), &config);
	ASSERT_NE(nullptr, abi);
	abi->addRegister(X86_REG_EAX, getGlobalByName("eax"));
	abi->addRegister(X86_REG_EFLAGS, getGlobalByName("cf"));
	abi->addRegister(X87_REG_TOP, getGlobalByName("fpu_stat_TOP"));
	abi->addRegister(X86_REG_ST0, getGlobalByName("st0"));

	ASSERT_TRUE(bridge.runOnModuleCustom(*module, &config));
	for (StringRef name : {"eax", "cf", "fpu_stat_TOP", "st0"})
	{
		auto* reg = module->getGlobalVariable(name, true);
		ASSERT_NE(nullptr, reg);
		EXPECT_TRUE(reg->isThreadLocal()) << name.str();
		EXPECT_EQ(GlobalVariable::GeneralDynamicTLSModel,
				reg->getThreadLocalMode()) << name.str();
	}
	EXPECT_FALSE(module->getGlobalVariable("ordinary_pe_global")->isThreadLocal());
}

TEST_F(Pe32PointerLegalizationTests,
		legalizesAdjacentGlobalAndStackPointerCells)
{
	parseInput(R"(
		@pe_ptr0 = external global i8*
		@pe_ptr1 = external global i8*

		define i8* @func(i8* %value) {
			%frame = alloca [8 x i8], align 4
			%slot0.bytes = getelementptr [8 x i8], [8 x i8]* %frame, i32 0, i32 0
			%slot0 = bitcast i8* %slot0.bytes to i8**
			%slot1.bytes = getelementptr [8 x i8], [8 x i8]* %frame, i32 0, i32 4
			%slot1 = bitcast i8* %slot1.bytes to i8**
			%global.value = load i8*, i8** @pe_ptr0, align 4
			store i8* %global.value, i8** @pe_ptr1, align 4
			store i8* %value, i8** %slot0, align 4
			%stack.value = load i8*, i8** %slot0, align 4
			store i8* %stack.value, i8** %slot1, align 4
			ret i8* %stack.value
		}
	)");
	auto config = createConfig("pe32", 32);

	EXPECT_TRUE(pass.runOnModuleCustom(*module, &config));

	unsigned guestLoads = 0;
	unsigned guestStores = 0;
	unsigned addressSpaceCasts = 0;
	for (Instruction& instruction : instructions(*module->getFunction("func")))
	{
		if (auto* load = dyn_cast<LoadInst>(&instruction))
		{
			if (load->getType()->isPointerTy()
					&& load->getType()->getPointerAddressSpace()
							== Pe32PointerLegalization::GuestPointerAddressSpace)
			{
				++guestLoads;
				EXPECT_EQ(4u, load->getAlignment());
			}
		}
		else if (auto* store = dyn_cast<StoreInst>(&instruction))
		{
			auto* type = store->getValueOperand()->getType();
			if (type->isPointerTy()
					&& type->getPointerAddressSpace()
							== Pe32PointerLegalization::GuestPointerAddressSpace)
			{
				++guestStores;
				EXPECT_EQ(4u, store->getAlignment());
			}
		}
		else if (isa<AddrSpaceCastInst>(&instruction))
		{
			++addressSpaceCasts;
		}
	}

	EXPECT_EQ(2u, guestLoads);
	EXPECT_EQ(3u, guestStores);
	EXPECT_EQ(5u, addressSpaceCasts);
	EXPECT_FALSE(verifyModule(*module, &errs()));
}

TEST_F(Pe32PointerLegalizationTests, ignoresNonPe32Modules)
{
	parseInput(R"(
		@pointer = external global i8*
		define i8* @func() {
			%value = load i8*, i8** @pointer
			ret i8* %value
		}
	)");

	auto pe64 = createConfig("pe64", 64);
	EXPECT_FALSE(pass.runOnModuleCustom(*module, &pe64));
	auto* value = cast<LoadInst>(getValueByName("value"));
	EXPECT_EQ(0u, value->getType()->getPointerAddressSpace());
	for (Instruction& instruction : instructions(*module->getFunction("func")))
	{
		EXPECT_FALSE(isa<AddrSpaceCastInst>(&instruction));
	}
}

TEST_F(Pe32PointerLegalizationTests,
		bridgeRegistersEscapingAllocaExtentAndRemovesLossyCasts)
{
	parseInput(R"(
		define void @func() {
			%frame = alloca [16 x i8], align 4
			%cell = alloca i32, align 4
			%guest = ptrtoint [16 x i8]* %frame to i32
			store i32 %guest, i32* %cell, align 4
			%reloaded = load i32, i32* %cell, align 4
			%host = inttoptr i32 %reloaded to i32*
			store i32 7, i32* %host, align 4
			ret void
		}
	)");
	auto config = createConfig("pe32", 32);

	EXPECT_TRUE(bridge.runOnModuleCustom(*module, &config));

	auto* function = module->getFunction("func");
	CallInst* encode = nullptr;
	CallInst* decode = nullptr;
	for (Instruction& instruction : instructions(*function))
	{
		if (auto* call = dyn_cast<CallInst>(&instruction))
		{
			if (call->getCalledFunction() != nullptr
					&& call->getCalledFunction()->getName()
							== "__retdec_pe32_host_to_guest")
			{
				encode = call;
			}
			else if (call->getCalledFunction() != nullptr
					&& call->getCalledFunction()->getName()
							== "__retdec_pe32_guest_to_host")
			{
				decode = call;
			}
		}
		if (auto* cast = dyn_cast<PtrToIntInst>(&instruction))
		{
			EXPECT_FALSE(cast->getPointerOperand()->getType()
					->getPointerAddressSpace() == 0
					&& cast->getType()->isIntegerTy(32));
		}
		if (auto* cast = dyn_cast<IntToPtrInst>(&instruction))
		{
			EXPECT_FALSE(cast->getOperand(0)->getType()->isIntegerTy(32)
					&& cast->getType()->getPointerAddressSpace() == 0);
		}
	}
	ASSERT_NE(nullptr, encode);
	ASSERT_NE(nullptr, decode);
	EXPECT_EQ(getValueByName("frame"),
			encode->getArgOperand(1)->stripPointerCasts());
	EXPECT_EQ(nullptr,
			module->getFunction("retdec_pe32_unregister_host_object"));
	auto* extent = cast<ConstantInt>(encode->getArgOperand(2));
	EXPECT_EQ(16u, extent->getZExtValue());
	EXPECT_FALSE(verifyModule(*module, &errs()));
}

TEST_F(Pe32PointerLegalizationTests,
		bridgeLeavesModulesWithoutLossyPointerCastsUnmodified)
{
	parseInput(R"(
		define i32 @func() {
			ret i32 0
		}
	)");
	auto config = createConfig("pe32", 32);

	EXPECT_FALSE(bridge.runOnModuleCustom(*module, &config));
	EXPECT_EQ(nullptr, module->getFunction("__retdec_pe32_host_to_guest"));
	EXPECT_EQ(nullptr, module->getFunction("__retdec_pe32_guest_to_host"));
}

TEST_F(Pe32PointerLegalizationTests,
		bridgeKeepsEscapingStackBaseMappedAfterEveryReturn)
{
	parseInput(R"(
		define i32 @func(i1 %choose) {
		entry:
			%frame = alloca [32 x i8], align 4
			%guest = ptrtoint [32 x i8]* %frame to i32
			br i1 %choose, label %first, label %second
		first:
			ret i32 %guest
		second:
			ret i32 %guest
		}
	)");
	auto config = createConfig("pe32", 32);

	ASSERT_TRUE(bridge.runOnModuleCustom(*module, &config));
	auto* function = module->getFunction("func");
	unsigned encodes = 0;
	for (Instruction& instruction : instructions(*function))
	{
		auto* call = dyn_cast<CallInst>(&instruction);
		if (call != nullptr && call->getCalledFunction() != nullptr
				&& call->getCalledFunction()->getName()
						== "__retdec_pe32_host_to_guest")
		{
			++encodes;
		}
	}
	EXPECT_EQ(1u, encodes);
	EXPECT_EQ(nullptr,
			module->getFunction("retdec_pe32_unregister_host_object"));
	EXPECT_FALSE(verifyModule(*module, &errs()));
}

TEST_F(Pe32PointerLegalizationTests,
		bridgeUsesConfiguredGlobalSpanForIndexedRecoveredScalar)
{
	parseInput(R"(
		@recovered.array = global i32 0
		@next.object = global i32 0
		@plain.scalar = global i32 0
		@plain.next = global i32 0
		define void @func(i32 %index) {
			%base = ptrtoint i32* @recovered.array to i32
			%offset = mul i32 %index, 4
			%address = add i32 %base, %offset
			%pointer = inttoptr i32 %address to i32*
			store i32 7, i32* %pointer, align 4
			ret void
		}
		define i32* @plain() {
			%guest = ptrtoint i32* @plain.scalar to i32
			%pointer = inttoptr i32 %guest to i32*
			ret i32* %pointer
		}
	)");
	auto config = createConfig("pe32", 32);
	addGlobalAddress(config, "recovered.array", 0x5137cec0);
	addGlobalAddress(config, "next.object", 0x5137d010);
	addGlobalAddress(config, "plain.scalar", 0x5137d020);
	addGlobalAddress(config, "plain.next", 0x5137d120);

	ASSERT_TRUE(bridge.runOnModuleCustom(*module, &config));
	CallInst* encode = nullptr;
	for (Instruction& instruction : instructions(*module->getFunction("func")))
	{
		auto* call = dyn_cast<CallInst>(&instruction);
		if (call != nullptr && call->getCalledFunction() != nullptr
				&& call->getCalledFunction()->getName()
						== "__retdec_pe32_host_to_guest")
		{
			encode = call;
		}
	}
	ASSERT_NE(nullptr, encode);
	EXPECT_EQ(module->getGlobalVariable("recovered.array"),
			encode->getArgOperand(1)->stripPointerCasts());
	auto* extent = cast<ConstantInt>(encode->getArgOperand(2));
	EXPECT_EQ(0x150u, extent->getZExtValue());
	CallInst* recoveredRegistration = nullptr;
	CallInst* plainRegistration = nullptr;
	for (Instruction& instruction : instructions(
			*module->getFunction("__retdec_pe32_initialize_mappings")))
	{
		auto* call = dyn_cast<CallInst>(&instruction);
		if (call == nullptr || call->getCalledFunction() == nullptr
				|| call->getCalledFunction()->getName()
						!= "retdec_pe32_register_host_object")
		{
			continue;
		}
		auto* object = call->getArgOperand(0)->stripPointerCasts();
		if (object == module->getGlobalVariable("recovered.array"))
		{
			recoveredRegistration = call;
		}
		else if (object == module->getGlobalVariable("plain.scalar"))
		{
			plainRegistration = call;
		}
	}
	ASSERT_NE(nullptr, recoveredRegistration);
	auto* recoveredRegisteredExtent = cast<ConstantInt>(
			recoveredRegistration->getArgOperand(1));
	EXPECT_EQ(0x150u, recoveredRegisteredExtent->getZExtValue());
	ASSERT_NE(nullptr, plainRegistration);
	auto* plainRegisteredExtent = cast<ConstantInt>(
			plainRegistration->getArgOperand(1));
	EXPECT_EQ(4u, plainRegisteredExtent->getZExtValue());
	CallInst* plainEncode = nullptr;
	for (Instruction& instruction : instructions(*module->getFunction("plain")))
	{
		auto* call = dyn_cast<CallInst>(&instruction);
		if (call != nullptr && call->getCalledFunction() != nullptr
				&& call->getCalledFunction()->getName()
						== "__retdec_pe32_host_to_guest")
		{
			plainEncode = call;
		}
	}
	ASSERT_NE(nullptr, plainEncode);
	auto* plainExtent = cast<ConstantInt>(plainEncode->getArgOperand(2));
	EXPECT_EQ(4u, plainExtent->getZExtValue());
	EXPECT_FALSE(verifyModule(*module, &errs()));
}

TEST_F(Pe32PointerLegalizationTests,
		bridgeBoundsIndexedExternalConstantAtFollowingFunction)
{
	parseInput(R"(
		@lookup = external constant i8*
		@distant.object = global i8 0
		define i8 @use.lookup(i32 %index) {
			%base = ptrtoint i8** @lookup to i32
			%address = add i32 %base, %index
			%pointer = inttoptr i32 %address to i8*
			%value = load i8, i8* %pointer, align 1
			ret i8 %value
		}
		define void @following.function() {
			ret void
		}
	)");
	auto config = createConfig("pe32", 32);
	addGlobalAddress(config, "lookup", 0x5133a8f3);
	addGlobalAddress(config, "distant.object", 0x51361ab0);
	addFunctionRange(config, "following.function", 0x5133a905, 0x5133a920);

	ASSERT_TRUE(bridge.runOnModuleCustom(*module, &config));
	CallInst* encode = nullptr;
	for (Instruction& instruction : instructions(*module->getFunction("use.lookup")))
	{
		auto* call = dyn_cast<CallInst>(&instruction);
		if (call != nullptr && call->getCalledFunction() != nullptr
				&& call->getCalledFunction()->getName()
						== "__retdec_pe32_host_to_guest")
		{
			encode = call;
		}
	}
	ASSERT_NE(nullptr, encode);
	EXPECT_EQ(module->getGlobalVariable("lookup"),
			encode->getArgOperand(1)->stripPointerCasts());
	auto* extent = cast<ConstantInt>(encode->getArgOperand(2));
	EXPECT_EQ(18u, extent->getZExtValue());
	EXPECT_FALSE(verifyModule(*module, &errs()));
}

TEST_F(Pe32PointerLegalizationTests,
		bridgePipelineIsOptInAndNormalizedAfterPointerCellLegalization)
{
	std::vector<std::string> passes{
			"retdec-pe32-pointer-bridge",
			"retdec-provider-init",
			"retdec-pe32-pointer-cells",
			"retdec-write-bc"};

	ASSERT_TRUE(Pe32PointerBridge::enableInPipeline(passes));
	std::vector<std::string> expected{
			"retdec-provider-init",
			"retdec-pe32-pointer-cells",
			"retdec-pe32-pointer-bridge",
			"retdec-write-bc"};
	EXPECT_EQ(expected, passes);

	std::vector<std::string> missingLegalization{"retdec-write-bc"};
	EXPECT_FALSE(Pe32PointerBridge::enableInPipeline(missingLegalization));
	EXPECT_EQ(1u, missingLegalization.size());
}

TEST_F(Pe32PointerLegalizationTests,
		bridgeTranslatesNativeValuesAtFourByteGuestPointerCells)
{
	parseInput(R"(
		@cell = external global i8*
		define i8* @func(i8* %native) {
			store i8* %native, i8** @cell, align 4
			%loaded = load i8*, i8** @cell, align 4
			ret i8* %loaded
		}
	)");
	auto config = createConfig("pe32", 32);
	ASSERT_TRUE(pass.runOnModuleCustom(*module, &config));
	unsigned addressSpaceCastsBefore = 0;
	for (Instruction& instruction : instructions(*module->getFunction("func")))
	{
		if (auto* addressSpaceCast = dyn_cast<AddrSpaceCastInst>(&instruction))
		{
			++addressSpaceCastsBefore;
			EXPECT_TRUE(addressSpaceCast->getSrcTy()->getPointerAddressSpace() == 0
					|| addressSpaceCast->getDestTy()->getPointerAddressSpace() == 0);
		}
	}
	ASSERT_EQ(2u, addressSpaceCastsBefore);

	EXPECT_TRUE(bridge.runOnModuleCustom(*module, &config));

	unsigned encodes = 0;
	unsigned decodes = 0;
	unsigned guestLoads = 0;
	unsigned guestStores = 0;
	for (Instruction& instruction : instructions(*module->getFunction("func")))
	{
		if (auto* call = dyn_cast<CallInst>(&instruction))
		{
			auto* called = call->getCalledFunction();
			encodes += called != nullptr
					&& called->getName() == "__retdec_pe32_host_to_guest";
			decodes += called != nullptr
					&& called->getName() == "__retdec_pe32_guest_to_host";
		}
		else if (auto* load = dyn_cast<LoadInst>(&instruction))
		{
			guestLoads += load->getType()->isPointerTy()
					&& load->getType()->getPointerAddressSpace()
							== Pe32PointerLegalization::GuestPointerAddressSpace;
		}
		else if (auto* store = dyn_cast<StoreInst>(&instruction))
		{
			auto* type = store->getValueOperand()->getType();
			guestStores += type->isPointerTy()
					&& type->getPointerAddressSpace()
							== Pe32PointerLegalization::GuestPointerAddressSpace;
		}
	}
	EXPECT_EQ(1u, encodes);
	EXPECT_EQ(1u, decodes);
	EXPECT_EQ(1u, guestLoads);
	EXPECT_EQ(1u, guestStores);
	EXPECT_FALSE(verifyModule(*module, &errs()));
}

TEST_F(Pe32PointerLegalizationTests,
		bridgeInitializesPointerGlobalsThroughPicSafeRuntimeConstructor)
{
	parseInput(R"(
		@target = global [8 x i8] zeroinitializer
		@pointer = global i8* getelementptr (
			[8 x i8], [8 x i8]* @target, i32 0, i32 3)
		define i8* @func() {
			%value = load i8*, i8** @pointer, align 4
			ret i8* %value
		}
	)");
	auto config = createConfig("pe32", 32);
	addGlobalAddress(config, "target", 0x401000);
	addGlobalAddress(config, "pointer", 0x402000);
	addFunctionRange(config, "func", 0x403000, 0x403020);
	ASSERT_TRUE(pass.runOnModuleCustom(*module, &config));

	ASSERT_TRUE(bridge.runOnModuleCustom(*module, &config));
	auto* pointer = module->getGlobalVariable("pointer");
	ASSERT_NE(nullptr, pointer);
	ASSERT_TRUE(pointer->hasInitializer());
	EXPECT_TRUE(pointer->getInitializer()->isNullValue());
	auto* initializer = module->getFunction(
			"__retdec_pe32_initialize_mappings");
	ASSERT_NE(nullptr, initializer);

	unsigned objectRegistrations = 0;
	unsigned functionRegistrations = 0;
	unsigned encodedInitializers = 0;
	unsigned fourBytePointerStores = 0;
	for (Instruction& instruction : instructions(initializer))
	{
		if (auto* call = dyn_cast<CallInst>(&instruction))
		{
			auto* called = call->getCalledFunction();
			if (called != nullptr
					&& called->getName() == "retdec_pe32_register_host_object")
			{
				++objectRegistrations;
			}
			else if (called != nullptr
					&& called->getName()
							== "retdec_pe32_register_host_function")
			{
				++functionRegistrations;
			}
			else if (called != nullptr
					&& called->getName()
							== "__retdec_pe32_host_to_guest")
			{
				++encodedInitializers;
			}
		}
		else if (auto* store = dyn_cast<StoreInst>(&instruction))
		{
			fourBytePointerStores += store->getValueOperand()->getType()
					->isIntegerTy(32) && store->getAlignment() == 4;
		}
	}
	EXPECT_EQ(2u, objectRegistrations);
	EXPECT_EQ(0u, functionRegistrations);
	EXPECT_EQ(1u, encodedInitializers);
	EXPECT_EQ(1u, fourBytePointerStores);
	EXPECT_NE(nullptr, module->getGlobalVariable("llvm.global_ctors"));
	EXPECT_FALSE(verifyModule(*module, &errs()));
}

TEST_F(Pe32PointerLegalizationTests,
		bridgeRegistersOnlyAddressTakenFunctionsAndAllowsGlobalDce)
{
	parseInput(R"(
		@address.taken = global void ()* @kept
		define internal void @kept() {
			ret void
		}
		define internal void @unused() {
			ret void
		}
		define internal void @direct() {
			ret void
		}
		define internal void @raw.target() {
			ret void
		}
		define void @caller() {
			call void @direct()
			call void inttoptr (i32 4214784 to void ()*)()
			ret void
		}
	)");
	auto config = createConfig("pe32", 32);
	addFunctionRange(config, "kept", 0x401000, 0x401010);
	addFunctionRange(config, "unused", 0x402000, 0x402010);
	addFunctionRange(config, "direct", 0x403000, 0x403010);
	addFunctionRange(config, "caller", 0x404000, 0x404010);
	addFunctionRange(config, "raw.target", 0x405000, 0x405010);

	ASSERT_TRUE(bridge.runOnModuleCustom(*module, &config));
	auto* initializer = module->getFunction(
			"__retdec_pe32_initialize_mappings");
	ASSERT_NE(nullptr, initializer);
	std::set<std::string> registeredFunctions;
	for (Instruction& instruction : instructions(initializer))
	{
		auto* call = dyn_cast<CallInst>(&instruction);
		if (call == nullptr
				|| call->getCalledFunction() == nullptr
				|| call->getCalledFunction()->getName()
						!= "retdec_pe32_register_host_function")
		{
			continue;
		}
		Value* target = call->getArgOperand(0)->stripPointerCasts();
		if (auto* function = dyn_cast<Function>(target))
		{
			registeredFunctions.insert(function->getName().str());
		}
	}
	EXPECT_EQ(std::set<std::string>({"kept", "raw.target"}),
			registeredFunctions);

	legacy::PassManager dce;
	dce.add(createGlobalDCEPass());
	dce.run(*module);
	EXPECT_EQ(nullptr, module->getFunction("unused"));
	EXPECT_NE(nullptr, module->getFunction("direct"));
	EXPECT_NE(nullptr, module->getFunction("kept"));
	EXPECT_NE(nullptr, module->getFunction("raw.target"));
	EXPECT_FALSE(verifyModule(*module, &errs()));
}

TEST_F(Pe32PointerLegalizationTests,
		bridgeKeepsRequestedEntriesPublicAndIsolatesRecoveredHelpersPerImage)
{
	auto createImage = [this](
			StringRef moduleName,
			StringRef rootName,
			StringRef objectName,
			StringRef exportedName,
			uint32_t addressBase)
	{
		auto image = std::make_unique<Module>(moduleName, context);
		image->setDataLayout("e-p:32:32:32-f80:32:32");
		auto* functionType = FunctionType::get(
				Type::getVoidTy(context), false);
		auto defineFunction = [&](StringRef name)
		{
			auto* function = Function::Create(
					functionType, GlobalValue::ExternalLinkage, name, image.get());
			auto* block = BasicBlock::Create(context, "entry", function);
			ReturnInst::Create(context, block);
			return function;
		};

		auto* root = defineFunction(rootName);
		auto* exported = defineFunction(exportedName);
		std::vector<Function*> helpers;
		for (StringRef name : {"__NMSG_WRITE", "__fload_withFB",
				"__heap_alloc", "__nh_malloc", "_malloc"})
		{
			helpers.push_back(defineFunction(name));
		}
		auto* object = new GlobalVariable(
				*image,
				Type::getInt32Ty(context),
				false,
				GlobalValue::InternalLinkage,
				ConstantInt::get(Type::getInt32Ty(context), 0),
				objectName);

		auto config = Config::empty(image.get());
		config.getConfig().architecture.setIsX86();
		config.getConfig().architecture.setBitSize(32);
		config.getConfig().architecture.setIsEndianLittle();
		config.getConfig().fileFormat.setIsPe32();
		config.insertFunction(root, addressBase, addressBase + 0x10);
		config.getConfig().parameters.selectedRanges.insert(
				retdec::common::AddressRange(addressBase, addressBase + 0x100));
		auto* exportedConfig = const_cast<retdec::common::Function*>(
				config.insertFunction(
					exported, addressBase + 0x100, addressBase + 0x110));
		exportedConfig->setIsExported(true);
		for (std::size_t i = 0; i < helpers.size(); ++i)
		{
			config.insertFunction(
					helpers[i], addressBase + 0x20 + i * 0x10,
					addressBase + 0x30 + i * 0x10);
		}
		config.getConfig().globals.insert(retdec::common::Object(
				objectName.str(),
				retdec::common::Storage::inMemory(addressBase + 0x200)));

		Pe32PointerBridge imageBridge;
		EXPECT_TRUE(imageBridge.runOnModuleCustom(*image, &config));
		EXPECT_TRUE(root->hasExternalLinkage());
		EXPECT_TRUE(exported->hasExternalLinkage());
		for (Function* helper : helpers)
		{
			EXPECT_TRUE(helper->hasInternalLinkage());
		}
		auto* initializer = image->getFunction(
				"__retdec_pe32_initialize_mappings");
		EXPECT_NE(nullptr, initializer);
		EXPECT_TRUE(initializer->hasInternalLinkage());
		EXPECT_FALSE(verifyModule(*image, &errs()));
		(void)object;
		return image;
	};

	auto first = createImage("first", "root.first", "object.first",
			"export.first", 0x401000);
	auto second = createImage("second", "root.second", "object.second",
			"export.second", 0x501000);
	std::set<std::string> firstExternalDefinitions;
	std::set<std::string> secondExternalDefinitions;
	auto auditImage = [](
			Module& image,
			std::set<std::string>& externalDefinitions)
	{
		unsigned recoveredHelpers = 0;
		unsigned mappingInitializers = 0;
		for (Function& function : image)
		{
			if (!function.isDeclaration() && function.hasExternalLinkage())
			{
				externalDefinitions.insert(function.getName().str());
			}
			recoveredHelpers += function.getName().startswith("__NMSG_WRITE")
					|| function.getName().startswith("__fload_withFB")
					|| function.getName().startswith("__heap_alloc")
					|| function.getName().startswith("__nh_malloc")
					|| function.getName().startswith("_malloc");
			mappingInitializers += function.getName().startswith(
					"__retdec_pe32_initialize_mappings");
		}
		EXPECT_EQ(5u, recoveredHelpers);
		EXPECT_EQ(1u, mappingInitializers);
		auto* constructors = image.getGlobalVariable("llvm.global_ctors");
		ASSERT_NE(nullptr, constructors);
		auto* constructorArray = dyn_cast<ConstantArray>(
				constructors->getInitializer());
		ASSERT_NE(nullptr, constructorArray);
		EXPECT_EQ(1u, constructorArray->getNumOperands());
	};
	auditImage(*first, firstExternalDefinitions);
	auditImage(*second, secondExternalDefinitions);
	std::vector<std::string> collisions;
	std::set_intersection(
			firstExternalDefinitions.begin(), firstExternalDefinitions.end(),
			secondExternalDefinitions.begin(), secondExternalDefinitions.end(),
			std::back_inserter(collisions));
	EXPECT_TRUE(collisions.empty());
	EXPECT_FALSE(verifyModule(*first, &errs()));
	EXPECT_FALSE(verifyModule(*second, &errs()));
}

TEST_F(Pe32PointerLegalizationTests,
		bridgeMapsDataRelocatedFunctionWhileKeepingItImageLocal)
{
	parseInput(R"(
		define void @root() {
			ret void
		}
		define void @relocated() {
			ret void
		}
		define void @private() {
			ret void
		}
	)");
	auto config = createConfig("pe32", 32);
	addFunctionRange(config, "root", 0x401000, 0x401010);
	addFunctionRange(config, "relocated", 0x402000, 0x402010);
	addFunctionRange(config, "private", 0x403000, 0x403010);
	addPe32HighLowRelocation(config, 0x402020, 0x402000);
	config.getConfig().parameters.selectedRanges.insert(
			retdec::common::AddressRange(0x401000, 0x401010));

	ASSERT_TRUE(bridge.runOnModuleCustom(*module, &config));
	auto* relocated = module->getFunction("relocated");
	auto* privateFunction = module->getFunction("private");
	ASSERT_NE(nullptr, relocated);
	ASSERT_NE(nullptr, privateFunction);
	EXPECT_TRUE(relocated->hasInternalLinkage());
	EXPECT_TRUE(privateFunction->hasInternalLinkage());

	auto* initializer = module->getFunction(
			"__retdec_pe32_initialize_mappings");
	ASSERT_NE(nullptr, initializer);
	bool registered = false;
	for (Instruction& instruction : instructions(initializer))
	{
		auto* call = dyn_cast<CallInst>(&instruction);
		if (call == nullptr || call->getCalledFunction() == nullptr
				|| call->getCalledFunction()->getName()
						!= "retdec_pe32_register_host_function")
		{
			continue;
		}
		registered |= call->getArgOperand(0)->stripPointerCasts() == relocated;
	}
	EXPECT_TRUE(registered);
	EXPECT_FALSE(verifyModule(*module, &errs()));
}

TEST_F(Pe32PointerLegalizationTests,
		bridgeEncodesPeDecoratedFunctionNamesForElfNativeOutput)
{
	parseInput(R"(
		declare i32 @"_RtlUnwind@16"(i32)
		define i32 @"__seh_longjmp_unwind@4"(i32 %arg) {
			%result = call i32 @"_RtlUnwind@16"(i32 %arg)
			ret i32 %result
		}
	)");
	auto config = createConfig("pe32", 32);
	auto* root = module->getFunction("__seh_longjmp_unwind@4");
	addFunctionRange(config, root->getName(), 0x401000, 0x401020);
	config.getConfig().parameters.selectedRanges.insert(
			retdec::common::AddressRange(0x401000, 0x401020));

	EXPECT_TRUE(bridge.runOnModuleCustom(*module, &config));
	EXPECT_EQ(nullptr, module->getFunction("__seh_longjmp_unwind@4"));
	EXPECT_EQ(nullptr, module->getFunction("_RtlUnwind@16"));

	auto* encodedRoot = module->getFunction(
			"__retdec_pe32_elf_5f5f7365685f6c6f6e676a6d705f756e77696e644034");
	auto* encodedImport = module->getFunction(
			"__retdec_pe32_elf_5f52746c556e77696e64403136");
	ASSERT_NE(nullptr, encodedRoot);
	ASSERT_NE(nullptr, encodedImport);
	EXPECT_NE(nullptr, module->getFunction(
			(encodedRoot->getName() + ".retdec_native").str()));
	for (Function& function : *module)
	{
		EXPECT_EQ(StringRef::npos, function.getName().find('@'));
	}
	auto* configured = config.getConfigFunction(encodedRoot);
	ASSERT_NE(nullptr, configured);
	EXPECT_EQ("__seh_longjmp_unwind@4", configured->getRealName());
	EXPECT_FALSE(verifyModule(*module, &errs()));
}

TEST_F(Pe32PointerLegalizationTests,
		bridgeEncodesDecoratedDeclarationWithoutOtherBridgeWork)
{
	parseInput(R"(
		declare i32 @"_RtlUnwind@16"(i32)
	)");
	auto config = createConfig("pe32", 32);

	EXPECT_TRUE(bridge.runOnModuleCustom(*module, &config));
	EXPECT_EQ(nullptr, module->getFunction("_RtlUnwind@16"));
	EXPECT_NE(nullptr, module->getFunction(
			"__retdec_pe32_elf_5f52746c556e77696e64403136"));
	EXPECT_EQ(nullptr, module->getFunction("__retdec_pe32_host_to_guest"));
	EXPECT_FALSE(verifyModule(*module, &errs()));
}

TEST_F(Pe32PointerLegalizationTests,
		bridgeInitializesIntegerAndPointerRelocationCellsAsFourByteGuests)
{
	parseInput(R"(
		@target = global [8 x i8] zeroinitializer
		@integer.host = global i32 ptrtoint ([8 x i8]* @target to i32)
		@pointer.guest = global i8* inttoptr (i32 4198400 to i8*)
		@pointer.nested = global i8* inttoptr (i32 add (
			i32 ptrtoint ([8 x i8]* @target to i32), i32 5) to i8*)
	)");
	auto config = createConfig("pe32", 32);
	addGlobalAddress(config, "target", 0x401000);

	ASSERT_TRUE(bridge.runOnModuleCustom(*module, &config));
	for (StringRef name : {"integer.host", "pointer.guest", "pointer.nested"})
	{
		auto* global = module->getGlobalVariable(name);
		ASSERT_NE(nullptr, global);
		EXPECT_TRUE(global->getInitializer()->isNullValue());
	}

	auto* initializer = module->getFunction(
			"__retdec_pe32_initialize_mappings");
	ASSERT_NE(nullptr, initializer);
	unsigned encodes = 0;
	unsigned stores = 0;
	bool preservedGuestAddress = false;
	for (Instruction& instruction : instructions(initializer))
	{
		for (Value* operand : instruction.operands())
		{
			if (auto* value = dyn_cast<ConstantInt>(operand))
			{
				preservedGuestAddress |= value->getZExtValue() == 0x401000;
			}
		}
		if (auto* call = dyn_cast<CallInst>(&instruction))
		{
			auto* called = call->getCalledFunction();
			encodes += called != nullptr
					&& called->getName() == "__retdec_pe32_host_to_guest";
		}
		else if (auto* store = dyn_cast<StoreInst>(&instruction))
		{
			if (store->getValueOperand()->getType()->isIntegerTy(32))
			{
				++stores;
				EXPECT_EQ(4u, store->getAlignment());
			}
		}
	}
	EXPECT_EQ(2u, encodes);
	EXPECT_EQ(3u, stores);
	EXPECT_TRUE(preservedGuestAddress);
	EXPECT_FALSE(verifyModule(*module, &errs()));
}

TEST_F(Pe32PointerLegalizationTests,
		bridgePreservesGuestToNativeToIntegerRoundTripWithoutHostEncoding)
{
	parseInput(R"(
		define i32 @func(i32 addrspace(271)* %guest) {
			%native = addrspacecast i32 addrspace(271)* %guest to i32*
			%address = ptrtoint i32* %native to i32
			ret i32 %address
		}
	)");
	auto config = createConfig("pe32", 32);

	ASSERT_TRUE(bridge.runOnModuleCustom(*module, &config));
	unsigned encodes = 0;
	unsigned rawGuestCasts = 0;
	for (Instruction& instruction : instructions(*module->getFunction("func")))
	{
		if (auto* call = dyn_cast<CallInst>(&instruction))
		{
			auto* called = call->getCalledFunction();
			encodes += called != nullptr
					&& called->getName() == "__retdec_pe32_host_to_guest";
		}
		else if (auto* cast = dyn_cast<PtrToIntInst>(&instruction))
		{
			rawGuestCasts += cast->getPointerOperand()->getType()
					->getPointerAddressSpace()
				== Pe32PointerLegalization::GuestPointerAddressSpace;
		}
	}
	EXPECT_EQ(0u, encodes);
	EXPECT_EQ(1u, rawGuestCasts);
	EXPECT_FALSE(verifyModule(*module, &errs()));
}

TEST_F(Pe32PointerLegalizationTests,
		bridgeMaterializesNestedConstantPointerExpressions)
{
	parseInput(R"(
		@target = global [8 x i8] zeroinitializer
		define i8* @func() {
			ret i8* inttoptr (i32 add (
				i32 ptrtoint ([8 x i8]* @target to i32), i32 5) to i8*)
		}
	)");
	auto config = createConfig("pe32", 32);

	ASSERT_TRUE(bridge.runOnModuleCustom(*module, &config));
	unsigned encodes = 0;
	unsigned decodes = 0;
	for (Instruction& instruction : instructions(*module->getFunction("func")))
	{
		for (Value* operand : instruction.operands())
		{
			EXPECT_EQ(nullptr, dyn_cast<ConstantExpr>(operand));
		}
		if (auto* call = dyn_cast<CallInst>(&instruction))
		{
			auto* called = call->getCalledFunction();
			encodes += called != nullptr
					&& called->getName() == "__retdec_pe32_host_to_guest";
			decodes += called != nullptr
					&& called->getName() == "__retdec_pe32_guest_to_host";
		}
	}
	EXPECT_EQ(1u, encodes);
	EXPECT_EQ(1u, decodes);
	EXPECT_FALSE(verifyModule(*module, &errs()));
}

TEST_F(Pe32PointerLegalizationTests,
		bridgeCreatesAllNativeEntryWrappersAndPromotesOnlyProvenPointerArguments)
{
	parseInput(R"(
		define internal i32 @helper(i32 %guest) {
			%pointer = inttoptr i32 %guest to i32*
			%value = load i32, i32* %pointer, align 4
			ret i32 %value
		}
		define i32 @entry(i32 %scalar, i32 %direct, i32 %forwarded, i32 %control) {
			%scaled.index = mul i32 %scalar, 32
			%direct.address = add i32 %direct, %scaled.index
			%direct.pointer = inttoptr i32 %direct.address to i32*
			%direct.value = load i32, i32* %direct.pointer, align 4
			%derived = add i32 %forwarded, 4
			%enabled = icmp ne i32 %control, 0
			%selected = select i1 %enabled, i32 %derived, i32 0
			%forwarded.value = call i32 @helper(i32 %selected)
			%sum = add i32 %direct.value, %forwarded.value
			%result = add i32 %sum, %scalar
			ret i32 %result
		}
		define i32 @private(i32 %guest) {
			%pointer = inttoptr i32 %guest to i32*
			%value = load i32, i32* %pointer, align 4
			ret i32 %value
		}
		define i32 @scalar.entry(i32 %value) {
			%result = add i32 %value, 1
			ret i32 %result
		}
	)");
	auto config = createConfig("pe32", 32);
	addFunctionRange(config, "entry", 0x401000, 0x401020);
	addFunctionRange(config, "helper", 0x402000, 0x402020);
	addFunctionRange(config, "private", 0x403000, 0x403020);
	addFunctionRange(config, "scalar.entry", 0x404000, 0x404020);
	config.getConfig().parameters.selectedRanges.insert(
			retdec::common::AddressRange(0x401000, 0x401020));
	config.getConfig().parameters.selectedRanges.insert(
			retdec::common::AddressRange(0x404000, 0x404020));

	ASSERT_TRUE(bridge.runOnModuleCustom(*module, &config));
	auto* guestEntry = module->getFunction("entry");
	auto* nativeEntry = module->getFunction("entry.retdec_native");
	ASSERT_NE(nullptr, guestEntry);
	ASSERT_NE(nullptr, nativeEntry);
	EXPECT_TRUE(guestEntry->getFunctionType()->getParamType(0)->isIntegerTy(32));
	EXPECT_TRUE(guestEntry->getFunctionType()->getParamType(1)->isIntegerTy(32));
	EXPECT_TRUE(guestEntry->getFunctionType()->getParamType(2)->isIntegerTy(32));
	EXPECT_TRUE(guestEntry->getFunctionType()->getParamType(3)->isIntegerTy(32));
	EXPECT_TRUE(nativeEntry->getFunctionType()->getParamType(0)->isIntegerTy(32));
	EXPECT_TRUE(nativeEntry->getFunctionType()->getParamType(1)->isPointerTy());
	EXPECT_TRUE(nativeEntry->getFunctionType()->getParamType(2)->isPointerTy());
	EXPECT_TRUE(nativeEntry->getFunctionType()->getParamType(3)->isIntegerTy(32));
	EXPECT_EQ(nullptr, module->getFunction("helper.retdec_native"));
	EXPECT_EQ(nullptr, module->getFunction("private.retdec_native"));
	auto* scalarEntry = module->getFunction("scalar.entry.retdec_native");
	ASSERT_NE(nullptr, scalarEntry);
	EXPECT_TRUE(scalarEntry->getFunctionType()->getParamType(0)->isIntegerTy(32));

	unsigned encodes = 0;
	CallInst* guestCall = nullptr;
	for (Instruction& instruction : instructions(nativeEntry))
	{
		auto* call = dyn_cast<CallInst>(&instruction);
		if (call == nullptr || call->getCalledFunction() == nullptr)
		{
			continue;
		}
		if (call->getCalledFunction()->getName()
				== "__retdec_pe32_host_to_guest")
		{
			++encodes;
			EXPECT_EQ(call->getArgOperand(0), call->getArgOperand(1));
			auto* extent = dyn_cast<ConstantInt>(call->getArgOperand(2));
			ASSERT_NE(nullptr, extent);
			EXPECT_TRUE(extent->isZero());
		}
		else if (call->getCalledFunction() == guestEntry)
		{
			guestCall = call;
		}
	}
	EXPECT_EQ(2u, encodes);
	ASSERT_NE(nullptr, guestCall);
	EXPECT_TRUE(guestCall->getArgOperand(0)->getType()->isIntegerTy(32));
	EXPECT_TRUE(guestCall->getArgOperand(1)->getType()->isIntegerTy(32));
	EXPECT_TRUE(guestCall->getArgOperand(2)->getType()->isIntegerTy(32));
	EXPECT_TRUE(guestCall->getArgOperand(3)->getType()->isIntegerTy(32));
	unsigned scalarEncodes = 0;
	for (Instruction& instruction : instructions(scalarEntry))
	{
		auto* call = dyn_cast<CallInst>(&instruction);
		scalarEncodes += call != nullptr
				&& call->getCalledFunction() != nullptr
				&& call->getCalledFunction()->getName()
						== "__retdec_pe32_host_to_guest";
	}
	EXPECT_EQ(0u, scalarEncodes);
	EXPECT_FALSE(verifyModule(*module, &errs()));
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
