/**
* @file tests/bin2llvmir/optimizations/stack/stack_tests.cpp
* @brief Tests for the @c StackAnalysis pass.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
*/

#include "retdec/bin2llvmir/optimizations/stack/stack.h"
#include "retdec/bin2llvmir/providers/abi/abi.h"
#include "retdec/bin2llvmir/providers/config.h"
#include "bin2llvmir/utils/llvmir_tests.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

class StackAnalysisTests: public LlvmIrTests
{
	protected:
		StackAnalysis pass;
};

TEST_F(StackAnalysisTests, reconstructsIndexedFrameRelativeStackObject)
{
	parseInput(R"(
		@ebp = global i32 0
		define void @func(i32 %index, float %value) {
			%stack_var_-192 = alloca i32
			%stack_var_-156 = alloca i32
			%stack_var_-4 = alloca i32
			%anchor = ptrtoint i32* %stack_var_-4 to i32
			store i32 %anchor, i32* @ebp
			%base = load i32, i32* @ebp
			%scaled = mul i32 %index, 4
			%displaced = add i32 %scaled, -188
			%address = add i32 %base, %displaced
			%pointer = inttoptr i32 %address to float*
			store float %value, float* %pointer
			ret void
		}
	)");

	auto config = Config::empty(module.get());
	config.getConfig().registers.insert(retdec::config::Object(
			"ebp", retdec::config::Storage::inRegister("ebp")));
	auto function = retdec::config::Function("func");
	function.locals.insert(retdec::config::Object(
			"stack_var_-192", retdec::config::Storage::onStack(-192)));
	function.locals.insert(retdec::config::Object(
			"stack_var_-156", retdec::config::Storage::onStack(-156)));
	function.locals.insert(retdec::config::Object(
			"stack_var_-4", retdec::config::Storage::onStack(-4)));
	config.getConfig().functions.insert(function);
	auto* abi = AbiProvider::addAbi(module.get(), &config);

	EXPECT_TRUE(pass.runOnModuleCustom(*module, &config, abi));
	auto* stackObject = cast<AllocaInst>(getValueByName("stack_var_-192"));
	EXPECT_EQ(9u, cast<ConstantInt>(
			stackObject->getArraySize())->getZExtValue());
	EXPECT_NE(nullptr, getValueByName("base"));
	EXPECT_NE(nullptr, getValueByName("pointer.stack"));
}

TEST_F(StackAnalysisTests, reconstructsTwoObjectsWithoutReplacingSharedFrameBase)
{
	parseInput(R"(
		@ebp = global i32 0
		define void @func(i32 %index, float %first, float %second) {
			%stack_var_-400 = alloca i32
			%stack_var_-380 = alloca i32
			%stack_var_-192 = alloca i32
			%stack_var_-156 = alloca i32
			%stack_var_-4 = alloca i32
			%anchor = ptrtoint i32* %stack_var_-4 to i32
			store i32 %anchor, i32* @ebp
			%base = load i32, i32* @ebp
			%scaled = mul i32 %index, 4
			%first.displaced = add i32 %scaled, -188
			%first.address = add i32 %base, %first.displaced
			%first.pointer = inttoptr i32 %first.address to float*
			store float %first, float* %first.pointer
			%second.displaced = add i32 %scaled, -396
			%second.address = add i32 %base, %second.displaced
			%second.pointer = inttoptr i32 %second.address to float*
			store float %second, float* %second.pointer
			ret void
		}
	)");

	auto config = Config::empty(module.get());
	config.getConfig().registers.insert(retdec::config::Object(
			"ebp", retdec::config::Storage::inRegister("ebp")));
	auto function = retdec::config::Function("func");
	for (int offset : {-400, -380, -192, -156, -4})
	{
		std::string name = "stack_var_" + std::to_string(offset);
		function.locals.insert(retdec::config::Object(
				name, retdec::config::Storage::onStack(offset)));
	}
	config.getConfig().functions.insert(function);
	auto* abi = AbiProvider::addAbi(module.get(), &config);

	EXPECT_TRUE(pass.runOnModuleCustom(*module, &config, abi));
	auto* firstObject = cast<AllocaInst>(getValueByName("stack_var_-192"));
	auto* secondObject = cast<AllocaInst>(getValueByName("stack_var_-400"));
	EXPECT_EQ(9u, cast<ConstantInt>(
			firstObject->getArraySize())->getZExtValue());
	EXPECT_EQ(5u, cast<ConstantInt>(
			secondObject->getArraySize())->getZExtValue());
	EXPECT_NE(nullptr, getValueByName("base"));
	EXPECT_NE(nullptr, getValueByName("first.pointer.stack"));
	EXPECT_NE(nullptr, getValueByName("second.pointer.stack"));
}

TEST_F(StackAnalysisTests, keepsIndexedAddressWhenFrameBaseIsAmbiguous)
{
	parseInput(R"(
		@ebp = global i32 0
		define void @func(i1 %condition, i32 %index, float %value) {
		entry:
			%stack_var_-192 = alloca i32
			%stack_var_-156 = alloca i32
			%stack_var_-4 = alloca i32
			br i1 %condition, label %left, label %right
		left:
			store i32 0, i32* @ebp
			br label %merge
		right:
			%anchor = ptrtoint i32* %stack_var_-4 to i32
			store i32 %anchor, i32* @ebp
			br label %merge
		merge:
			%base = load i32, i32* @ebp
			%scaled = mul i32 %index, 4
			%displaced = add i32 %scaled, -188
			%address = add i32 %base, %displaced
			%pointer = inttoptr i32 %address to float*
			store float %value, float* %pointer
			ret void
		}
	)");

	auto config = Config::empty(module.get());
	config.getConfig().registers.insert(retdec::config::Object(
			"ebp", retdec::config::Storage::inRegister("ebp")));
	auto function = retdec::config::Function("func");
	function.locals.insert(retdec::config::Object(
			"stack_var_-192", retdec::config::Storage::onStack(-192)));
	function.locals.insert(retdec::config::Object(
			"stack_var_-156", retdec::config::Storage::onStack(-156)));
	function.locals.insert(retdec::config::Object(
			"stack_var_-4", retdec::config::Storage::onStack(-4)));
	config.getConfig().functions.insert(function);
	auto* abi = AbiProvider::addAbi(module.get(), &config);

	EXPECT_FALSE(pass.runOnModuleCustom(*module, &config, abi));
	auto* stackObject = llvm::cast<llvm::AllocaInst>(
			getValueByName("stack_var_-192"));
	EXPECT_EQ(1u, llvm::cast<llvm::ConstantInt>(
			stackObject->getArraySize())->getZExtValue());
	EXPECT_NE(nullptr, getValueByName("base"));
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
