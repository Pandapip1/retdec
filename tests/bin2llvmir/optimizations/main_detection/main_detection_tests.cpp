/**
 * @file tests/bin2llvmir/optimizations/main_detection/main_detection_tests.cpp
 * @brief Tests for the @c MainDetection pass.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 */

#include "retdec/bin2llvmir/optimizations/main_detection/main_detection.h"
#include "bin2llvmir/utils/llvmir_tests.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

class MainDetectionTests : public LlvmIrTests
{
protected:
	MainDetection pass;
};

TEST_F(MainDetectionTests, SelectedStaticallyLinkedFunctionRetainsBody)
{
	parseInput(R"(
		define i32 @selected_library_body() {
			ret i32 1
		}
		define i32 @unselected_library_body() {
			ret i32 2
		}
	)");

	auto config = Config::empty(module.get());
	config.getConfig().fileType.setIsShared();
	config.insertFunction(
			module->getFunction("selected_library_body"), 0x1000, 0x1010);
	config.insertFunction(
			module->getFunction("unselected_library_body"), 0x2000, 0x2010);
	auto* selected = config.getConfigFunction(
			module->getFunction("selected_library_body"));
	auto* unselected = config.getConfigFunction(
			module->getFunction("unselected_library_body"));
	selected->setIsStaticallyLinked();
	unselected->setIsStaticallyLinked();
	config.getConfig().parameters.selectedRanges.insert(
			retdec::common::AddressRange(0x1000, 0x1010));

	pass.runOnModuleCustom(*module, &config);

	EXPECT_FALSE(module->getFunction("selected_library_body")->isDeclaration());
	EXPECT_TRUE(module->getFunction("unselected_library_body")->isDeclaration());
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
