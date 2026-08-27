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
	auto* config = ConfigProvider::addConfigJsonString(module.get(), R"({
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

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
