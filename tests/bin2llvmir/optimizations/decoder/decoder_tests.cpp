/**
* @file tests/bin2llvmir/optimizations/decoder/decoder_tests.cpp
* @brief Tests for the @c Decoder pass.
* @copyright (c) 2026 Avast Software, licensed under the MIT license
*/

#include "retdec/bin2llvmir/optimizations/decoder/decoder.h"
#include "bin2llvmir/utils/llvmir_tests.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

class DecoderTests: public LlvmIrTests
{
	protected:
		CallInst* transformToIndirectCall(
				Decoder& decoder,
				Config& config,
				CallInst* pseudo,
				Value* target)
		{
			decoder._module = module.get();
			decoder._config = &config;
			return decoder.transformToIndirectCall(pseudo, target);
		}
};

TEST_F(DecoderTests, unresolvedComputedCallIsPreservedAsIndirectCall)
{
	parseInput(R"(
		@eax = global i32 0

		declare void @__pseudo_call(i32)

		define void @caller(i32 %target) {
		entry:
		  call void @__pseudo_call(i32 %target)
		  ret void
		}
	)");
	auto config = Config::fromJsonString(module.get(), R"({
		"architecture" : {
			"bitSize" : 32,
			"endian" : "little",
			"name" : "x86"
		}
	})");

	auto* caller = module->getFunction("caller");
	auto* pseudo = dyn_cast<CallInst>(&caller->getEntryBlock().front());
	ASSERT_NE(nullptr, pseudo);
	Value* target = &*caller->arg_begin();

	Decoder decoder;
	auto* indirect = transformToIndirectCall(
			decoder, config, pseudo, target);
	ASSERT_NE(nullptr, indirect);
	EXPECT_EQ(nullptr, indirect->getCalledFunction());
	EXPECT_EQ(0u, indirect->getNumArgOperands());
	EXPECT_TRUE(indirect->getType()->isIntegerTy(32));

	auto* targetCast = dyn_cast<IntToPtrInst>(indirect->getCalledValue());
	ASSERT_NE(nullptr, targetCast);
	EXPECT_EQ(target, targetCast->getOperand(0));

	auto* resultStore = dyn_cast<StoreInst>(indirect->getNextNode());
	ASSERT_NE(nullptr, resultStore);
	EXPECT_EQ(indirect, resultStore->getValueOperand());
	EXPECT_EQ(module->getGlobalVariable("eax"),
			resultStore->getPointerOperand());
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
