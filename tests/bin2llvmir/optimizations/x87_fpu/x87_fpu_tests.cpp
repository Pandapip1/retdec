/**
* @file tests/bin2llvmir/optimizations/x87_fpu/x87_fpu_tests.cpp
* @brief Tests for the @c X87FpuAnalysis pass.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
*/

#include <set>

#include "retdec/bin2llvmir/optimizations/x87_fpu/x87_fpu.h"
#include "retdec/bin2llvmir/providers/abi/abi.h"
#include "retdec/capstone2llvmir/x86/x86_defs.h"
#include "bin2llvmir/utils/llvmir_tests.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

class X87FpuAnalysisTests: public LlvmIrTests
{
	protected:
		X87FpuAnalysis pass;
};

TEST_F(X87FpuAnalysisTests, usesRuntimeTopForValuesPassedAcrossCalls)
{
	AbiProvider::clear();
	parseInput(R"(
		@fpu_stat_TOP = global i3 0
		@st0 = global x86_fp80 0xK00000000000000000000
		@st1 = global x86_fp80 0xK00000000000000000000
		@st2 = global x86_fp80 0xK00000000000000000000
		@st3 = global x86_fp80 0xK00000000000000000000
		@st4 = global x86_fp80 0xK00000000000000000000
		@st5 = global x86_fp80 0xK00000000000000000000
		@st6 = global x86_fp80 0xK00000000000000000000
		@st7 = global x86_fp80 0xK00000000000000000000
		declare void @__frontend_reg_store.fpr(i3, x86_fp80)
		declare x86_fp80 @__frontend_reg_load.fpr(i3)
		declare void @callee()
		define x86_fp80 @caller(x86_fp80 %value) {
			%top.before = load i3, i3* @fpu_stat_TOP
			%pushed = sub i3 %top.before, 1
			store i3 %pushed, i3* @fpu_stat_TOP
			call void @__frontend_reg_store.fpr(i3 %pushed, x86_fp80 %value)
			call void @callee()
			%top.after = load i3, i3* @fpu_stat_TOP
			%result = call x86_fp80 @__frontend_reg_load.fpr(i3 %top.after)
			ret x86_fp80 %result
		}
	)");

	auto config = Config::fromJsonString(module.get(), R"({
		"architecture" : {
			"bitSize" : 32,
			"endian" : "little",
			"name" : "x86"
		}
	})");
	config.setLlvmX87DataStorePseudoFunction(
			module->getFunction("__frontend_reg_store.fpr"));
	config.setLlvmX87DataLoadPseudoFunction(
			module->getFunction("__frontend_reg_load.fpr"));
	auto* abi = AbiProvider::addAbi(module.get(), &config);
	abi->addRegister(X87_REG_TOP, module->getGlobalVariable("fpu_stat_TOP"));

	std::set<Value*> stackRegisters;
	for (unsigned n = 0; n < 8; ++n)
	{
		auto* reg = module->getGlobalVariable("st" + std::to_string(n));
		abi->addRegister(X86_REG_ST0 + n, reg);
		stackRegisters.insert(reg);
	}

	EXPECT_TRUE(pass.runOnModuleCustom(*module, &config, abi));

	auto* caller = module->getFunction("caller");
	auto* pushed = getValueByName("pushed");
	auto* topAfter = getValueByName("top.after");
	unsigned stackLoads = 0;
	unsigned stackStores = 0;
	unsigned pseudoCalls = 0;
	unsigned ordinaryCalls = 0;
	for (BasicBlock& block : *caller)
	for (Instruction& instruction : block)
	{
		if (auto* load = dyn_cast<LoadInst>(&instruction))
		{
			stackLoads += stackRegisters.count(load->getPointerOperand());
		}
		else if (auto* store = dyn_cast<StoreInst>(&instruction))
		{
			if (stackRegisters.count(store->getPointerOperand()))
			{
				++stackStores;
				auto* select = dyn_cast<SelectInst>(store->getValueOperand());
				ASSERT_NE(nullptr, select);
				auto* condition = dyn_cast<ICmpInst>(select->getCondition());
				ASSERT_NE(nullptr, condition);
				EXPECT_EQ(pushed, condition->getOperand(0));
			}
		}
		else if (auto* call = dyn_cast<CallInst>(&instruction))
		{
			if (call->getCalledFunction() == config.getLlvmX87DataStorePseudoFunction()
					|| call->getCalledFunction() == config.getLlvmX87DataLoadPseudoFunction())
			{
				++pseudoCalls;
			}
			else
			{
				++ordinaryCalls;
			}
		}
	}

	// A runtime-indexed store reads and conditionally updates all eight slots;
	// the later runtime-indexed load reads all eight again after the call.
	EXPECT_EQ(16u, stackLoads);
	EXPECT_EQ(8u, stackStores);
	EXPECT_EQ(0u, pseudoCalls);
	EXPECT_EQ(1u, ordinaryCalls);
	auto* returned = cast<ReturnInst>(caller->back().getTerminator())->getReturnValue();
	auto* resultSelect = dyn_cast<SelectInst>(returned);
	ASSERT_NE(nullptr, resultSelect);
	auto* condition = dyn_cast<ICmpInst>(resultSelect->getCondition());
	ASSERT_NE(nullptr, condition);
	EXPECT_EQ(topAfter, condition->getOperand(0));
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
