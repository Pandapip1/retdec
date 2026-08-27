/**
 * @file tests/bin2llvmir/optimizations/register_localization/register_localization_tests.cpp
 * @brief Tests for the @c RegisterLocalization pass.
 * @copyright (c) 2026 Avast Software, licensed under the MIT license
 */

#include "retdec/bin2llvmir/optimizations/register_localization/register_localization.h"
#include "retdec/bin2llvmir/providers/abi/x86.h"
#include "retdec/capstone2llvmir/x86/x86_defs.h"
#include "bin2llvmir/utils/llvmir_tests.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

class RegisterLocalizationTests : public LlvmIrTests
{
protected:
	RegisterLocalization pass;
};

TEST_F(RegisterLocalizationTests, preservesIncomingArchitecturalRegisterValue)
{
	parseInput(R"(
		@fpu_top = internal global i3 0
		define i3 @readThenUpdate() {
		entry:
			%incoming = load i3, i3* @fpu_top
			%updated = add i3 %incoming, 1
			store i3 %updated, i3* @fpu_top
			ret i3 %incoming
		}
	)");
	auto config = Config::empty(module.get());
	AbiX86 abi(module.get(), &config);
	auto* architectural = getGlobalByName("fpu_top");
	abi.addRegister(X87_REG_TOP, architectural);

	ASSERT_TRUE(pass.runOnModuleCustom(*module, &abi, &config));

	auto* localized = getNthInstruction<AllocaInst>();
	ASSERT_NE(nullptr, localized);
	auto* entryLoad = dyn_cast<LoadInst>(localized->getNextNode());
	ASSERT_NE(nullptr, entryLoad);
	EXPECT_EQ(architectural, entryLoad->getPointerOperand());
	auto* entryStore = dyn_cast<StoreInst>(entryLoad->getNextNode());
	ASSERT_NE(nullptr, entryStore);
	EXPECT_EQ(entryLoad, entryStore->getValueOperand());
	EXPECT_EQ(localized, entryStore->getPointerOperand());
	auto* originalLoad = cast<LoadInst>(getValueByName("incoming"));
	EXPECT_EQ(localized, originalLoad->getPointerOperand());
}

TEST_F(RegisterLocalizationTests, preservesArchitecturalRegisterSharedAcrossCall)
{
	parseInput(R"(
		@st7 = internal global x86_fp80 0xK00000000000000000000
		define i32 @caller() {
		entry:
			store x86_fp80 0xK4002B2D20000000000000, x86_fp80* @st7
			%result = call i32 @callee()
			ret i32 %result
		}
		define i32 @callee() {
		entry:
			%value = load x86_fp80, x86_fp80* @st7
			%result = fptosi x86_fp80 %value to i32
			ret i32 %result
		}
	)");
	auto config = Config::empty(module.get());
	AbiX86 abi(module.get(), &config);
	auto* architectural = getGlobalByName("st7");
	abi.addRegister(X86_REG_ST7, architectural);

	EXPECT_FALSE(pass.runOnModuleCustom(*module, &abi, &config));

	auto* sharedStore = getNthInstruction<StoreInst>();
	ASSERT_NE(nullptr, sharedStore);
	EXPECT_EQ(architectural, sharedStore->getPointerOperand());
	auto* sharedLoad = getNthInstruction<LoadInst>();
	ASSERT_NE(nullptr, sharedLoad);
	EXPECT_EQ(architectural, sharedLoad->getPointerOperand());
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
