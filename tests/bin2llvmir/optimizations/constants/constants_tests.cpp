/**
* @file tests/bin2llvmir/optimizations/constants/constants_tests.cpp
* @brief Tests for the ConstantsAnalysis pass.
* @copyright (c) 2026 Avast Software, licensed under the MIT license
*/

#include "retdec/bin2llvmir/optimizations/constants/constants.h"
#include "retdec/bin2llvmir/analyses/symbolic_tree.h"
#include "retdec/bin2llvmir/providers/abi/abi.h"
#include "retdec/bin2llvmir/providers/fileimage.h"
#include "bin2llvmir/utils/llvmir_tests.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

class ConstantsTests: public LlvmIrTests
{
	protected:
		void TearDown() override
		{
			SymbolicTree::setAbi(nullptr);
			SymbolicTree::setConfig(nullptr);
			LlvmIrTests::TearDown();
		}
};

TEST_F(ConstantsTests, preservesRegisterIndirectWideLoad)
{
	parseInput(R"(
		@edx = global i32 0
		define double @func() {
			%address = load i32, i32* @edx
			%pointer = inttoptr i32 %address to double*
			%value = load double, double* %pointer
			ret double %value
		}
	)");
	auto* config = ConfigProvider::addConfigJsonString(module.get(), R"({
		"architecture" : {
			"bitSize" : 32,
			"endian" : "little",
			"name" : "x86"
		},
		"registers" : [
			{
				"name" : "edx",
				"storage" : { "type" : "register", "value" : "edx" }
			}
		]
	})");
	ASSERT_NE(nullptr, config);
	FileImageProvider::addFileImage(module.get(), createFormat(), config);
	auto* abi = AbiProvider::addAbi(module.get(), config);
	SymbolicTree::setAbi(abi);
	SymbolicTree::setConfig(config);

	ConstantsAnalysis pass;
	pass.runOnModule(*module);

	auto* value = cast<LoadInst>(getValueByName("value"));
	auto* pointer = cast<IntToPtrInst>(value->getPointerOperand());
	auto* address = cast<LoadInst>(pointer->getOperand(0));
	EXPECT_EQ(module->getGlobalVariable("edx"), address->getPointerOperand());
}

TEST_F(ConstantsTests, preservesIndirectLoadThroughPointerGlobal)
{
	parseInput(R"(
		@pointer = global i16* null
		define i16 @func() {
			%runtime_pointer = load i16*, i16** @pointer
			%value = load i16, i16* %runtime_pointer
			ret i16 %value
		}
	)");
	auto* config = ConfigProvider::addConfigJsonString(module.get(), R"({
		"architecture" : {
			"bitSize" : 32,
			"endian" : "little",
			"name" : "x86"
		}
	})");
	ASSERT_NE(nullptr, config);
	FileImageProvider::addFileImage(module.get(), createFormat(), config);
	auto* abi = AbiProvider::addAbi(module.get(), config);
	SymbolicTree::setAbi(abi);
	SymbolicTree::setConfig(config);

	ConstantsAnalysis pass;
	pass.runOnModule(*module);

	auto* value = cast<LoadInst>(getValueByName("value"));
	auto* runtimePointer = cast<LoadInst>(value->getPointerOperand());
	EXPECT_EQ(module->getGlobalVariable("pointer"),
			runtimePointer->getPointerOperand());
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
