/**
 * @file tests/bin2llvmir/pe32_runtime_pic_tests.cpp
 * @brief End-to-end high-address PIC tests for the PE32 pointer runtime.
 * @copyright (c) 2026 Avast Software, licensed under the MIT license
 */

#include <cstdint>
#include <string>

#include <dlfcn.h>
#include <gtest/gtest.h>

#include "retdec/bin2llvmir/pe32_runtime.h"

namespace retdec {
namespace bin2llvmir {
namespace tests {

namespace {

template<typename Function>
Function loadFunction(void* module, const char* name)
{
	dlerror();
	auto* symbol = dlsym(module, name);
	EXPECT_EQ(nullptr, dlerror());
	EXPECT_NE(nullptr, symbol);
	return reinterpret_cast<Function>(symbol);
}

} // anonymous namespace

TEST(Pe32PointerRuntimePicTests,
		roundTripsHighStackGlobalAndFunctionPointersAcrossDsoBoundary)
{
	auto* module = dlopen(RETDEC_PE32_RUNTIME_PIC_FIXTURE, RTLD_NOW | RTLD_LOCAL);
	ASSERT_NE(nullptr, module) << dlerror();

	auto globalHost = loadFunction<std::uintptr_t (*)()>(
			module, "pe32_pic_global_host");
	auto functionHost = loadFunction<std::uintptr_t (*)()>(
			module, "pe32_pic_function_host");
	auto globalGuest = loadFunction<std::uint32_t (*)()>(
			module, "pe32_pic_global_guest");
	auto functionGuest = loadFunction<std::uint32_t (*)()>(
			module, "pe32_pic_function_guest");
	auto pointerCell = loadFunction<std::uint32_t (*)()>(
			module, "pe32_pic_pointer_cell");
	auto decodePointerCell = loadFunction<void* (*)()>(
			module, "pe32_pic_decode_pointer_cell");
	auto callMappedFunction = loadFunction<int (*)(int)>(
			module, "pe32_pic_call_mapped_function");
	auto stackRoundTrip = loadFunction<int (*)(std::uintptr_t*)>(
			module, "pe32_pic_stack_round_trip");

	ASSERT_GT(globalHost(), UINT32_MAX);
	ASSERT_GT(functionHost(), UINT32_MAX);
	EXPECT_EQ(0x401000u, globalGuest());
	EXPECT_EQ(0x402000u, functionGuest());
	EXPECT_EQ(0x401000u, pointerCell());
	EXPECT_EQ(reinterpret_cast<void*>(globalHost()), decodePointerCell());
	EXPECT_EQ(reinterpret_cast<void*>(globalHost()),
			__retdec_pe32_guest_to_host(0x401000));
	EXPECT_EQ(42, callMappedFunction(35));

	std::uintptr_t stackHost = 0;
	EXPECT_TRUE(stackRoundTrip(&stackHost));
	EXPECT_GT(stackHost, UINT32_MAX);

	EXPECT_EQ(0, dlclose(module));
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
