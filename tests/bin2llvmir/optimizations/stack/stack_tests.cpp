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

TEST_F(StackAnalysisTests, preservesOverlappingQwordAndDwordAccessWidths)
{
	parseInput(R"(
		@ebp = global i32 0
		define i32 @func(i32 %index, double %value) {
			%stack_var_-12 = alloca i32
			%stack_var_-4 = alloca i32
			%anchor = ptrtoint i32* %stack_var_-4 to i32
			store i32 %anchor, i32* @ebp
			%base1 = load i32, i32* @ebp
			%scaled1 = mul i32 %index, 4
			%displaced1 = add i32 %scaled1, -8
			%address1 = add i32 %base1, %displaced1
			%qword = inttoptr i32 %address1 to double*
			store double %value, double* %qword
			%base2 = load i32, i32* @ebp
			%scaled2 = mul i32 %index, 4
			%displaced2 = add i32 %scaled2, -8
			%address2 = add i32 %base2, %displaced2
			%dword = inttoptr i32 %address2 to i32*
			%low = load i32, i32* %dword
			ret i32 %low
		}
	)");

	auto config = Config::empty(module.get());
	config.getConfig().registers.insert(retdec::config::Object(
			"ebp", retdec::config::Storage::inRegister("ebp")));
	auto function = retdec::config::Function("func");
	function.locals.insert(retdec::config::Object(
			"stack_var_-12", retdec::config::Storage::onStack(-12)));
	function.locals.insert(retdec::config::Object(
			"stack_var_-4", retdec::config::Storage::onStack(-4)));
	config.getConfig().functions.insert(function);
	auto* abi = AbiProvider::addAbi(module.get(), &config);

	auto changed = pass.runOnModuleCustom(*module, &config, abi);
	EXPECT_TRUE(changed);
	auto* stackObject = cast<AllocaInst>(getValueByName("stack_var_-12"));
	EXPECT_TRUE(stackObject->getAllocatedType()->isIntegerTy(32));
	StoreInst* qwordStore = nullptr;
	for (BasicBlock& block : *module->getFunction("func"))
	for (Instruction& instruction : block)
	{
		auto* store = dyn_cast<StoreInst>(&instruction);
		if (store != nullptr && store->getValueOperand()->getType()->isDoubleTy())
		{
			qwordStore = store;
		}
	}
	ASSERT_NE(nullptr, qwordStore);
	EXPECT_TRUE(qwordStore->getPointerOperand()->getType()
			->getPointerElementType()->isDoubleTy());
	auto* low = cast<LoadInst>(getValueByName("low"));
	EXPECT_TRUE(low->getType()->isIntegerTy(32));
	EXPECT_TRUE(low->getPointerOperand()->getType()
			->getPointerElementType()->isIntegerTy(32));
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
	// The indexed pointer escapes scalar-object reasoning, so retain the whole
	// native local-frame interval through saved EBP at offset -4.
	EXPECT_EQ(772u, cast<ArrayType>(frame->getAllocatedType())->getNumElements());
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

TEST_F(StackAnalysisTests, coalescesDisjointViewsOfAddressEscapedStackOutput)
{
	parseInput(R"(
		declare void @write_output(i32*)
		define i8 @func() {
			%output_start = alloca i32
			%output_member = alloca i8
			call void @write_output(i32* %output_start)
			%member = load i8, i8* %output_member
			ret i8 %member
		}
	)");

	auto config = Config::empty(module.get());
	auto function = retdec::config::Function("func");
	function.locals.insert(retdec::config::Object(
			"output_start", retdec::config::Storage::onStack(-24)));
	function.locals.insert(retdec::config::Object(
			"output_member", retdec::config::Storage::onStack(-18)));
	config.getConfig().functions.insert(function);

	StackFrameCoalescing lowerFrame;
	EXPECT_TRUE(lowerFrame.runOnModuleCustom(*module, &config));
	auto* frame = cast<AllocaInst>(getValueByName("stack_frame"));
	// The escaped pointer can name an aggregate extending through the end of
	// the native local frame (saved EBP begins at offset -4).
	EXPECT_EQ(20u, cast<ArrayType>(frame->getAllocatedType())->getNumElements());
	auto* start = getValueByName("output_start.frame");
	auto* member = getValueByName("output_member.frame.addr");
	ASSERT_NE(nullptr, start);
	ASSERT_NE(nullptr, member);
	EXPECT_EQ(-24, config.getStackVariableOffset(start).getValue());
	EXPECT_EQ(-18, config.getStackVariableOffset(member).getValue());
	CallInst* outputCall = nullptr;
	LoadInst* memberLoad = nullptr;
	for (auto& block : *module->getFunction("func"))
	for (auto& instruction : block)
	{
		if (auto* call = dyn_cast<CallInst>(&instruction))
		{
			outputCall = call;
		}
		else if (auto* load = dyn_cast<LoadInst>(&instruction))
		{
			memberLoad = load;
		}
	}
	ASSERT_NE(nullptr, outputCall);
	ASSERT_NE(nullptr, memberLoad);
	EXPECT_EQ(start, outputCall->getArgOperand(0));
	EXPECT_EQ(member, memberLoad->getPointerOperand());
}

TEST_F(StackAnalysisTests, reservesNativeLocalFrameForEscapedScalarAnchor)
{
	parseInput(R"(
		declare void @copy_into(i8*, i32)
		define void @func() {
			%stack_var_-84 = alloca i32
			%destination = bitcast i32* %stack_var_-84 to i8*
			call void @copy_into(i8* %destination, i32 80)
			ret void
		}
	)");

	auto config = Config::empty(module.get());
	auto function = retdec::config::Function("func");
	function.locals.insert(retdec::config::Object(
			"stack_var_-84", retdec::config::Storage::onStack(-84)));
	config.getConfig().functions.insert(function);

	StackFrameCoalescing lowerFrame;
	EXPECT_TRUE(lowerFrame.runOnModuleCustom(*module, &config));
	auto* frame = cast<AllocaInst>(getValueByName("stack_frame"));
	EXPECT_EQ(80u, cast<ArrayType>(frame->getAllocatedType())->getNumElements());
	auto* destination = cast<BitCastInst>(getValueByName("destination"));
	EXPECT_EQ(getValueByName("stack_var_-84.frame"),
			destination->getOperand(0));
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
