/**
* @file tests/bin2llvmir/optimizations/stack_pointer_ops/tests/stack_pointer_ops_tests.cpp
* @brief Tests for the @c StackPointerOpsRemove pass.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
*/

#include "retdec/bin2llvmir/optimizations/stack_pointer_ops/stack_pointer_ops.h"
#include "retdec/bin2llvmir/providers/abi/abi.h"
#include "retdec/bin2llvmir/providers/abi/x86.h"
#include "bin2llvmir/utils/llvmir_tests.h"

using namespace ::testing;

namespace retdec {
namespace bin2llvmir {
namespace tests {

/**
 * @brief Tests for the @c InstOpt pass.
 */
class StackPointerOpsRemoveTests: public LlvmIrTests
{
	protected:
		StackPointerOpsRemove pass;
};

//
// runOnModule()
//

TEST_F(StackPointerOpsRemoveTests, passDoesNotSegfaultAndReturnsFalseIfConfigForModuleDoesNotExists)
{
	bool b = pass.runOnModule(*module);

	EXPECT_FALSE(b);
}

TEST_F(StackPointerOpsRemoveTests, passDoesNotSegfaultAndReturnsFalseIfNullptrConfigPassed)
{
	bool b = pass.runOnModuleCustom(*module, nullptr);

	EXPECT_FALSE(b);
}

TEST_F(StackPointerOpsRemoveTests, passRemovesAllStoresToStackRegistersEvenIfTheyHaveUses)
{
	parseInput(R"(
		@esp = global i32 0
		define void @func() {
			%a = load i32, i32* @esp
			%b = add i32 %a, 1234
			store i32 %b, i32* @esp
			%c = load i32, i32* @esp
			ret void
		}
	)");
	auto* esp = getGlobalByName("esp");
	auto c = Config::empty(module.get());
	AbiX86 abi(module.get(), &c);
	abi.addRegister(X86_REG_ESP, esp);

	bool b = pass.runOnModuleCustom(*module, &abi);

	std::string exp = R"(
		@esp = global i32 0
		define void @func() {
			%a = load i32, i32* @esp
			%b = add i32 %a, 1234
			%c = load i32, i32* @esp
			ret void
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(b);
}

TEST_F(StackPointerOpsRemoveTests, passKeepsAllStoresToNonStackPointerRegisters)
{
	parseInput(R"(
		@eax = global i32 0
		define void @func() {
			%a = load i32, i32* @eax
			%b = add i32 %a, 1234
			store i32 %b, i32* @eax
			ret void
		}
	)");
	auto* eax = getGlobalByName("eax");
	auto c = Config::empty(module.get());
	AbiX86 abi(module.get(), &c);
	abi.addRegister(X86_REG_EAX, eax);

	bool b = pass.runOnModuleCustom(*module, &abi);

	std::string exp = R"(
		@eax = global i32 0
		define void @func() {
			%a = load i32, i32* @eax
			%b = add i32 %a, 1234
			store i32 %b, i32* @eax
			ret void
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_FALSE(b);
}

TEST_F(StackPointerOpsRemoveTests, removesDeadPopIntoLocalizedRegisterAtReturn)
{
	parseInput(R"(
		@esp = global i32 0
		@esi = global i32 0
		define i32 @func() {
			%stack_pointer = load i32, i32* @esp
			%stack_address = inttoptr i32 %stack_pointer to i32*
			%popped = load i32, i32* %stack_address
			store i32 %popped, i32* @esi
			ret i32 7
		}
	)");
	auto c = Config::empty(module.get());
	AbiX86 abi(module.get(), &c);
	abi.addRegister(X86_REG_ESP, getGlobalByName("esp"));
	abi.addRegister(X86_REG_ESI, getGlobalByName("esi"));

	EXPECT_TRUE(pass.runOnModuleCustom(*module, &abi));
	bool loadsSyntheticStackPointer = false;
	for (auto& block : *module->getFunction("func"))
	for (auto& instruction : block)
	{
		if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
		{
			loadsSyntheticStackPointer |=
					load->getPointerOperand() == module->getGlobalVariable("esp");
		}
	}
	EXPECT_FALSE(loadsSyntheticStackPointer);
	auto* restored = llvm::dyn_cast<llvm::LoadInst>(
			getNthInstruction<llvm::StoreInst>()->getValueOperand());
	ASSERT_NE(nullptr, restored);
	EXPECT_EQ(module->getGlobalVariable("esi"), restored->getPointerOperand());
}

TEST_F(StackPointerOpsRemoveTests, keepsPopValueThatRemainsSemanticallyUsed)
{
	parseInput(R"(
		@esp = global i32 0
		define i32 @func() {
			%local = alloca i32
			%stack_pointer = load i32, i32* @esp
			%stack_address = inttoptr i32 %stack_pointer to i32*
			%popped = load i32, i32* %stack_address
			store i32 %popped, i32* %local
			ret i32 %popped
		}
	)");
	auto c = Config::empty(module.get());
	AbiX86 abi(module.get(), &c);
	abi.addRegister(X86_REG_ESP, getGlobalByName("esp"));

	EXPECT_FALSE(pass.runOnModuleCustom(*module, &abi));
	EXPECT_NE(nullptr, getValueByName("popped"));
}

TEST_F(StackPointerOpsRemoveTests, removesDeadPopIntoRegisterLocalizationAlloca)
{
	parseInput(R"(
		@esp = global i32 0
		@esi = global i32 0
		define void @func() {
			%esi.local = alloca i32
			%stack_pointer = load i32, i32* @esp
			%stack_address = inttoptr i32 %stack_pointer to i32*
			%popped = load i32, i32* %stack_address
			store i32 %popped, i32* %esi.local
			ret void
		}
	)");
	auto* localized = cast<AllocaInst>(getValueByName("esi.local"));
	localized->setName("esi");
	auto c = Config::empty(module.get());
	AbiX86 abi(module.get(), &c);
	abi.addRegister(X86_REG_ESP, getGlobalByName("esp"));
	abi.addRegister(X86_REG_ESI, getGlobalByName("esi"));

	EXPECT_TRUE(pass.runOnModuleCustom(*module, &abi));
	EXPECT_EQ(nullptr, getValueByName("popped"));
	EXPECT_TRUE(localized->use_empty());
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
