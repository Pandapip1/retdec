/**
 * @file tests/bin2llvmir/pe32_runtime_pic_fixture.cpp
 * @brief High-address PIC fixture for the PE32 pointer mapping runtime.
 * @copyright (c) 2026 Avast Software, licensed under the MIT license
 */

#include <cstdint>

#include "retdec/bin2llvmir/pe32_runtime.h"

namespace {

std::uint32_t pointerCell = 0;
std::uint32_t globalObject = 0x5a17c0de;
std::uint32_t globalGuest = 0;
std::uint32_t functionGuest = 0;

int mappedFunction(int value)
{
	return value + 7;
}

__attribute__((constructor)) void initializeMappings()
{
	globalGuest = retdec_pe32_register_host_object(
			&globalObject, sizeof(globalObject), 0x401000);
	functionGuest = retdec_pe32_register_host_function(
			reinterpret_cast<void*>(&mappedFunction), 0x402000);
	pointerCell = __retdec_pe32_host_to_guest(
			&globalObject, &globalObject, sizeof(globalObject));
}

} // anonymous namespace

extern "C" std::uintptr_t pe32_pic_global_host()
{
	return reinterpret_cast<std::uintptr_t>(&globalObject);
}

extern "C" std::uintptr_t pe32_pic_function_host()
{
	return reinterpret_cast<std::uintptr_t>(&mappedFunction);
}

extern "C" std::uint32_t pe32_pic_global_guest()
{
	return globalGuest;
}

extern "C" std::uint32_t pe32_pic_function_guest()
{
	return functionGuest;
}

extern "C" std::uint32_t pe32_pic_pointer_cell()
{
	static_assert(sizeof(pointerCell) == 4, "PE32 pointer cells are four bytes");
	return pointerCell;
}

extern "C" void* pe32_pic_decode_pointer_cell()
{
	return __retdec_pe32_guest_to_host(pointerCell);
}

extern "C" int pe32_pic_call_mapped_function(int value)
{
	using Function = int (*)(int);
	auto* pointer = __retdec_pe32_guest_to_host(functionGuest);
	return reinterpret_cast<Function>(pointer)(value);
}

extern "C" int pe32_pic_stack_round_trip(std::uintptr_t* hostAddress)
{
	std::uint32_t local = 0x73;
	*hostAddress = reinterpret_cast<std::uintptr_t>(&local);
	const auto guest = __retdec_pe32_host_to_guest(
			&local, &local, sizeof(local));
	const bool matches = __retdec_pe32_guest_to_host(guest) == &local;
	const bool removed = retdec_pe32_unregister_host_object(&local) != 0;
	return matches && removed;
}
