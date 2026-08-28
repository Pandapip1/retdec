/**
 * @file include/retdec/bin2llvmir/pe32_runtime.h
 * @brief Runtime address translation for PE32 code retargeted to a native host.
 * @copyright (c) 2026 Avast Software, licensed under the MIT license
 */

#ifndef RETDEC_BIN2LLVMIR_PE32_RUNTIME_H
#define RETDEC_BIN2LLVMIR_PE32_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register a native object as one contiguous PE32 guest-address region.
 * A zero preferred address asks the runtime to allocate a nonzero guest base.
 * Returns the guest base, or zero on invalid input or an address conflict.
 */
uint32_t retdec_pe32_register_host_object(
		void* host_base,
		uint32_t extent,
		uint32_t preferred_guest_base);

/** Register a native function as a one-byte addressable guest region. */
uint32_t retdec_pe32_register_host_function(
		void* host_function,
		uint32_t preferred_guest_address);

/** Remove the region whose native base exactly matches @p host_base. */
int retdec_pe32_unregister_host_object(void* host_base);

/**
 * Preserve an automatic stack mapping in runtime-owned storage.  Generated
 * code calls this when an escaped native frame returns, so outstanding PE32
 * guest pointers remain valid after the native stack bytes are reused.
 */
int retdec_pe32_retire_stack_object(void* host_base);

/** Remove the region containing @p host_pointer. */
int retdec_pe32_unregister_host_pointer(void* host_pointer);

/**
 * Generated-code hook: translate/register a host pointer as a guest address.
 * A zero allocation_size discovers and registers the containing host virtual
 * memory mapping.  Such an automatic mapping remains valid until explicitly
 * removed with retdec_pe32_unregister_host_pointer() or process termination.
 */
uint32_t __retdec_pe32_host_to_guest(
		void* pointer,
		void* allocation_base,
		uint32_t allocation_size);

/** Generated-code hook: translate a registered guest address to the host. */
void* __retdec_pe32_guest_to_host(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
