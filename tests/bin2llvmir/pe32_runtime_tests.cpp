/**
 * @file tests/bin2llvmir/pe32_runtime_tests.cpp
 * @brief Tests for the PE32 native-retarget pointer runtime.
 * @copyright (c) 2026 Avast Software, licensed under the MIT license
 */

#include <array>
#include <cstdint>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

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

TEST(Pe32RuntimeTests, TranslatesObjectsAndFunctionsInsideRegisteredSections)
{
	std::array<uint8_t, 128> section{};
	constexpr uint32_t guestBase = 0x51000000u;
	ASSERT_EQ(guestBase,
			retdec_pe32_register_host_object(
					section.data(), section.size(), guestBase));

	EXPECT_EQ(guestBase + 37,
			__retdec_pe32_host_to_guest(
					section.data() + 37, section.data() + 32, 16));
	EXPECT_EQ(guestBase + 64,
			retdec_pe32_register_host_function(
					section.data() + 64, guestBase + 64));
	EXPECT_EQ(section.data() + 64,
			__retdec_pe32_guest_to_host(guestBase + 64));
	EXPECT_EQ(1, retdec_pe32_unregister_host_object(section.data()));
}

#if defined(__linux__)
TEST(Pe32RuntimeTests,
		AutomaticallyRegistersOneMappingForPointersAndDerivedOffsets)
{
	const long pageSize = sysconf(_SC_PAGESIZE);
	ASSERT_GT(pageSize, 0);
	auto* mapping = static_cast<uint8_t*>(mmap(
			nullptr,
			static_cast<size_t>(pageSize) * 2,
			PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS,
			-1,
			0));
	ASSERT_NE(MAP_FAILED, mapping);
	if (sizeof(uintptr_t) > sizeof(uint32_t)
			&& reinterpret_cast<uintptr_t>(mapping) <= UINT32_MAX)
	{
		munmap(mapping, static_cast<size_t>(pageSize) * 2);
		GTEST_SKIP() << "host did not provide a high virtual address";
	}

	auto* first = mapping + 17;
	auto* second = mapping + pageSize + 29;
	const uint32_t firstGuest = __retdec_pe32_host_to_guest(first, first, 0);
	const uint32_t secondGuest = __retdec_pe32_host_to_guest(second, second, 0);
	ASSERT_NE(0u, firstGuest);
	EXPECT_EQ(firstGuest + static_cast<uint32_t>(second - first), secondGuest);
	EXPECT_EQ(second, __retdec_pe32_guest_to_host(secondGuest));
	EXPECT_EQ(1, retdec_pe32_unregister_host_pointer(second));
	EXPECT_EQ(nullptr, __retdec_pe32_guest_to_host(firstGuest));
	EXPECT_EQ(0, retdec_pe32_unregister_host_pointer(first));
	EXPECT_EQ(0, munmap(mapping, static_cast<size_t>(pageSize) * 2));
}

#if defined(__x86_64__) && defined(MAP_32BIT)
TEST(Pe32RuntimeTests, AutomaticallyRegistersLowHostMapping)
{
	const long pageSize = sysconf(_SC_PAGESIZE);
	ASSERT_GT(pageSize, 0);
	auto* mapping = static_cast<uint8_t*>(mmap(
			nullptr,
			static_cast<size_t>(pageSize),
			PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT,
			-1,
			0));
	if (mapping == MAP_FAILED)
	{
		GTEST_SKIP() << "MAP_32BIT is unavailable";
	}
	ASSERT_LE(reinterpret_cast<uintptr_t>(mapping), UINT32_MAX);
	auto* pointer = mapping + 23;
	const uint32_t guest = __retdec_pe32_host_to_guest(pointer, pointer, 0);
	ASSERT_NE(0u, guest);
	EXPECT_EQ(pointer + 7, __retdec_pe32_guest_to_host(guest + 7));
	EXPECT_EQ(1, retdec_pe32_unregister_host_pointer(pointer));
	EXPECT_EQ(0, munmap(mapping, static_cast<size_t>(pageSize)));
}
#endif

TEST(Pe32RuntimeTests,
		AutomaticMappingFailsClosedOnOverlappingExplicitSubrange)
{
	const long pageSize = sysconf(_SC_PAGESIZE);
	ASSERT_GT(pageSize, 0);
	auto* mapping = static_cast<uint8_t*>(mmap(
			nullptr,
			static_cast<size_t>(pageSize) * 2,
			PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS,
			-1,
			0));
	ASSERT_NE(MAP_FAILED, mapping);
	constexpr uint32_t guestBase = 0x61000000u;
	ASSERT_EQ(guestBase, retdec_pe32_register_host_object(
			mapping + 128, 64, guestBase));
	EXPECT_EQ(guestBase + 12, __retdec_pe32_host_to_guest(
			mapping + 140, mapping + 140, 0));
	EXPECT_EQ(0u, __retdec_pe32_host_to_guest(
			mapping + pageSize, mapping + pageSize, 0));
	EXPECT_EQ(1, retdec_pe32_unregister_host_pointer(mapping + 140));

	const uint32_t automaticGuest = __retdec_pe32_host_to_guest(
			mapping + pageSize, mapping + pageSize, 0);
	ASSERT_NE(0u, automaticGuest);
	EXPECT_EQ(1, retdec_pe32_unregister_host_pointer(mapping + pageSize));
	EXPECT_EQ(0, munmap(mapping, static_cast<size_t>(pageSize) * 2));
}

TEST(Pe32RuntimeTests, AutomaticMappingRejectsNullAndUnmappedPointers)
{
	EXPECT_EQ(0u, __retdec_pe32_host_to_guest(nullptr, nullptr, 0));
	EXPECT_EQ(nullptr, __retdec_pe32_guest_to_host(1));
	const long pageSize = sysconf(_SC_PAGESIZE);
	ASSERT_GT(pageSize, 0);
	auto* mapping = static_cast<uint8_t*>(mmap(
			nullptr,
			static_cast<size_t>(pageSize),
			PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS,
			-1,
			0));
	ASSERT_NE(MAP_FAILED, mapping);
	ASSERT_EQ(0, munmap(mapping, static_cast<size_t>(pageSize)));
	EXPECT_EQ(0u, __retdec_pe32_host_to_guest(mapping, mapping, 0));
}
#endif

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
