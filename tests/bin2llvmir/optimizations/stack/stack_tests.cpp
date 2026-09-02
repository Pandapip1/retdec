/**
* @file tests/bin2llvmir/optimizations/stack/stack_tests.cpp
* @brief Tests for the @c StackAnalysis pass.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
*/

#include <llvm/IR/InstIterator.h>

#include "retdec/bin2llvmir/optimizations/stack/stack.h"
#include "retdec/bin2llvmir/optimizations/stack_pointer_ops/stack_pointer_ops.h"
#include "retdec/bin2llvmir/providers/abi/abi.h"
#include "retdec/bin2llvmir/providers/asm_instruction.h"
#include "retdec/bin2llvmir/providers/config.h"
#include "bin2llvmir/utils/llvmir_tests.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

class StackAnalysisTests: public LlvmIrTests
{
	protected:
		struct SyntheticInstruction
		{
			cs_insn instruction = {};
			cs_detail detail = {};

			SyntheticInstruction()
			{
				instruction.detail = &detail;
			}
		};

		void setStackMemoryInstruction(
				SyntheticInstruction& instruction,
				unsigned id,
				int displacement)
		{
			instruction.instruction.id = id;
			instruction.instruction.size = 1;
			instruction.detail.x86.op_count = 1;
			auto& operand = instruction.detail.x86.operands[0];
			operand.type = X86_OP_MEM;
			operand.mem.base = X86_REG_ESP;
			operand.mem.index = X86_REG_INVALID;
			operand.mem.scale = 1;
			operand.mem.disp = displacement;
			operand.size = 4;
			operand.access = CS_AC_READ;
		}

		void setSimpleInstruction(
				SyntheticInstruction& instruction,
				unsigned id,
				x86_reg operand = X86_REG_INVALID)
		{
			instruction.instruction.id = id;
			instruction.instruction.size = 1;
			if (operand != X86_REG_INVALID)
			{
				instruction.detail.x86.op_count = 1;
				instruction.detail.x86.operands[0].type = X86_OP_REG;
				instruction.detail.x86.operands[0].reg = operand;
			}
		}

		void mapSyntheticInstructions(
				std::vector<SyntheticInstruction>& decoded)
		{
			AsmInstruction::setLlvmToAsmGlobalVariable(
					module.get(), module->getGlobalVariable("llvm2asm"));
			unsigned index = 0;
			for (Function& function : *module)
			for (Instruction& instruction : instructions(function))
			{
				auto* marker = dyn_cast<StoreInst>(&instruction);
				if (marker != nullptr && marker->getPointerOperand()
						== module->getGlobalVariable("llvm2asm"))
				{
					ASSERT_LT(index, decoded.size());
					decoded[index].instruction.address = 0x1000 + index;
					AsmInstruction::getLlvmToCapstoneInsnMap(module.get())[marker]
							= &decoded[index++].instruction;
				}
			}
			ASSERT_EQ(decoded.size(), index);
		}

		Abi* addX86Abi(Config& config)
		{
			config.getConfig().architecture.setIsX86();
			config.getConfig().architecture.setBitSize(32);
			return AbiProvider::addAbi(module.get(), &config);
		}

		StackAnalysis pass;
};

TEST_F(StackAnalysisTests, localizesIncomingStackSlotsAcrossBalancedRetryLoop)
{
	parseInput(R"(
		@esp = global i32 0
		@llvm2asm = global i64 0
		declare i32 @allocator()
		define i32 @func(i1 %retry) {
		entry:
			store volatile i64 4096, i64* @llvm2asm
			%entry.sp = load i32, i32* @esp
			%entry.address = add i32 %entry.sp, 4
			%entry.pointer = inttoptr i32 %entry.address to i32*
			%initial_size = load i32, i32* %entry.pointer
			br label %loop
		loop:
			store volatile i64 4097, i64* @llvm2asm
			%loop.sp = load i32, i32* @esp
			%loop.address = add i32 %loop.sp, 4
			%loop.pointer = inttoptr i32 %loop.address to i32*
			%loop_size = load i32, i32* %loop.pointer
			%pushed.sp = sub i32 %loop.sp, 4
			store i32 %pushed.sp, i32* @esp
			store volatile i64 4098, i64* @llvm2asm
			%allocated = call i32 @allocator()
			store volatile i64 4099, i64* @llvm2asm
			%called.sp = load i32, i32* @esp
			%popped.sp = add i32 %called.sp, 4
			store i32 %popped.sp, i32* @esp
			store volatile i64 4100, i64* @llvm2asm
			%compare.sp = load i32, i32* @esp
			%compare.address = add i32 %compare.sp, 8
			%compare.pointer = inttoptr i32 %compare.address to i32*
			%retry_limit = load i32, i32* %compare.pointer
			br i1 %retry, label %retry_block, label %done
		retry_block:
			store volatile i64 4101, i64* @llvm2asm
			%retry.sp = load i32, i32* @esp
			%retry.address = add i32 %retry.sp, 4
			%retry.pointer = inttoptr i32 %retry.address to i32*
			%retry_size = load i32, i32* %retry.pointer
			%retry.pushed.sp = sub i32 %retry.sp, 4
			store i32 %retry.pushed.sp, i32* @esp
			store volatile i64 4102, i64* @llvm2asm
			%retried = call i32 @allocator()
			store volatile i64 4103, i64* @llvm2asm
			%retry.called.sp = load i32, i32* @esp
			%retry.popped.sp = add i32 %retry.called.sp, 4
			store i32 %retry.popped.sp, i32* @esp
			br label %loop
		done:
			%sum1 = add i32 %initial_size, %loop_size
			%sum2 = add i32 %sum1, %retry_limit
			%sum3 = add i32 %sum2, %retry_size
			%sum4 = add i32 %sum3, %allocated
			%sum5 = add i32 %sum4, %retried
			ret i32 %sum5
		}
	)");

	auto config = Config::empty(module.get());
	config.getConfig().registers.insert(retdec::common::Object(
			"esp", retdec::common::Storage::inRegister("esp")));
	auto function = retdec::common::Function("func");
	function.locals.insert(retdec::common::Object(
			"first_argument", retdec::common::Storage::onStack(4)));
	for (const char* name : {"first_argument", "second_argument"})
	{
		retdec::common::Object parameter(name, retdec::common::Storage());
		parameter.type.setLlvmIr("i32");
		function.parameters.push_back(parameter);
	}
	function.callingConvention.setIsCdecl();
	config.getConfig().functions.insert(function);
	auto allocator = retdec::common::Function("allocator");
	allocator.callingConvention.setIsCdecl();
	config.getConfig().functions.insert(allocator);
	auto* abi = addX86Abi(config);
	abi->addRegister(X86_REG_ESP, module->getGlobalVariable("esp"));
	SymbolicTree::setAbi(abi);
	SymbolicTree::setConfig(&config);

	std::vector<SyntheticInstruction> decoded(8);
	setStackMemoryInstruction(decoded[0], X86_INS_CMP, 4);
	setStackMemoryInstruction(decoded[1], X86_INS_PUSH, 4);
	setSimpleInstruction(decoded[2], X86_INS_CALL);
	setSimpleInstruction(decoded[3], X86_INS_POP, X86_REG_ECX);
	setStackMemoryInstruction(decoded[4], X86_INS_CMP, 8);
	setStackMemoryInstruction(decoded[5], X86_INS_PUSH, 4);
	setSimpleInstruction(decoded[6], X86_INS_CALL);
	setSimpleInstruction(decoded[7], X86_INS_POP, X86_REG_ECX);
	mapSyntheticInstructions(decoded);

	pass.runOnModuleCustom(*module, &config, abi);
	for (const char* name : {"initial_size", "loop_size", "retry_size"})
	{
		auto* load = cast<LoadInst>(getValueByName(name));
		EXPECT_EQ(config.getLlvmStackVariable(
				module->getFunction("func"), 4), load->getPointerOperand());
	}
	auto* retryLimit = cast<LoadInst>(getValueByName("retry_limit"));
	EXPECT_EQ(config.getLlvmStackVariable(
			module->getFunction("func"), 8), retryLimit->getPointerOperand());
}

TEST_F(StackAnalysisTests, leavesIncomingStackSlotRawAfterUnequalStackDeltaMerge)
{
	parseInput(R"(
		@esp = global i32 0
		@llvm2asm = global i64 0
		define i32 @func(i1 %condition) {
		entry:
			br i1 %condition, label %balanced, label %pushed
		balanced:
			br label %merge
		pushed:
			store volatile i64 4096, i64* @llvm2asm
			%before_push = load i32, i32* @esp
			%after_push = sub i32 %before_push, 4
			store i32 %after_push, i32* @esp
			br label %merge
		merge:
			store volatile i64 4097, i64* @llvm2asm
			%merged.sp = load i32, i32* @esp
			%merged.address = add i32 %merged.sp, 4
			%merged.pointer = inttoptr i32 %merged.address to i32*
			%ambiguous_load = load i32, i32* %merged.pointer
			ret i32 %ambiguous_load
		}
	)");

	auto config = Config::empty(module.get());
	config.getConfig().registers.insert(retdec::common::Object(
			"esp", retdec::common::Storage::inRegister("esp")));
	auto function = retdec::common::Function("func");
	function.locals.insert(retdec::common::Object(
			"first_argument", retdec::common::Storage::onStack(4)));
	config.getConfig().functions.insert(function);
	auto* abi = addX86Abi(config);
	SymbolicTree::setAbi(abi);
	SymbolicTree::setConfig(&config);
	std::vector<SyntheticInstruction> decoded(2);
	setSimpleInstruction(decoded[0], X86_INS_PUSH, X86_REG_EAX);
	setStackMemoryInstruction(decoded[1], X86_INS_CMP, 4);
	mapSyntheticInstructions(decoded);

	pass.runOnModuleCustom(*module, &config, abi);
	auto* load = cast<LoadInst>(getValueByName("ambiguous_load"));
	EXPECT_EQ(getValueByName("merged.pointer"), load->getPointerOperand());
}

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
	config.getConfig().registers.insert(retdec::common::Object(
			"ebp", retdec::common::Storage::inRegister("ebp")));
	auto function = retdec::common::Function("func");
	function.locals.insert(retdec::common::Object(
			"stack_var_-192", retdec::common::Storage::onStack(-192)));
	function.locals.insert(retdec::common::Object(
			"stack_var_-156", retdec::common::Storage::onStack(-156)));
	function.locals.insert(retdec::common::Object(
			"stack_var_-4", retdec::common::Storage::onStack(-4)));
	config.getConfig().functions.insert(function);
	auto* abi = addX86Abi(config);

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
	config.getConfig().registers.insert(retdec::common::Object(
			"ebp", retdec::common::Storage::inRegister("ebp")));
	auto function = retdec::common::Function("func");
	function.locals.insert(retdec::common::Object(
			"stack_var_-12", retdec::common::Storage::onStack(-12)));
	function.locals.insert(retdec::common::Object(
			"stack_var_-4", retdec::common::Storage::onStack(-4)));
	config.getConfig().functions.insert(function);
	auto* abi = addX86Abi(config);

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
	config.getConfig().registers.insert(retdec::common::Object(
			"ebp", retdec::common::Storage::inRegister("ebp")));
	auto function = retdec::common::Function("func");
	for (int offset : {-400, -380, -192, -156, -4})
	{
		std::string name = "stack_var_" + std::to_string(offset);
		function.locals.insert(retdec::common::Object(
				name, retdec::common::Storage::onStack(offset)));
	}
	config.getConfig().functions.insert(function);
	auto* abi = addX86Abi(config);

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
	auto function = retdec::common::Function("func");
	for (int offset : {-776, -772, -768, -548})
	{
		std::string name = "stack_var_" + std::to_string(offset);
		function.locals.insert(retdec::common::Object(
				name, retdec::common::Storage::onStack(offset)));
	}
	config.getConfig().functions.insert(function);
	auto* abi = addX86Abi(config);

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
	EXPECT_EQ(-776, config.getStackVariableOffset(firstMember).value());
	EXPECT_EQ(-772, config.getStackVariableOffset(secondMember).value());
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
	auto function = retdec::common::Function("func");
	for (int offset : {-32, -28})
	{
		std::string name = "stack_var_" + std::to_string(offset);
		retdec::common::Object object(
				name, retdec::common::Storage::onStack(offset));
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
	EXPECT_EQ(-32, config.getStackVariableOffset(firstMember).value());
	EXPECT_EQ(-28, config.getStackVariableOffset(secondMember).value());
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
	auto function = retdec::common::Function("func");
	function.locals.insert(retdec::common::Object(
			"whole", retdec::common::Storage::onStack(-16)));
	function.locals.insert(retdec::common::Object(
			"byte_view", retdec::common::Storage::onStack(-13)));
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
			byteLoad->getPointerOperand()).value());
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
	auto function = retdec::common::Function("func");
	function.locals.insert(retdec::common::Object(
			"output_start", retdec::common::Storage::onStack(-24)));
	function.locals.insert(retdec::common::Object(
			"output_member", retdec::common::Storage::onStack(-18)));
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
	EXPECT_EQ(-24, config.getStackVariableOffset(start).value());
	EXPECT_EQ(-18, config.getStackVariableOffset(member).value());
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
	auto function = retdec::common::Function("func");
	function.locals.insert(retdec::common::Object(
			"stack_var_-84", retdec::common::Storage::onStack(-84)));
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
	config.getConfig().registers.insert(retdec::common::Object(
			"ebp", retdec::common::Storage::inRegister("ebp")));
	auto function = retdec::common::Function("func");
	function.locals.insert(retdec::common::Object(
			"stack_var_-192", retdec::common::Storage::onStack(-192)));
	function.locals.insert(retdec::common::Object(
			"stack_var_-156", retdec::common::Storage::onStack(-156)));
	function.locals.insert(retdec::common::Object(
			"stack_var_-4", retdec::common::Storage::onStack(-4)));
	config.getConfig().functions.insert(function);
	auto* abi = addX86Abi(config);

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
