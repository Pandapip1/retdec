/**
 * @file src/bin2llvmir/pe32_runtime.cpp
 * @brief Runtime address translation for PE32 native retargeting.
 * @copyright (c) 2026 Avast Software, licensed under the MIT license
 */

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>

#include "retdec/bin2llvmir/pe32_runtime.h"

namespace {

struct Region
{
	uintptr_t hostBase;
	uint32_t guestBase;
	uint32_t extent;
};

std::mutex regionsMutex;
std::vector<Region> regions;
uint32_t nextAutomaticGuest = 0xff000000u;

bool hostRangeValid(uintptr_t base, uint32_t extent)
{
	return base != 0
			&& extent != 0
			&& base <= std::numeric_limits<uintptr_t>::max() - (extent - 1u);
}

bool guestRangeValid(uint32_t base, uint32_t extent)
{
	return base != 0
			&& extent != 0
			&& base <= std::numeric_limits<uint32_t>::max() - (extent - 1u);
}

template<typename T>
bool rangesOverlap(T firstBase, uint32_t firstSize, T secondBase, uint32_t secondSize)
{
	return firstBase <= secondBase + (secondSize - 1u)
			&& secondBase <= firstBase + (firstSize - 1u);
}

bool regionCanGrow(const Region& region, uint32_t extent)
{
	if (!hostRangeValid(region.hostBase, extent)
			|| !guestRangeValid(region.guestBase, extent))
	{
		return false;
	}
	for (const auto& other : regions)
	{
		if (&other == &region)
		{
			continue;
		}
		if (rangesOverlap(
					region.hostBase, extent, other.hostBase, other.extent)
				|| rangesOverlap(
					region.guestBase, extent, other.guestBase, other.extent))
		{
			return false;
		}
	}
	return true;
}

uint32_t allocateGuestBase(uint32_t extent)
{
	const uint64_t wideAlignedExtent = std::max(
			uint64_t{16},
			(static_cast<uint64_t>(extent) + 15u) & ~uint64_t{15u});
	if (wideAlignedExtent > std::numeric_limits<uint32_t>::max())
	{
		return 0;
	}
	const uint32_t alignedExtent = static_cast<uint32_t>(wideAlignedExtent);
	uint32_t candidate = nextAutomaticGuest;
	while (candidate > 0x10000u + alignedExtent)
	{
		candidate -= alignedExtent;
		bool conflict = false;
		for (const auto& region : regions)
		{
			if (rangesOverlap(
						candidate, extent, region.guestBase, region.extent))
			{
				conflict = true;
				candidate = region.guestBase;
				break;
			}
		}
		if (!conflict)
		{
			nextAutomaticGuest = candidate;
			return candidate;
		}
	}
	return 0;
}

uint32_t registerRegion(
		uintptr_t hostBase,
		uint32_t extent,
		uint32_t preferredGuestBase)
{
	if (!hostRangeValid(hostBase, extent))
	{
		return 0;
	}

	std::lock_guard<std::mutex> lock(regionsMutex);
	for (auto& region : regions)
	{
		if (region.hostBase == hostBase)
		{
			if (preferredGuestBase != 0
					&& preferredGuestBase != region.guestBase)
			{
				return 0;
			}
			if (extent > region.extent)
			{
				if (!regionCanGrow(region, extent))
				{
					return 0;
				}
				region.extent = extent;
			}
			return region.guestBase;
		}
		if (hostBase >= region.hostBase
				&& hostBase - region.hostBase < region.extent
				&& extent <= region.extent - (hostBase - region.hostBase))
		{
			const uint32_t containedGuest = region.guestBase
					+ static_cast<uint32_t>(hostBase - region.hostBase);
			return preferredGuestBase == 0
					|| preferredGuestBase == containedGuest
					? containedGuest
					: 0;
		}
		if (rangesOverlap(hostBase, extent, region.hostBase, region.extent))
		{
			return 0;
		}
	}

	uint32_t guestBase = preferredGuestBase;
	if (guestBase == 0)
	{
		guestBase = allocateGuestBase(extent);
	}
	if (!guestRangeValid(guestBase, extent))
	{
		return 0;
	}
	for (const auto& region : regions)
	{
		if (rangesOverlap(guestBase, extent, region.guestBase, region.extent))
		{
			return 0;
		}
	}
	regions.push_back({hostBase, guestBase, extent});
	return guestBase;
}

} // anonymous namespace

extern "C" uint32_t retdec_pe32_register_host_object(
		void* hostBase,
		uint32_t extent,
		uint32_t preferredGuestBase)
{
	return registerRegion(
			reinterpret_cast<uintptr_t>(hostBase), extent, preferredGuestBase);
}

extern "C" uint32_t retdec_pe32_register_host_function(
		void* hostFunction,
		uint32_t preferredGuestAddress)
{
	return retdec_pe32_register_host_object(
			hostFunction, 1, preferredGuestAddress);
}

extern "C" int retdec_pe32_unregister_host_object(void* hostBase)
{
	const auto address = reinterpret_cast<uintptr_t>(hostBase);
	std::lock_guard<std::mutex> lock(regionsMutex);
	auto region = std::find_if(
			regions.begin(), regions.end(),
			[address](const Region& item) { return item.hostBase == address; });
	if (region == regions.end())
	{
		return 0;
	}
	regions.erase(region);
	return 1;
}

extern "C" uint32_t __retdec_pe32_host_to_guest(
		void* pointer,
		void* allocationBase,
		uint32_t allocationSize)
{
	if (pointer == nullptr)
	{
		return 0;
	}
	const auto address = reinterpret_cast<uintptr_t>(pointer);
	{
		std::lock_guard<std::mutex> lock(regionsMutex);
		for (const auto& region : regions)
		{
			if (address >= region.hostBase
					&& address - region.hostBase < region.extent)
			{
				return region.guestBase + static_cast<uint32_t>(
						address - region.hostBase);
			}
		}
	}
	const auto base = reinterpret_cast<uintptr_t>(allocationBase);
	if (!hostRangeValid(base, allocationSize)
			|| address < base
			|| address - base >= allocationSize)
	{
		allocationBase = pointer;
		allocationSize = 1;
	}

	const uint32_t guestBase = retdec_pe32_register_host_object(
			allocationBase, allocationSize, 0);
	if (guestBase == 0)
	{
		return 0;
	}
	return guestBase + static_cast<uint32_t>(
			address - reinterpret_cast<uintptr_t>(allocationBase));
}

extern "C" void* __retdec_pe32_guest_to_host(uint32_t guestAddress)
{
	if (guestAddress == 0)
	{
		return nullptr;
	}
	std::lock_guard<std::mutex> lock(regionsMutex);
	for (const auto& region : regions)
	{
		if (guestAddress >= region.guestBase
				&& guestAddress - region.guestBase < region.extent)
		{
			return reinterpret_cast<void*>(
					region.hostBase + (guestAddress - region.guestBase));
		}
	}
	return nullptr;
}
