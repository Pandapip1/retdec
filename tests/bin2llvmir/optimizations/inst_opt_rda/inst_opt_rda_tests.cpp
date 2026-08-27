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

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
