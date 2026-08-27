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
			decoder._abi = AbiProvider::addAbi(module.get(), &config);
			decoder._abi->addRegister(
					X86_REG_EAX, module->getGlobalVariable("eax"));
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
	auto configJson = config::Config::fromJsonString(R"({
		"architecture" : {
			"bitSize" : 32,
			"endian" : "little",
			"name" : "x86"
		}
	})");
	auto config = Config::fromConfig(module.get(), configJson);

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

TEST_F(DecoderTests, conditionalTailTransferReturnsInsteadOfFallingThrough)
{
	parseInput(R"(
		@esp = global i32 0
		@eax = global i32 0

		define i32 @tail() {
		entry:
		  %sp = load i32, i32* @esp
		  store i32 %sp, i32* @eax
		  ret i32 undef
		}

		define i32 @caller(i1 %condition) {
		entry:
		  br i1 %condition, label %taken, label %fallthrough
		taken:
		  %result = call i32 @tail()
		  br label %fallthrough
		fallthrough:
		  store i32 42, i32* @eax
		  ret i32 undef
		}
	)");

	auto* caller = module->getFunction("caller");
	auto takenIt = caller->begin();
	++takenIt;
	auto* taken = &*takenIt;
	auto* call = dyn_cast<CallInst>(&taken->front());
	ASSERT_NE(nullptr, call);

	Decoder decoder;
	inlineSharedTailBranches(decoder, {{call, nullptr}});

	EXPECT_EQ(
			1u,
			std::distance(
					pred_begin(&caller->back()), pred_end(&caller->back())));
	unsigned tailLoads = 0;
	unsigned returns = 0;
	for (auto& block : *caller)
	for (auto& instruction : block)
	{
		tailLoads += isa<LoadInst>(&instruction)
				&& cast<LoadInst>(&instruction)->getPointerOperand()
						== module->getGlobalVariable("esp");
		returns += isa<ReturnInst>(&instruction);
	}
	EXPECT_EQ(1u, tailLoads);
	EXPECT_EQ(2u, returns);
	EXPECT_TRUE(module->getFunction("tail")->use_empty());
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
