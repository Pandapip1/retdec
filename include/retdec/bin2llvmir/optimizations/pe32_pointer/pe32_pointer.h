/**
 * @file include/retdec/bin2llvmir/optimizations/pe32_pointer/pe32_pointer.h
 * @brief Preserve the width of pointer-valued PE32 memory cells.
 * @copyright (c) 2026 Avast Software, licensed under the MIT license
 */

#ifndef RETDEC_BIN2LLVMIR_OPTIMIZATIONS_PE32_POINTER_PE32_POINTER_H
#define RETDEC_BIN2LLVMIR_OPTIMIZATIONS_PE32_POINTER_PE32_POINTER_H

#include <string>
#include <vector>

#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

#include "retdec/bin2llvmir/providers/config.h"

namespace retdec {
namespace bin2llvmir {

/**
 * Pointer types normally inherit their size from the LLVM target.  That is
 * incorrect for a PE32 memory cell after the lifted module is retargeted to a
 * 64-bit host: the cell is still a 32-bit guest pointer.  Represent values in
 * such cells with LLVM's x86 ptr32_uptr address space and cast to the ordinary
 * address space only after loading (and before storing).
 */
class Pe32PointerLegalization : public llvm::ModulePass
{
	public:
		static char ID;
		static constexpr unsigned GuestPointerAddressSpace = 271;

		Pe32PointerLegalization();
		bool runOnModule(llvm::Module& module) override;
		bool runOnModuleCustom(llvm::Module& module, Config* config);

	private:
		bool run();

	private:
		llvm::Module* _module = nullptr;
		Config* _config = nullptr;
};

/**
 * Replace lossy PE32 host/guest pointer casts with an explicit translation
 * ABI.  This pass is intentionally opt-in: native retargeting must provide a
 * process-wide implementation of the two declared bridge functions.
 *
 * __retdec_pe32_host_to_guest(pointer, allocationBase, allocationSize)
 * returns a stable 32-bit guest address for a native pointer.  The base and
 * extent let the runtime preserve pointer arithmetic across an escaped stack
 * or global object.  A zero extent requests discovery of the containing host
 * virtual-memory mapping for a generated native-entry wrapper.
 *
 * __retdec_pe32_guest_to_host(address) performs the inverse translation.
 * Every selected/exported non-variadic guest-ABI function retains its original
 * symbol and receives a native ABI wrapper named
 * <guest-symbol>.retdec_native.  Integer parameters that provably feed guest
 * pointer dereferences become native pointer parameters; all other parameters
 * pass through unchanged.  Return values retain the guest function's original
 * ABI, including guest-encoded integer pointer returns.
 *
 * PE data that remains outside the LLVM module has the same guest ABI.  In
 * particular, a PE32 HIGHLOW cell must retain its original four-byte preferred
 * guest address; it must not become a native ELF relocation against the mapped
 * function or object.  The generated mapping constructor registers the native
 * definitions at those guest addresses.  The module flag named by
 * ExternalPointerEncodingModuleFlag makes this requirement machine-readable
 * to external-image serializers.
 */
class Pe32PointerBridge : public llvm::ModulePass
{
	public:
		static char ID;
		static constexpr const char* ExternalPointerEncodingModuleFlag =
				"retdec.pe32.external-pointers-use-guest-addresses";

		Pe32PointerBridge();
		bool runOnModule(llvm::Module& module) override;
		bool runOnModuleCustom(llvm::Module& module, Config* config);
		static bool enableInPipeline(std::vector<std::string>& passes);

	private:
		bool run();

	private:
		llvm::Module* _module = nullptr;
		Config* _config = nullptr;
};

} // namespace bin2llvmir
} // namespace retdec

#endif
