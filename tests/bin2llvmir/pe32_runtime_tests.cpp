/**
 * @file tests/bin2llvmir/pe32_runtime_tests.cpp
 * @brief Tests for the PE32 native-retarget pointer runtime.
 * @copyright (c) 2026 Avast Software, licensed under the MIT license
 */

#include <array>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "retdec/bin2llvmir/pe32_runtime.h"

namespace retdec {
namespace bin2llvmir {
namespace tests {

namespace {

void sampleFunction()
{
}

} // anonymous namespace

TEST(Pe32RuntimeTests, RoundTripsHighHostObjectAndInteriorPointers)
{
	if (sizeof(uintptr_t) <= sizeof(uint32_t))
	{
		GTEST_SKIP() << "requires a host address wider than PE32";
	}
	std::array<uint8_t, 32> object{};
	ASSERT_GT(reinterpret_cast<uintptr_t>(object.data()), UINT32_MAX);

	const auto guest = retdec_pe32_register_host_object(
			object.data(), object.size(), 0);
	ASSERT_NE(0u, guest);
	EXPECT_EQ(object.data() + 11,
			__retdec_pe32_guest_to_host(guest + 11));
	EXPECT_EQ(guest + 11,
			__retdec_pe32_host_to_guest(
					object.data() + 11, object.data(), object.size()));
	EXPECT_EQ(1, retdec_pe32_unregister_host_object(object.data()));
	EXPECT_EQ(nullptr, __retdec_pe32_guest_to_host(guest));
}

TEST(Pe32RuntimeTests, RegistersExactGuestFunctionAddress)
{
	if (sizeof(uintptr_t) <= sizeof(uint32_t))
	{
		GTEST_SKIP() << "requires a host address wider than PE32";
	}
	constexpr uint32_t guestAddress = 0x5218f270u;
	auto* function = reinterpret_cast<void*>(&sampleFunction);
	ASSERT_GT(reinterpret_cast<uintptr_t>(function), UINT32_MAX);

	EXPECT_EQ(guestAddress,
			retdec_pe32_register_host_function(function, guestAddress));
	EXPECT_EQ(function, __retdec_pe32_guest_to_host(guestAddress));
	EXPECT_EQ(1, retdec_pe32_unregister_host_object(function));
}

TEST(Pe32RuntimeTests, ConcurrentRegistrationReturnsOneStableGuestAddress)
{
	std::array<uint8_t, 64> object{};
	std::array<uint32_t, 32> results{};
	std::vector<std::thread> threads;
	for (size_t i = 0; i < results.size(); ++i)
	{
		threads.emplace_back([&, i]() {
			results[i] = __retdec_pe32_host_to_guest(
					object.data() + i, object.data(), object.size());
		});
	}
	for (auto& thread : threads)
	{
		thread.join();
	}
	for (size_t i = 1; i < results.size(); ++i)
	{
		EXPECT_EQ(results[0] + i, results[i]);
	}
	EXPECT_EQ(1, retdec_pe32_unregister_host_object(object.data()));
}

TEST(Pe32RuntimeTests, RejectsOverlappingExactGuestRegions)
{
	std::array<uint8_t, 8> first{};
	std::array<uint8_t, 8> second{};
	constexpr uint32_t guestAddress = 0x62001000u;
	ASSERT_EQ(guestAddress,
			retdec_pe32_register_host_object(
					first.data(), first.size(), guestAddress));
	EXPECT_EQ(0u,
			retdec_pe32_register_host_object(
					second.data(), second.size(), guestAddress + 4));
	EXPECT_EQ(1, retdec_pe32_unregister_host_object(first.data()));
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
