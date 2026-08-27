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
			auto json = std::string(R"({
				"architecture" : {
					"bitSize" : )") + std::to_string(bitSize) + R"(,
					"endian" : "little",
					"name" : "x86"
				},
				"fileFormat" : ")" + fileFormat + R"("
			})";
			auto commonConfig = config::Config::fromJsonString(json);
			return Config::fromConfig(module.get(), commonConfig);
		}

		Pe32PointerLegalization pass;
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

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
