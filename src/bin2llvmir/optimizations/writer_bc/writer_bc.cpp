/**
 * @file src/bin2llvmir/optimizations/writer_bc/writer_bc.cpp
 * @brief Generate the current bitcode.
 * @copyright (c) 2020 Avast Software, licensed under the MIT license
 */

#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/ToolOutputFile.h>

#include "retdec/bin2llvmir/optimizations/writer_bc/writer_bc.h"
#include "retdec/bin2llvmir/providers/config.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

namespace {

constexpr unsigned kSyntheticSourceLine = 1;
constexpr unsigned kSyntheticSourceColumn = 0;
constexpr unsigned kDwarfVersion = 4;
constexpr const char* kDwarfVersionModuleFlag = "Dwarf Version";
constexpr const char* kDebugInfoVersionModuleFlag = "Debug Info Version";
constexpr const char* kSyntheticDebugProducer = "RetDec lifted binary";

bool attachLiftedDwarf(Module& module, Config& config)
{
	if (!config.getConfig().parameters.isEmitLiftedDwarf())
	{
		return false;
	}

	auto inputPath = config.getConfig().parameters.getInputFile();
	StringRef input(inputPath);
	StringRef fileName = sys::path::filename(input);
	StringRef directory = sys::path::parent_path(input);
	if (fileName.empty())
	{
		fileName = "retdec-lifted-binary";
	}

	DIBuilder builder(module);
	auto* file = builder.createFile(fileName, directory);
	builder.createCompileUnit(
			dwarf::DW_LANG_C,
			file,
			kSyntheticDebugProducer,
			true,
			StringRef(),
			0);
	auto* functionType = builder.createSubroutineType(
			builder.getOrCreateTypeArray(None));
	bool changed = false;
	for (auto& function : module)
	{
		if (function.isDeclaration() || function.isIntrinsic()
				|| function.getSubprogram() != nullptr
				|| config.getFunctionAddress(&function).isUndefined())
		{
			continue;
		}

		auto* subprogram = builder.createFunction(
				file,
				function.getName(),
				function.getName(),
				file,
				kSyntheticSourceLine,
				functionType,
				kSyntheticSourceLine,
				DINode::FlagPrototyped | DINode::FlagArtificial,
				DISubprogram::SPFlagDefinition | DISubprogram::SPFlagOptimized);
		function.setSubprogram(subprogram);
		for (auto& block : function)
		{
			for (auto& instruction : block)
			{
				if (instruction.getDebugLoc())
				{
					continue;
				}
				instruction.setDebugLoc(DILocation::get(
						module.getContext(),
						kSyntheticSourceLine,
						kSyntheticSourceColumn,
						subprogram));
			}
		}
		changed = true;
	}
	builder.finalize();

	if (changed && module.getModuleFlag(kDwarfVersionModuleFlag) == nullptr)
	{
		module.addModuleFlag(
				Module::Warning, kDwarfVersionModuleFlag, kDwarfVersion);
	}
	if (changed && module.getModuleFlag(kDebugInfoVersionModuleFlag) == nullptr)
	{
		module.addModuleFlag(
				Module::Warning,
				kDebugInfoVersionModuleFlag,
				DEBUG_METADATA_VERSION);
	}
	return changed;
}

} // anonymous namespace

char BitcodeWriter::ID = 0;

static RegisterPass<BitcodeWriter> X(
		"retdec-write-bc",
		"Generate the current bitcode",
		 false, // Only looks at CFG
		 false // Analysis Pass
);

BitcodeWriter::BitcodeWriter() :
		ModulePass(ID)
{

}

/**
 * Create bitcode output file object.
 */
std::unique_ptr<ToolOutputFile> createBitcodeOutputFile(
		const std::string& outputFile)
{
	std::unique_ptr<ToolOutputFile> Out;

	if (outputFile.empty())
	{
		throw std::runtime_error("bitcode output file was not specified");
	}

	std::error_code EC;
	Out.reset(new ToolOutputFile(outputFile, EC, sys::fs::F_None));
	if (EC)
	{
		throw std::runtime_error(
			"failed to create llvm::ToolOutputFile for .bc: " + EC.message()
		);
	}

	return Out;
}

bool BitcodeWriter::runOnModule(Module& M)
{
	auto* c = ConfigProvider::getConfig(&M);

	auto out = c->getConfig().parameters.getOutputBitcodeFile();
	if (out.empty())
	{
		return false;
	}

	const auto changed = attachLiftedDwarf(M, *c);

	std::unique_ptr<ToolOutputFile> bcOut = createBitcodeOutputFile(out);
	raw_ostream* bcOs = &bcOut->os();
	bool ShouldPreserveUseListOrder = true;
	WriteBitcodeToFile(M, *bcOs, ShouldPreserveUseListOrder);
	bcOut->keep();

	return changed;
}

} // namespace bin2llvmir
} // namespace retdec
