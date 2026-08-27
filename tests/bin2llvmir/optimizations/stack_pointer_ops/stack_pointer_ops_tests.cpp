/**
* @file tests/bin2llvmir/optimizations/stack_pointer_ops/tests/stack_pointer_ops_tests.cpp
* @brief Tests for the @c StackPointerOpsRemove pass.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
*/

#include "retdec/bin2llvmir/optimizations/stack_pointer_ops/stack_pointer_ops.h"
#include "retdec/bin2llvmir/providers/config.h"
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
	auto s = retdec::config::Storage::inRegister("esp");
	auto r = retdec::config::Object("esp", s);
	auto c = Config::empty(module.get());
	c.getConfig().registers.insert(r);

	bool b = pass.runOnModuleCustom(*module, &c);

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
	auto s = retdec::config::Storage::inRegister("eax");
	auto r = retdec::config::Object("eax", s);
	auto c = Config::empty(module.get());
	c.getConfig().registers.insert(r);

	bool b = pass.runOnModuleCustom(*module, &c);

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
	c.getConfig().registers.insert(retdec::config::Object(
			"esp", retdec::config::Storage::inRegister("esp")));
	c.getConfig().registers.insert(retdec::config::Object(
			"esi", retdec::config::Storage::inRegister("esi")));

	EXPECT_TRUE(pass.runOnModuleCustom(*module, &c));
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

TEST_F(StackPointerOpsRemoveTests, removesDeadPopChainIntoRegistersAtReturn)
{
	parseInput(R"(
		@esp = global i32 0
		@edi = global i32 0
		@esi = global i32 0
		@ebx = global i32 0
		define i32 @func() {
			%stack_pointer = load i32, i32* @esp
			%address0 = inttoptr i32 %stack_pointer to i32*
			%popped0 = load i32, i32* %address0
			store i32 %popped0, i32* @edi
			%offset4 = add i32 %stack_pointer, 4
			%address4 = inttoptr i32 %offset4 to i32*
			%popped4 = load i32, i32* %address4
			store i32 %popped4, i32* @esi
			%offset8 = add i32 %stack_pointer, 8
			%address8 = inttoptr i32 %offset8 to i32*
			%popped8 = load i32, i32* %address8
			store i32 %popped8, i32* @ebx
			ret i32 7
		}
	)");
	auto c = Config::empty(module.get());
	for (auto name : {"esp", "edi", "esi", "ebx"})
	{
		c.getConfig().registers.insert(retdec::config::Object(
				name, retdec::config::Storage::inRegister(name)));
	}

	EXPECT_TRUE(pass.runOnModuleCustom(*module, &c));
	for (auto& block : *module->getFunction("func"))
	for (auto& instruction : block)
	{
		if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
		{
			EXPECT_NE(load->getPointerOperand(),
					module->getGlobalVariable("esp"));
		}
	}
	for (auto name : {"edi", "esi", "ebx"})
	{
		bool restoredFromEntry = false;
		for (auto& block : *module->getFunction("func"))
		for (auto& instruction : block)
		{
			auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
			auto* load = store == nullptr ? nullptr
					: llvm::dyn_cast<llvm::LoadInst>(store->getValueOperand());
			restoredFromEntry |= store != nullptr && load != nullptr
					&& store->getPointerOperand()
							== module->getGlobalVariable(name)
					&& load->getPointerOperand()
							== module->getGlobalVariable(name);
		}
		EXPECT_TRUE(restoredFromEntry);
	}
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
	c.getConfig().registers.insert(retdec::config::Object(
			"esp", retdec::config::Storage::inRegister("esp")));

	EXPECT_FALSE(pass.runOnModuleCustom(*module, &c));
	EXPECT_NE(nullptr, getValueByName("popped"));
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
