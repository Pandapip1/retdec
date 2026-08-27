/**
 * @file tests/bin2llvmir/optimizations/inst_opt_rda/inst_opt_rda_tests.cpp
 * @brief Tests for the reaching-definitions instruction optimizer.
 * @copyright (c) 2026 Avast Software, licensed under the MIT license
 */

#include <llvm/IR/InstIterator.h>

#include "bin2llvmir/utils/llvmir_tests.h"
#include "retdec/bin2llvmir/optimizations/inst_opt_rda/inst_opt_rda_pass.h"
#include "retdec/bin2llvmir/providers/abi/x86.h"
#include "retdec/capstone2llvmir/x86/x86_defs.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

class InstructionRdaOptimizerTests : public LlvmIrTests
{
protected:
	InstructionRdaOptimizer pass;
};

TEST_F(InstructionRdaOptimizerTests,
		preservesArchitecturalRegisterStoreObservedByAnotherFunction)
{
	parseInput(R"(
		@st6 = internal global x86_fp80 0xK00000000000000000000
		define void @caller(x86_fp80 %value) {
		entry:
			store x86_fp80 %value, x86_fp80* @st6
			%result = call i32 @callee()
			ret void
		}
		define i32 @callee() {
		entry:
			%value = load x86_fp80, x86_fp80* @st6
			%result = fptosi x86_fp80 %value to i32
			ret i32 %result
		}
	)");
	auto config = Config::empty(module.get());
	AbiX86 abi(module.get(), &config);
	auto* architectural = getGlobalByName("st6");
	abi.addRegister(X86_REG_ST6, architectural);

	pass.runOnModuleCustom(*module, &abi);

	auto* caller = getFunctionByName("caller");
	bool preservedStore = false;
	for (auto& instruction : instructions(caller))
	{
		if (auto* store = dyn_cast<StoreInst>(&instruction))
		{
			preservedStore |= store->getPointerOperand() == architectural;
		}
	}
	EXPECT_TRUE(preservedStore);
}

TEST_F(InstructionRdaOptimizerTests,
		doesNotForwardArchitecturalRegisterValueAcrossCall)
{
	parseInput(R"(
		@top = internal global i3 0
		define i3 @caller() {
		entry:
			store i3 7, i3* @top
			call void @callee()
			%after.call = load i3, i3* @top
			ret i3 %after.call
		}
		define void @callee() {
		entry:
			%entry.top = load i3, i3* @top
			%popped = add i3 %entry.top, 1
			store i3 %popped, i3* @top
			ret void
		}
	)");
	auto config = Config::empty(module.get());
	AbiX86 abi(module.get(), &config);
	auto* architectural = getGlobalByName("top");
	abi.addRegister(X87_REG_TOP, architectural);

	pass.runOnModuleCustom(*module, &abi);

	auto* caller = getFunctionByName("caller");
	auto* returned = cast<ReturnInst>(caller->back().getTerminator())->getReturnValue();
	auto* loadAfterCall = dyn_cast<LoadInst>(returned);
	ASSERT_NE(nullptr, loadAfterCall);
	EXPECT_EQ(architectural, loadAfterCall->getPointerOperand());
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
