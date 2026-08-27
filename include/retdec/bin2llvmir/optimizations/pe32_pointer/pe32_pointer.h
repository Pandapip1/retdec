/**
 * @file include/retdec/bin2llvmir/optimizations/pe32_pointer/pe32_pointer.h
 * @brief Preserve the width of pointer-valued PE32 memory cells.
 * @copyright (c) 2026 Avast Software, licensed under the MIT license
 */

#ifndef RETDEC_BIN2LLVMIR_OPTIMIZATIONS_PE32_POINTER_PE32_POINTER_H
#define RETDEC_BIN2LLVMIR_OPTIMIZATIONS_PE32_POINTER_PE32_POINTER_H

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
 * or global object.  Unknown external objects are registered as one-byte
 * exact mappings until a native boundary supplies a wider region.
 *
 * __retdec_pe32_guest_to_host(address) performs the inverse translation.
 */
class Pe32PointerBridge : public llvm::ModulePass
{
	public:
		static char ID;

		Pe32PointerBridge();
		bool runOnModule(llvm::Module& module) override;
		bool runOnModuleCustom(llvm::Module& module, Config* config);

	private:
		bool run();

	private:
		llvm::Module* _module = nullptr;
		Config* _config = nullptr;
};

} // namespace bin2llvmir
} // namespace retdec

#endif
