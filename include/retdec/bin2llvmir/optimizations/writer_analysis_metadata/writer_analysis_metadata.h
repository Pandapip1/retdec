/**
 * @file include/retdec/bin2llvmir/optimizations/writer_analysis_metadata/writer_analysis_metadata.h
 * @brief Versioned machine-readable binary analysis metadata writer.
 */

#ifndef RETDEC_BIN2LLVMIR_OPTIMIZATIONS_WRITER_ANALYSIS_METADATA_WRITER_ANALYSIS_METADATA_H
#define RETDEC_BIN2LLVMIR_OPTIMIZATIONS_WRITER_ANALYSIS_METADATA_WRITER_ANALYSIS_METADATA_H

#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

namespace retdec {
namespace bin2llvmir {

class AnalysisMetadataWriter : public llvm::ModulePass
{
	public:
		static char ID;
		AnalysisMetadataWriter();
		bool runOnModule(llvm::Module& module) override;
};

} // namespace bin2llvmir
} // namespace retdec

#endif
