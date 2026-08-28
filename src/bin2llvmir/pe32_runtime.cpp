/**
 * @file src/bin2llvmir/pe32_runtime.cpp
 * @brief Runtime address translation for PE32 native retargeting.
 * @copyright (c) 2026 Avast Software, licensed under the MIT license
 */

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <vector>

#include "retdec/bin2llvmir/pe32_runtime.h"

namespace {

struct Region
{
	uintptr_t hostBase;
	uint32_t guestBase;
	uint32_t extent;
	bool fixed;
	std::shared_ptr<std::vector<uint8_t>> retiredStorage{};
};

std::mutex regionsMutex;
using RegionList = std::list<Region>;
using RegionIterator = RegionList::iterator;
RegionList regions;
std::map<uintptr_t, RegionIterator> hostRegions;
std::map<uint32_t, RegionIterator> guestRegions;
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

Region* findHostRegion(uintptr_t address)
{
	auto next = hostRegions.upper_bound(address);
	if (next == hostRegions.begin())
	{
		return nullptr;
	}
	const auto region = std::prev(next)->second;
	return address - region->hostBase < region->extent ? &*region : nullptr;
}

Region* findGuestRegion(uint32_t address)
{
	auto next = guestRegions.upper_bound(address);
	if (next == guestRegions.begin())
	{
		return nullptr;
	}
	const auto region = std::prev(next)->second;
	return address - region->guestBase < region->extent ? &*region : nullptr;
}

Region* findHostOverlap(
		uintptr_t base, uint32_t extent, const Region* ignored = nullptr)
{
	auto region = hostRegions.lower_bound(base);
	if (region != hostRegions.begin())
	{
		--region;
	}
	for (; region != hostRegions.end(); ++region)
	{
		auto* candidate = &*region->second;
		if (candidate->hostBase > base + (extent - 1u))
		{
			break;
		}
		if (candidate != ignored && rangesOverlap(
				base, extent, candidate->hostBase, candidate->extent))
		{
			return candidate;
		}
	}
	return nullptr;
}

Region* findGuestOverlap(
		uint32_t base, uint32_t extent, const Region* ignored = nullptr)
{
	auto region = guestRegions.lower_bound(base);
	if (region != guestRegions.begin())
	{
		--region;
	}
	for (; region != guestRegions.end(); ++region)
	{
		auto* candidate = &*region->second;
		if (candidate->guestBase > base + (extent - 1u))
		{
			break;
		}
		if (candidate != ignored && rangesOverlap(
				base, extent, candidate->guestBase, candidate->extent))
		{
			return candidate;
		}
	}
	return nullptr;
}

void collectHostOverlaps(
		uintptr_t base, uint32_t extent, std::set<Region*>& overlaps)
{
	auto region = hostRegions.lower_bound(base);
	if (region != hostRegions.begin())
	{
		--region;
	}
	for (; region != hostRegions.end(); ++region)
	{
		auto* candidate = &*region->second;
		if (candidate->hostBase > base + (extent - 1u))
		{
			break;
		}
		if (rangesOverlap(base, extent, candidate->hostBase, candidate->extent))
		{
			overlaps.insert(candidate);
		}
	}
}

void collectGuestOverlaps(
		uint32_t base, uint32_t extent, std::set<Region*>& overlaps)
{
	auto region = guestRegions.lower_bound(base);
	if (region != guestRegions.begin())
	{
		--region;
	}
	for (; region != guestRegions.end(); ++region)
	{
		auto* candidate = &*region->second;
		if (candidate->guestBase > base + (extent - 1u))
		{
			break;
		}
		if (rangesOverlap(base, extent, candidate->guestBase, candidate->extent))
		{
			overlaps.insert(candidate);
		}
	}
}

Region* addRegion(Region region)
{
	regions.push_back(std::move(region));
	const auto added = std::prev(regions.end());
	hostRegions.emplace(added->hostBase, added);
	guestRegions.emplace(added->guestBase, added);
	return &*added;
}

void eraseRegion(Region* region)
{
	const auto hostEntry = hostRegions.find(region->hostBase);
	if (hostEntry == hostRegions.end())
	{
		return;
	}
	guestRegions.erase(region->guestBase);
	regions.erase(hostEntry->second);
	hostRegions.erase(hostEntry);
}

bool regionCanGrow(const Region& region, uint32_t extent)
{
	if (!hostRangeValid(region.hostBase, extent)
			|| !guestRangeValid(region.guestBase, extent))
	{
		return false;
	}
	return findHostOverlap(region.hostBase, extent, &region) == nullptr
			&& findGuestOverlap(region.guestBase, extent, &region) == nullptr;
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
		auto* conflict = findGuestOverlap(candidate, extent);
		if (conflict == nullptr)
		{
			nextAutomaticGuest = candidate;
			return candidate;
		}
		candidate = conflict->guestBase;
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
		std::set<Region*> mergedRegions;
		bool foundOverlap = false;
		do
		{
			foundOverlap = false;
			std::set<Region*> overlappingRegions;
			collectHostOverlaps(
					merged.hostBase, merged.extent, overlappingRegions);
			collectGuestOverlaps(
					merged.guestBase, merged.extent, overlappingRegions);
			for (auto* region : overlappingRegions)
			{
				if (!mergedRegions.insert(region).second)
				{
					continue;
				}
				if (!mappingsHaveSameOffset(
						merged.hostBase, merged.guestBase,
						region->hostBase, region->guestBase)
						|| !region->fixed)
				{
					return 0;
				}

				const uintptr_t mergedHostLast = merged.hostBase
						+ (merged.extent - 1u);
				const uintptr_t regionHostLast = region->hostBase
						+ (region->extent - 1u);
				const uintptr_t unionHostBase = std::min(
						merged.hostBase, region->hostBase);
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
						merged.guestBase, region->guestBase);
				if (!hostRangeValid(unionHostBase, unionExtent)
						|| !guestRangeValid(unionGuestBase, unionExtent))
				{
					return 0;
				}
				merged = {unionHostBase, unionGuestBase, unionExtent, true};
				foundOverlap = true;
			}
		} while (foundOverlap);

		for (Region* region : mergedRegions)
		{
			eraseRegion(region);
		}
		addRegion(merged);
		return fixedGuestBase;
	};
	if (preferredGuestBase != 0)
	{
		return registerFixedRegion(preferredGuestBase);
	}

	if (auto hostEntry = hostRegions.find(hostBase);
			hostEntry != hostRegions.end())
	{
		auto& region = *hostEntry->second;
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

	if (auto* region = findHostRegion(hostBase);
			region != nullptr && rangeContains(
					region->hostBase, region->extent, hostBase, extent))
	{
		return region->guestBase
				+ static_cast<uint32_t>(hostBase - region->hostBase);
	}

	if (auto* region = findHostOverlap(hostBase, extent))
	{
		if (!allowFixedOverlap || !region->fixed)
		{
			return 0;
		}
		uint32_t derivedGuestBase = 0;
		if (hostBase < region->hostBase)
		{
			const uintptr_t difference = region->hostBase - hostBase;
			if (difference >= region->guestBase)
			{
				return 0;
			}
			derivedGuestBase = region->guestBase
					- static_cast<uint32_t>(difference);
		}
		else
		{
			const uintptr_t difference = hostBase - region->hostBase;
			if (difference > std::numeric_limits<uint32_t>::max()
					|| region->guestBase
							> std::numeric_limits<uint32_t>::max()
									- difference)
			{
				return 0;
			}
			derivedGuestBase = region->guestBase
					+ static_cast<uint32_t>(difference);
		}
		return registerFixedRegion(derivedGuestBase);
	}

	uint32_t guestBase = allocateGuestBase(extent);
	if (!guestRangeValid(guestBase, extent))
	{
		return 0;
	}
	if (findGuestOverlap(guestBase, extent) != nullptr)
	{
		return 0;
	}
	addRegion({hostBase, guestBase, extent, false});
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
	const auto region = hostRegions.find(address);
	if (region == hostRegions.end())
	{
		return 0;
	}
	eraseRegion(&*region->second);
	return 1;
}

extern "C" int retdec_pe32_retire_stack_object(void* hostBase)
{
	const auto address = reinterpret_cast<uintptr_t>(hostBase);
	std::lock_guard<std::mutex> lock(regionsMutex);
	auto hostEntry = hostRegions.find(address);
	if (hostEntry == hostRegions.end() || hostEntry->second->fixed)
	{
		return 0;
	}
	auto region = hostEntry->second;

	std::shared_ptr<std::vector<uint8_t>> storage;
	try
	{
		storage = std::make_shared<std::vector<uint8_t>>(region->extent);
	}
	catch (const std::bad_alloc&)
	{
		return 0;
	}
	std::memcpy(
			storage->data(),
			reinterpret_cast<const void*>(address),
			region->extent);
	hostRegions.erase(hostEntry);
	region->hostBase = reinterpret_cast<uintptr_t>(storage->data());
	region->retiredStorage = std::move(storage);
	hostRegions.emplace(region->hostBase, region);
	return 1;
}

extern "C" int retdec_pe32_unregister_host_pointer(void* hostPointer)
{
	const auto address = reinterpret_cast<uintptr_t>(hostPointer);
	std::lock_guard<std::mutex> lock(regionsMutex);
	auto* region = findHostRegion(address);
	if (region == nullptr)
	{
		return 0;
	}
	eraseRegion(region);
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
		if (auto* region = findHostRegion(address))
		{
			// A known allocation may reuse an address previously registered
			// with a smaller extent (notably a native stack frame).  Let
			// registerRegion() grow the exact-base mapping before returning an
			// interior guest pointer.  Unknown or already-contained mappings
			// keep the fast path.
			if (!validKnownAllocation || rangeContains(
					region->hostBase,
					region->extent,
					requestedBase,
					allocationSize))
			{
				return region->guestBase + static_cast<uint32_t>(
						address - region->hostBase);
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
	auto* region = findGuestRegion(guestAddress);
	if (region != nullptr)
	{
		return reinterpret_cast<void*>(
				region->hostBase + (guestAddress - region->guestBase));
	}
	return nullptr;
}
