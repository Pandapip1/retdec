/**
 * @file tests/bin2llvmir/optimizations/pe32_pointer/pe32_pointer_tests.cpp
 * @brief Tests for PE32 pointer-cell legalization.
 * @copyright (c) 2026 Avast Software, licensed under the MIT license
 */

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Verifier.h>

#include "bin2llvmir/utils/llvmir_tests.h"
#include "retdec/bin2llvmir/optimizations/pe32_pointer/pe32_pointer.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

class Pe32PointerLegalizationTests: public LlvmIrTests
{
	protected:
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

		Pe32PointerLegalization pass;
		Pe32PointerBridge bridge;
};

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

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
