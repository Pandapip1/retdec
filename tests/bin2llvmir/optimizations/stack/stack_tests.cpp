/**
* @file tests/bin2llvmir/optimizations/stack/stack_tests.cpp
* @brief Tests for the @c StackAnalysis pass.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
*/

#include "retdec/bin2llvmir/optimizations/stack/stack.h"
#include "retdec/bin2llvmir/optimizations/stack_pointer_ops/stack_pointer_ops.h"
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

TEST_F(StackAnalysisTests, coalescesIndexedAndFixedStackObjectViews)
{
	parseInput(R"(
		define void @func(i32 %index, i32 %value) {
			%stack_var_-776 = alloca i32
			%stack_var_-772 = alloca i32
			%stack_var_-768 = alloca i32
			%stack_var_-548 = alloca i32
			%base = ptrtoint i32* %stack_var_-776 to i32
			%scaled = mul i32 %index, 4
			%address = add i32 %scaled, %base
			%pointer = inttoptr i32 %address to i32*
			store i32 %value, i32* %pointer
			store i32 5, i32* %stack_var_-772
			store i32 6, i32* %stack_var_-768
			ret void
		}
	)");

	auto config = Config::empty(module.get());
	auto function = retdec::config::Function("func");
	for (int offset : {-776, -772, -768, -548})
	{
		std::string name = "stack_var_" + std::to_string(offset);
		function.locals.insert(retdec::config::Object(
				name, retdec::config::Storage::onStack(offset)));
	}
	config.getConfig().functions.insert(function);
	auto* abi = AbiProvider::addAbi(module.get(), &config);

	EXPECT_TRUE(pass.runOnModuleCustom(*module, &config, abi));
	EXPECT_TRUE(module->getFunction("func")->hasFnAttribute("retdec.stack.frame"));
	StackFrameCoalescing lowerFrame;
	EXPECT_TRUE(lowerFrame.runOnModuleCustom(*module, &config));
	auto* frame = cast<AllocaInst>(getValueByName("stack_frame"));
	EXPECT_EQ(232u, cast<ArrayType>(frame->getAllocatedType())->getNumElements());
	EXPECT_EQ(4u, frame->getAlignment());
	auto* firstMember = getValueByName("stack_var_-776.frame");
	auto* secondMember = getValueByName("stack_var_-772.frame");
	EXPECT_EQ(-776, config.getStackVariableOffset(firstMember).getValue());
	EXPECT_EQ(-772, config.getStackVariableOffset(secondMember).getValue());
	auto* base = cast<PtrToIntInst>(getValueByName("base"));
	EXPECT_EQ(firstMember, base->getPointerOperand());
	bool fixedMemberUsesFrame = false;
	for (BasicBlock& block : *module->getFunction("func"))
	for (Instruction& instruction : block)
	{
		if (auto* store = dyn_cast<StoreInst>(&instruction))
		{
			auto* value = dyn_cast<ConstantInt>(store->getValueOperand());
			fixedMemberUsesFrame |= value != nullptr && value->equalsInt(5)
					&& store->getPointerOperand() == secondMember;
		}
	}
	EXPECT_TRUE(fixedMemberUsesFrame);
	EXPECT_NE(nullptr, getValueByName("pointer.stack"));
	EXPECT_FALSE(lowerFrame.runOnModuleCustom(*module, &config));
}

TEST_F(StackAnalysisTests, restoresConfiguredWidthBeforeCoalescingAdjacentObjects)
{
	parseInput(R"(
		define i32 @func(i32 %counter) {
			%stack_var_-32 = alloca i64, align 4
			%stack_var_-28 = alloca i32, align 4
			%wide.counter = sext i32 %counter to i64
			store i64 %wide.counter, i64* %stack_var_-32, align 4
			store i32 305419896, i32* %stack_var_-28, align 4
			%wide.loaded = load i64, i64* %stack_var_-32, align 4
			%high = lshr i64 %wide.loaded, 32
			%high.i32 = trunc i64 %high to i32
			%cursor = load i32, i32* %stack_var_-28, align 4
			%result = add i32 %high.i32, %cursor
			ret i32 %result
		}
	)");

	auto config = Config::empty(module.get());
	auto function = retdec::config::Function("func");
	for (int offset : {-32, -28})
	{
		std::string name = "stack_var_" + std::to_string(offset);
		retdec::config::Object object(
				name, retdec::config::Storage::onStack(offset));
		object.type.setLlvmIr("i32*");
		function.locals.insert(object);
	}
	config.getConfig().functions.insert(function);

	module->getFunction("func")->addFnAttr("retdec.stack.frame");
	StackFrameCoalescing lowerFrame;
	EXPECT_TRUE(lowerFrame.runOnModuleCustom(*module, &config));

	auto* restored = cast<AllocaInst>(getValueByName("stack_var_-32"));
	EXPECT_TRUE(restored->getAllocatedType()->isIntegerTy(32));
	auto* firstMember = getValueByName("stack_var_-32.frame");
	auto* secondMember = getValueByName("stack_var_-28.frame");
	unsigned firstStores = 0;
	unsigned firstLoads = 0;
	for (BasicBlock& block : *module->getFunction("func"))
	for (Instruction& instruction : block)
	{
		if (auto* store = dyn_cast<StoreInst>(&instruction))
		{
			if (store->getPointerOperand() == firstMember)
			{
				EXPECT_TRUE(store->getValueOperand()->getType()->isIntegerTy(32));
				++firstStores;
			}
		}
		else if (auto* load = dyn_cast<LoadInst>(&instruction))
		{
			if (load->getPointerOperand() == firstMember)
			{
				EXPECT_TRUE(load->getType()->isIntegerTy(32));
				++firstLoads;
			}
		}
	}
	EXPECT_EQ(1u, firstStores);
	EXPECT_EQ(1u, firstLoads);
	EXPECT_EQ(-32, config.getStackVariableOffset(firstMember).getValue());
	EXPECT_EQ(-28, config.getStackVariableOffset(secondMember).getValue());
}

TEST_F(StackAnalysisTests, coalescesOverlappingLocalViewsWithoutDynamicFrameTag)
{
	parseInput(R"(
		define i8 @func(double %value) {
			%whole = alloca double
			%byte_view = alloca i8
			store double %value, double* %whole
			%byte = load i8, i8* %byte_view
			store i8 %byte, i8* %byte_view
			ret i8 %byte
		}
	)");

	auto config = Config::empty(module.get());
	auto function = retdec::config::Function("func");
	function.locals.insert(retdec::config::Object(
			"whole", retdec::config::Storage::onStack(-16)));
	function.locals.insert(retdec::config::Object(
			"byte_view", retdec::config::Storage::onStack(-13)));
	config.getConfig().functions.insert(function);

	StackFrameCoalescing lowerFrame;
	EXPECT_TRUE(lowerFrame.runOnModuleCustom(*module, &config));
	EXPECT_NE(nullptr, getValueByName("stack_frame"));
	LoadInst* byteLoad = nullptr;
	for (auto& block : *module->getFunction("func"))
	for (auto& instruction : block)
	{
		auto* load = dyn_cast<LoadInst>(&instruction);
		if (load != nullptr && load->getType()->isIntegerTy(8))
		{
			byteLoad = load;
		}
	}
	ASSERT_NE(nullptr, byteLoad);
	EXPECT_EQ(-13, config.getStackVariableOffset(
			byteLoad->getPointerOperand()).getValue());
	EXPECT_FALSE(module->getFunction("func")->hasFnAttribute(
			"retdec.stack.frame"));
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
