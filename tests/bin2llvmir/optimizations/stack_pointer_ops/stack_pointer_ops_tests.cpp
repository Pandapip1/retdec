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

class StackFrameCoalescingTests: public LlvmIrTests
{
	protected:
		StackFrameCoalescing pass;
};

TEST_F(StackFrameCoalescingTests,
		reservesMaximumAccumulatedOutgoingStackExcursion)
{
	parseInput(R"(
		define void @func(i1 %condition) {
		entry:
			%stack_var_-104 = alloca i32, align 4
			%stack_var_-4 = alloca i32, align 4
			%guest.esp = ptrtoint i32* %stack_var_-104 to i32
			br i1 %condition, label %short.path, label %long.path
		short.path:
			%short.esp = sub i32 %guest.esp, 16
			br label %join
		long.path:
			%long.esp = sub i32 %guest.esp, 32
			br label %join
		join:
			%path.esp = phi i32 [ %short.esp, %short.path ], [ %long.esp, %long.path ]
			%outgoing.esp = sub i32 %path.esp, 12
			%outgoing.pointer = inttoptr i32 %outgoing.esp to i32*
			store i32 7, i32* %outgoing.pointer, align 4
			ret void
		}
	)");
	auto config = Config::empty(module.get());
	auto function = retdec::common::Function("func");
	function.locals.insert(retdec::common::Object(
			"stack_var_-104", retdec::common::Storage::onStack(-104)));
	function.locals.insert(retdec::common::Object(
			"stack_var_-4", retdec::common::Storage::onStack(-4)));
	config.getConfig().functions.insert(function);

	EXPECT_TRUE(pass.runOnModuleCustom(*module, &config));

	auto* frame = llvm::cast<llvm::AllocaInst>(getValueByName("stack_frame"));
	auto* frameType = llvm::cast<llvm::ArrayType>(frame->getAllocatedType());
	// The coalesced native interval is [-148, 0): the two CFG alternatives
	// reach -120 and -136, then the final three argument words reach -148.
	EXPECT_EQ(148u, frameType->getNumElements());

	auto* oldAnchor = llvm::cast<llvm::AllocaInst>(
			getValueByName("stack_var_-104"));
	EXPECT_TRUE(oldAnchor->use_empty());
}

TEST_F(StackFrameCoalescingTests,
		expandsAnExistingFrameWhenLaterPassesExposeDeeperOutgoingAccess)
{
	parseInput(R"(
		define void @func() {
		entry:
			%stack_frame = alloca [108 x i8], align 4, !retdec.stack.frame.start !0
			%guest.esp = ptrtoint [108 x i8]* %stack_frame to i32
			%deep.esp = sub i32 %guest.esp, 272
			%deep.pointer = inttoptr i32 %deep.esp to i32*
			store i32 9, i32* %deep.pointer, align 4
			ret void
		}
		!0 = !{i64 -104}
	)");
	auto config = Config::empty(module.get());

	EXPECT_TRUE(pass.runOnModuleCustom(*module, &config));

	auto* frame = llvm::cast<llvm::AllocaInst>(getValueByName("stack_frame"));
	auto* frameType = llvm::cast<llvm::ArrayType>(frame->getAllocatedType());
	// Existing [-104, 4) plus a 0x110-byte below-ESP excursion gives
	// [-376, 4).  The old frame base is now 272 bytes into owned storage.
	EXPECT_EQ(380u, frameType->getNumElements());
	auto* oldBase = llvm::cast<llvm::GetElementPtrInst>(
			getValueByName("stack_frame.old.base"));
	auto* prefix = llvm::cast<llvm::ConstantInt>(oldBase->getOperand(2));
	EXPECT_EQ(272u, prefix->getZExtValue());
}

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

TEST_F(StackPointerOpsRemoveTests,
		removesLaterRegisterRestorePopWithAccumulatedStackDisplacement)
{
	parseInput(R"(
		@esp = global i32 0
		@edi = global i32 0
		@esi = global i32 0
		define i32 @func() {
			%stack_pointer_0 = load i32, i32* @esp
			%stack_address_0 = inttoptr i32 %stack_pointer_0 to i32*
			%popped_edi = load i32, i32* %stack_address_0
			store i32 %popped_edi, i32* @edi
			%stack_pointer_1 = load i32, i32* @esp
			%later_address = add i32 %stack_pointer_1, 4
			%stack_address_1 = inttoptr i32 %later_address to i32*
			%popped_esi = load i32, i32* %stack_address_1
			store i32 %popped_esi, i32* @esi
			ret i32 7
		}
	)");
	auto c = Config::empty(module.get());
	AbiX86 abi(module.get(), &c);
	abi.addRegister(X86_REG_ESP, getGlobalByName("esp"));
	abi.addRegister(X86_REG_EDI, getGlobalByName("edi"));
	abi.addRegister(X86_REG_ESI, getGlobalByName("esi"));

	EXPECT_TRUE(pass.runOnModuleCustom(*module, &abi));
	bool loadsSyntheticStackPointer = false;
	bool hasLaterAddress = false;
	for (auto& block : *module->getFunction("func"))
	for (auto& instruction : block)
	{
		hasLaterAddress |= instruction.getName() == "later_address";
		if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
		{
			loadsSyntheticStackPointer |=
					load->getPointerOperand() == module->getGlobalVariable("esp");
		}
	}
	EXPECT_FALSE(loadsSyntheticStackPointer);
	EXPECT_FALSE(hasLaterAddress);
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
			%esi.global-to-local = alloca i32
			%incoming = load i32, i32* @esi
			store i32 %incoming, i32* %esi.global-to-local
			%stack_pointer = load i32, i32* @esp
			%stack_address = inttoptr i32 %stack_pointer to i32*
			%popped = load i32, i32* %stack_address
			store i32 %popped, i32* %esi.global-to-local
			ret void
		}
	)");
	auto* localized = llvm::cast<llvm::AllocaInst>(
			getValueByName("esi.global-to-local"));
	auto c = Config::empty(module.get());
	AbiX86 abi(module.get(), &c);
	abi.addRegister(X86_REG_ESP, getGlobalByName("esp"));
	abi.addRegister(X86_REG_ESI, getGlobalByName("esi"));

	EXPECT_TRUE(pass.runOnModuleCustom(*module, &abi));
	bool hasPoppedValue = false;
	for (auto& block : *module->getFunction("func"))
	for (auto& instruction : block)
	{
		hasPoppedValue |= instruction.getName() == "popped";
	}
	EXPECT_FALSE(hasPoppedValue);
	EXPECT_EQ(1u, std::distance(localized->user_begin(), localized->user_end()));
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
