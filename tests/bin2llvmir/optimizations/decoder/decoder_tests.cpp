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

		void inlineSharedTailBranches(
				Decoder& decoder,
				const std::vector<std::pair<CallInst*, StoreInst*>>& calls)
		{
			decoder._module = module.get();
			decoder._llvm2capstone =
					&AsmInstruction::getLlvmToCapstoneInsnMap(module.get());
			for (const auto& call : calls)
			{
				decoder._splitBranchCalls.push_back(
						{call.first, call.second, nullptr});
			}
			decoder.inlineSharedTailBranches();
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

TEST_F(DecoderTests, branchesFromMultipleEntriesInlineSharedTailState)
{
	parseInput(R"(
		@eax = global i32 0
		@edi = global i32 0

		define i32 @shared_tail() {
		entry:
		  %live_edi = load i32, i32* @edi
		  store i32 %live_edi, i32* @eax
		  ret i32 undef
		}

		define i32 @entry_one(i32 %value) {
		entry:
		  store i32 %value, i32* @edi
		  %result = call i32 @shared_tail()
		  store i32 %result, i32* @eax
		  ret i32 undef
		}

		define i32 @entry_two(i32 %value) {
		entry:
		  store i32 %value, i32* @edi
		  %result = call i32 @shared_tail()
		  store i32 %result, i32* @eax
		  ret i32 undef
		}
	)");

	std::vector<std::pair<CallInst*, StoreInst*>> branches;
	for (auto* name : {"entry_one", "entry_two"})
	{
		auto* function = module->getFunction(name);
		CallInst* call = nullptr;
		StoreInst* bridge = nullptr;
		for (auto& instruction : function->front())
		{
			if (auto* candidate = dyn_cast<CallInst>(&instruction))
			{
				call = candidate;
			}
			else if (auto* store = dyn_cast<StoreInst>(&instruction))
			{
				if (call != nullptr && store->getValueOperand() == call)
				{
					bridge = store;
				}
			}
		}
		ASSERT_NE(nullptr, call);
		ASSERT_NE(nullptr, bridge);
		branches.emplace_back(call, bridge);
	}

	Decoder decoder;
	inlineSharedTailBranches(decoder, branches);

	for (auto* name : {"entry_one", "entry_two"})
	{
		auto* function = module->getFunction(name);
		unsigned calls = 0;
		unsigned eaxStores = 0;
		for (auto& block : *function)
		for (auto& instruction : block)
		{
			calls += isa<CallInst>(&instruction);
			if (auto* store = dyn_cast<StoreInst>(&instruction))
			{
				eaxStores += store->getPointerOperand()
						== module->getGlobalVariable("eax");
			}
		}
		EXPECT_EQ(0u, calls);
		EXPECT_EQ(1u, eaxStores);
	}
	EXPECT_TRUE(module->getFunction("shared_tail")->use_empty());
	EXPECT_TRUE(module->getFunction("shared_tail")->hasInternalLinkage());
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
