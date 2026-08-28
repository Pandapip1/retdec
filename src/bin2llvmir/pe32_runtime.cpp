/**
 * @file src/bin2llvmir/pe32_runtime.cpp
 * @brief Runtime address translation for PE32 native retargeting.
 * @copyright (c) 2026 Avast Software, licensed under the MIT license
 */

#include <algorithm>
#include <cstdio>
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
	bool fixed;
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

bool discoverHostMapping(
		uintptr_t address,
		uintptr_t& mappingBase,
		uint32_t& mappingExtent)
{
#if defined(__linux__)
	auto* mappings = std::fopen("/proc/self/maps", "r");
	if (mappings == nullptr)
	{
		return false;
	}
	char line[512];
	bool found = false;
	while (std::fgets(line, sizeof(line), mappings) != nullptr)
	{
		unsigned long long start = 0;
		unsigned long long end = 0;
		if (std::sscanf(line, "%llx-%llx", &start, &end) != 2
				|| address < start || address >= end)
		{
			continue;
		}
		const uint64_t extent = end - start;
		if (extent != 0 && extent <= std::numeric_limits<uint32_t>::max())
		{
			mappingBase = static_cast<uintptr_t>(start);
			mappingExtent = static_cast<uint32_t>(extent);
			found = true;
		}
		break;
	}
	std::fclose(mappings);
	return found;
#else
	(void)address;
	(void)mappingBase;
	(void)mappingExtent;
	return false;
#endif
}

template<typename T>
bool rangesOverlap(T firstBase, uint32_t firstSize, T secondBase, uint32_t secondSize)
{
	return firstBase <= secondBase + (secondSize - 1u)
			&& secondBase <= firstBase + (firstSize - 1u);
}

template<typename T>
bool rangeContains(T outerBase, uint32_t outerSize, T innerBase, uint32_t innerSize)
{
	return innerBase >= outerBase
			&& innerBase - outerBase < outerSize
			&& innerSize <= outerSize - (innerBase - outerBase);
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

bool mappingsHaveSameOffset(
		uintptr_t firstHost,
		uint32_t firstGuest,
		uintptr_t secondHost,
		uint32_t secondGuest)
{
	if (firstHost >= secondHost)
	{
		const uintptr_t hostDifference = firstHost - secondHost;
		return hostDifference <= std::numeric_limits<uint32_t>::max()
				&& firstGuest >= secondGuest
				&& firstGuest - secondGuest == hostDifference;
	}
	const uintptr_t hostDifference = secondHost - firstHost;
	return hostDifference <= std::numeric_limits<uint32_t>::max()
			&& secondGuest >= firstGuest
			&& secondGuest - firstGuest == hostDifference;
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
		uint32_t preferredGuestBase,
		bool allowFixedOverlap = false)
{
	if (!hostRangeValid(hostBase, extent))
	{
		return 0;
	}

	std::lock_guard<std::mutex> lock(regionsMutex);
	const auto registerFixedRegion = [&](uint32_t fixedGuestBase) -> uint32_t
	{
		if (!guestRangeValid(fixedGuestBase, extent))
		{
			return 0;
		}

		Region merged{hostBase, fixedGuestBase, extent, true};
		std::vector<bool> mergedRegions(regions.size(), false);
		bool foundOverlap = false;
		do
		{
			foundOverlap = false;
			for (size_t i = 0; i < regions.size(); ++i)
			{
				const auto& region = regions[i];
				if (mergedRegions[i]
						|| (!rangesOverlap(
								merged.hostBase, merged.extent,
								region.hostBase, region.extent)
							&& !rangesOverlap(
								merged.guestBase, merged.extent,
								region.guestBase, region.extent)))
				{
					continue;
				}
				if (!mappingsHaveSameOffset(
						merged.hostBase, merged.guestBase,
						region.hostBase, region.guestBase)
						|| !region.fixed)
				{
					return 0;
				}

				const uintptr_t mergedHostLast = merged.hostBase
						+ (merged.extent - 1u);
				const uintptr_t regionHostLast = region.hostBase
						+ (region.extent - 1u);
				const uintptr_t unionHostBase = std::min(
						merged.hostBase, region.hostBase);
				const uintptr_t unionHostLast = std::max(
						mergedHostLast, regionHostLast);
				if (unionHostLast - unionHostBase
						>= std::numeric_limits<uint32_t>::max())
				{
					return 0;
				}
				const uint32_t unionExtent = static_cast<uint32_t>(
						unionHostLast - unionHostBase + 1u);
				const uint32_t unionGuestBase = std::min(
						merged.guestBase, region.guestBase);
				if (!hostRangeValid(unionHostBase, unionExtent)
						|| !guestRangeValid(unionGuestBase, unionExtent))
				{
					return 0;
				}
				merged = {unionHostBase, unionGuestBase, unionExtent, true};
				mergedRegions[i] = true;
				foundOverlap = true;
			}
		} while (foundOverlap);

		std::vector<Region> retainedRegions;
		retainedRegions.reserve(regions.size() + 1u);
		for (size_t i = 0; i < regions.size(); ++i)
		{
			if (!mergedRegions[i])
			{
				retainedRegions.push_back(regions[i]);
			}
		}
		regions.swap(retainedRegions);
		regions.push_back(merged);
		return fixedGuestBase;
	};
	if (preferredGuestBase != 0)
	{
		return registerFixedRegion(preferredGuestBase);
	}

	for (auto& region : regions)
	{
		if (region.hostBase == hostBase)
		{
			if (extent > region.extent)
			{
				if (region.fixed)
				{
					return registerFixedRegion(region.guestBase);
				}
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
			return containedGuest;
		}
		if (rangesOverlap(hostBase, extent, region.hostBase, region.extent))
		{
			uint32_t derivedGuestBase = 0;
			if (hostBase < region.hostBase)
			{
				const uintptr_t difference = region.hostBase - hostBase;
				if (difference >= region.guestBase)
				{
					return 0;
				}
				derivedGuestBase = region.guestBase
						- static_cast<uint32_t>(difference);
			}
			else
			{
				const uintptr_t difference = hostBase - region.hostBase;
				if (difference > std::numeric_limits<uint32_t>::max()
						|| region.guestBase
								> std::numeric_limits<uint32_t>::max()
										- difference)
				{
					return 0;
				}
				derivedGuestBase = region.guestBase
						+ static_cast<uint32_t>(difference);
			}

			if (region.fixed)
			{
				if (!allowFixedOverlap)
				{
					return 0;
				}
				return registerFixedRegion(derivedGuestBase);
			}

			// Generated PE32 stack frames may escape to asynchronous consumers.
			// Preserve their address translation after the producing function
			// returns.  A later stack frame that partially overlaps the retained
			// mapping must extend it with the same host/guest offset, rather than
			// invalidate an outstanding guest token.
			const uintptr_t regionHostLast = region.hostBase
					+ (region.extent - 1u);
			const uintptr_t requestedHostLast = hostBase + (extent - 1u);
			const uintptr_t unionHostBase = std::min(region.hostBase, hostBase);
			const uintptr_t unionHostLast = std::max(
					regionHostLast, requestedHostLast);
			if (unionHostLast - unionHostBase
					>= std::numeric_limits<uint32_t>::max())
			{
				return 0;
			}
			const uint32_t unionExtent = static_cast<uint32_t>(
					unionHostLast - unionHostBase + 1u);
			const uint32_t unionGuestBase = std::min(
					region.guestBase, derivedGuestBase);
			if (!hostRangeValid(unionHostBase, unionExtent)
					|| !guestRangeValid(unionGuestBase, unionExtent))
			{
				return 0;
			}
			for (const auto& other : regions)
			{
				if (&other == &region)
				{
					continue;
				}
				if (rangesOverlap(
							unionHostBase, unionExtent,
							other.hostBase, other.extent)
						|| rangesOverlap(
							unionGuestBase, unionExtent,
							other.guestBase, other.extent))
				{
					return 0;
				}
			}
			region = {unionHostBase, unionGuestBase, unionExtent, false};
			return derivedGuestBase;
		}
	}

	uint32_t guestBase = allocateGuestBase(extent);
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
	regions.push_back({hostBase, guestBase, extent, false});
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
	const auto region = std::find_if(
			regions.begin(), regions.end(),
			[address](const Region& item) { return item.hostBase == address; });
	if (region == regions.end())
	{
		return 0;
	}
	regions.erase(region);
	return 1;
}

extern "C" int retdec_pe32_unregister_host_pointer(void* hostPointer)
{
	const auto address = reinterpret_cast<uintptr_t>(hostPointer);
	std::lock_guard<std::mutex> lock(regionsMutex);
	auto region = std::find_if(
			regions.begin(), regions.end(),
			[address](const Region& item) {
				return address >= item.hostBase
						&& address - item.hostBase < item.extent;
			});
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
	const auto requestedBase = reinterpret_cast<uintptr_t>(allocationBase);
	const bool validKnownAllocation = allocationSize != 0
			&& hostRangeValid(requestedBase, allocationSize)
			&& address >= requestedBase
			&& address - requestedBase < allocationSize;
	{
		std::lock_guard<std::mutex> lock(regionsMutex);
		for (const auto& region : regions)
		{
			if (address >= region.hostBase
					&& address - region.hostBase < region.extent)
			{
				// A known allocation may reuse an address previously registered
				// with a smaller extent (notably a native stack frame).  Let
				// registerRegion() grow the exact-base mapping before returning an
				// interior guest pointer.  Unknown or already-contained mappings
				// keep the fast path.
				if (!validKnownAllocation || rangeContains(
						region.hostBase,
						region.extent,
						requestedBase,
						allocationSize))
				{
					return region.guestBase + static_cast<uint32_t>(
							address - region.hostBase);
				}
				break;
			}
		}
	}
	uintptr_t discoveredBase = 0;
	uint32_t discoveredExtent = 0;
	if (allocationSize == 0)
	{
		if (!discoverHostMapping(address, discoveredBase, discoveredExtent))
		{
			return 0;
		}
		allocationBase = reinterpret_cast<void*>(discoveredBase);
		allocationSize = discoveredExtent;
	}
	const auto effectiveBase = reinterpret_cast<uintptr_t>(allocationBase);
	if (!hostRangeValid(effectiveBase, allocationSize)
			|| address < effectiveBase
			|| address - effectiveBase >= allocationSize)
	{
		allocationBase = pointer;
		allocationSize = 1;
	}

	const uint32_t guestBase = registerRegion(
			reinterpret_cast<uintptr_t>(allocationBase), allocationSize, 0,
			validKnownAllocation);
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
