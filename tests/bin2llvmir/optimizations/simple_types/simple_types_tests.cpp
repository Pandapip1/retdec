/**
* @file tests/bin2llvmir/optimizations/simple_types/simple_types_tests.cpp
* @brief Tests for the @c SimpleTypesAnalysis pass.
* @copyright (c) 2026 Avast Software, licensed under the MIT license
*/

#include "retdec/bin2llvmir/analyses/reaching_definitions.h"
#include "retdec/bin2llvmir/optimizations/simple_types/simple_types.h"
#include "retdec/bin2llvmir/providers/abi/abi.h"
#include "retdec/bin2llvmir/providers/fileimage.h"
#include "bin2llvmir/utils/llvmir_tests.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

class SimpleTypesTests: public LlvmIrTests
{
};

TEST_F(SimpleTypesTests, inferredUsesDoNotChangeExportedParameterAbi)
{
	parseInput(R"(
		define i32 @divide(i32 %dividend, i32 %divisor) {
		entry:
		  %wide = sext i32 %dividend to i64
		  %quotient = sdiv i64 %wide, 7
		  %address = inttoptr i32 %divisor to i8*
		  store i8 0, i8* %address
		  %result = trunc i64 %quotient to i32
		  ret i32 %result
		}
	)");
	auto configJson = config::Config::fromJsonString(R"({
		"architecture" : {
			"bitSize" : 32,
			"endian" : "little",
			"name" : "x86"
		},
		"functions" : [
			{
				"isExported" : true,
				"name" : "divide",
				"parameters" : [
					{ "name" : "dividend", "type" : { "llvmIr" : "i32" } },
					{ "name" : "divisor", "type" : { "llvmIr" : "i32" } }
				]
			}
		]
	})");
	auto* config = ConfigProvider::addConfig(module.get(), configJson);
	ASSERT_NE(nullptr, config);
	FileImageProvider::addFileImage(module.get(), createFormat(), config);
	AbiProvider::addAbi(module.get(), config);

	SimpleTypesAnalysis pass;
	pass.runOnModule(*module);

	auto* function = module->getFunction("divide");
	ASSERT_NE(nullptr, function);
	ASSERT_EQ(2u, function->arg_size());
	EXPECT_TRUE(function->getFunctionType()->getParamType(0)->isIntegerTy(32));
	EXPECT_TRUE(function->getFunctionType()->getParamType(1)->isIntegerTy(32));

	auto* wide = getValueByName("wide");
	ASSERT_NE(nullptr, wide);
	EXPECT_TRUE(wide->getType()->isIntegerTy(64));
}

TEST_F(SimpleTypesTests, inferredWideUseDoesNotWidenRecoveredInternalParameterSlot)
{
	parseInput(R"(
		define i32 @callee(i32 %arg) {
		entry:
		  %wide = sext i32 %arg to i64
		  %sum = add i64 %wide, 1
		  %result = trunc i64 %sum to i32
		  ret i32 %result
		}

		define i32 @caller(i32 %value) {
		entry:
		  %result = call i32 @callee(i32 %value)
		  ret i32 %result
		}
	)");
	auto configJson = config::Config::fromJsonString(R"({
		"architecture" : {
			"bitSize" : 32,
			"endian" : "little",
			"name" : "x86"
		},
		"functions" : [
			{
				"name" : "callee",
				"parameters" : [
					{ "name" : "arg", "type" : { "llvmIr" : "i32" } }
				]
			}
		]
	})");
	auto* config = ConfigProvider::addConfig(module.get(), configJson);
	ASSERT_NE(nullptr, config);
	FileImageProvider::addFileImage(module.get(), createFormat(), config);
	AbiProvider::addAbi(module.get(), config);

	SimpleTypesAnalysis pass;
	pass.runOnModule(*module);

	auto* callee = module->getFunction("callee");
	ASSERT_NE(nullptr, callee);
	EXPECT_TRUE(callee->getFunctionType()->getParamType(0)->isIntegerTy(32));
	auto* caller = module->getFunction("caller");
	ASSERT_NE(nullptr, caller);
	auto* call = dyn_cast<CallInst>(getNthInstruction<CallInst>());
	ASSERT_NE(nullptr, call);
	EXPECT_TRUE(call->getArgOperand(0)->getType()->isIntegerTy(32));
}

TEST_F(SimpleTypesTests, inferredNarrowUseDoesNotShrinkRecoveredGlobalStorage)
{
	parseInput(R"(
		@tls_index = global i32 -1

		define i1 @is_tls_unallocated() {
		entry:
		  %index = load i32, i32* @tls_index
		  %flag = trunc i32 %index to i1
		  ret i1 %flag
		}
	)");
	auto configJson = config::Config::fromJsonString(R"({
		"architecture" : {
			"bitSize" : 32,
			"endian" : "little",
			"name" : "x86"
		},
		"globals" : [
			{
				"name" : "tls_index",
				"storage" : {
					"type" : "global",
					"value" : "0x1000"
				},
				"type" : { "llvmIr" : "i32" }
			}
		]
	})");
	auto* config = ConfigProvider::addConfig(module.get(), configJson);
	ASSERT_NE(nullptr, config);
	FileImageProvider::addFileImage(module.get(), createFormat(), config);
	AbiProvider::addAbi(module.get(), config);

	// SimpleTypes alternates its analysis and string-refinement phases.  Running
	// twice makes the regression independent of which phase the process starts
	// with and exercises the inference phase exactly once.
	SimpleTypesAnalysis firstPass;
	firstPass.runOnModule(*module);
	SimpleTypesAnalysis secondPass;
	secondPass.runOnModule(*module);

	auto* global = module->getGlobalVariable("tls_index");
	ASSERT_NE(nullptr, global);
	EXPECT_TRUE(global->getValueType()->isIntegerTy(32));
	auto* configured = config->getConfig().globals.getObjectByName("tls_index");
	ASSERT_NE(nullptr, configured);
	EXPECT_EQ("i32", configured->type.getLlvmIr());
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
