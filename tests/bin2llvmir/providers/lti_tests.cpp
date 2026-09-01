/**
* @file tests/bin2llvmir/providers/tests/lti_tests.cpp
* @brief Tests for the @c LtiProvider.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
*/

#include "retdec/ctypes/floating_point_type.h"
#include "retdec/ctypes/function_type.h"
#include "retdec/ctypes/integral_type.h"
#include "retdec/ctypes/member.h"
#include "retdec/ctypes/pointer_type.h"
#include "retdec/ctypes/struct_type.h"
#include "retdec/ctypes/typedefed_type.h"
#include "retdec/ctypes/union_type.h"
#include "retdec/ctypes/unknown_type.h"
#include "retdec/ctypes/void_type.h"
#include "retdec/bin2llvmir/providers/lti.h"
#include "bin2llvmir/utils/llvmir_tests.h"
#include "retdec/bin2llvmir/utils/ctypes2llvm.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

//
//=============================================================================
//  LtiTests
//=============================================================================
//

/**
 * @brief Tests for the @c Lti.
 */
class LtiTests: public LlvmIrTests
{

};

TEST_F(LtiTests, PeImportsUseConfiguredWindowsPrototypes)
{
	auto config = Config::empty(module.get());
	config.getConfig().architecture.setIsX86();
	config.getConfig().architecture.setBitSize(32);
	config.getConfig().fileFormat.setIsPe32();
	config.getConfig().parameters.libraryTypeInfoPaths = {
			RETDEC_TEST_WINDOWS_TYPES};
	auto image = FileImage(module.get(), createFormat(), &config);
	auto typeConfig = std::make_shared<ctypesparser::TypeConfig>();
	Lti lti(module.get(), &config, typeConfig, image.getImage());

	for (const auto& expected : std::vector<std::pair<const char*, unsigned>>{
			{"GetLastError", 0},
			{"GetStdHandle", 1},
			{"HeapAlloc", 3},
			{"HeapFree", 3},
			{"HeapReAlloc", 4},
			{"LoadLibraryA", 1},
			{"WriteFile", 5}})
	{
		auto* type = lti.getLlvmFunctionType(expected.first);
		ASSERT_NE(nullptr, type) << expected.first;
		EXPECT_EQ(expected.second, type->getNumParams()) << expected.first;
	}
}

TEST_F(LtiTests, MissingConfiguredTypeInformationFailsClosed)
{
	auto config = Config::empty(module.get());
	config.getConfig().architecture.setIsX86();
	config.getConfig().architecture.setBitSize(32);
	config.getConfig().fileFormat.setIsPe32();
	config.getConfig().parameters.libraryTypeInfoPaths = {
			"/definitely/missing/windows.json"};
	auto image = FileImage(module.get(), createFormat(), &config);
	auto typeConfig = std::make_shared<ctypesparser::TypeConfig>();

	EXPECT_THROW(
			Lti(module.get(), &config, typeConfig, image.getImage()),
			std::runtime_error);
}

//
//=============================================================================
//  LtiProviderTests
//=============================================================================
//

/**
 * @brief Tests for the @c LtiProviderTests.
 */
class LtiProviderTests: public LlvmIrTests
{

};

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
