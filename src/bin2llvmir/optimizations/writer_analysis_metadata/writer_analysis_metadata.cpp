/**
 * @file src/bin2llvmir/optimizations/writer_analysis_metadata/writer_analysis_metadata.cpp
 * @brief Versioned machine-readable binary analysis metadata writer.
 */

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <capstone/capstone.h>
#include <capstone/x86.h>
#include <llvm/IR/CFG.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "retdec/bin2llvmir/optimizations/writer_analysis_metadata/writer_analysis_metadata.h"
#include "retdec/bin2llvmir/providers/asm_instruction.h"
#include "retdec/bin2llvmir/providers/config.h"
#include "retdec/bin2llvmir/providers/fileimage.h"
#include "retdec/fileformat/file_format/file_format.h"
#include "retdec/fileformat/types/import_table/import.h"
#include "retdec/fileformat/types/import_table/import_table.h"
#include "retdec/fileformat/types/sec_seg/pe_coff_section.h"

namespace retdec {
namespace bin2llvmir {

namespace {

constexpr const char* kSchema = "retdec-analysis-metadata-v1";
constexpr std::uint64_t kPeSectionWritable = 0x80000000ULL;

using Writer = rapidjson::Writer<rapidjson::StringBuffer>;

void key(Writer& writer, const char* value)
{
	writer.Key(value);
}

void address(Writer& writer, std::uint64_t value)
{
	writer.Uint64(value);
}

std::string registerName(csh handle, unsigned int reg)
{
	if (reg == X86_REG_INVALID)
	{
		return {};
	}
	const auto* name = cs_reg_name(handle, reg);
	return name == nullptr ? std::string() : std::string(name);
}

void writeRegister(Writer& writer, const char* name, csh handle, unsigned int reg)
{
	key(writer, name);
	writer.String(registerName(handle, reg).c_str());
}

void writeX86Operand(Writer& writer, csh handle, const cs_x86_op& operand)
{
	writer.StartObject();
	key(writer, "size");
	writer.Uint(operand.size);
	key(writer, "access");
	writer.Uint(operand.access);
	switch (operand.type)
	{
		case X86_OP_REG:
			key(writer, "kind");
			writer.String("register");
			writeRegister(writer, "register", handle, operand.reg);
			break;
		case X86_OP_IMM:
			key(writer, "kind");
			writer.String("immediate");
			key(writer, "value");
			writer.Int64(operand.imm);
			break;
		case X86_OP_MEM:
			key(writer, "kind");
			writer.String("memory");
			writeRegister(writer, "segment", handle, operand.mem.segment);
			writeRegister(writer, "base", handle, operand.mem.base);
			writeRegister(writer, "index", handle, operand.mem.index);
			key(writer, "scale");
			writer.Int(operand.mem.scale);
			key(writer, "displacement");
			writer.Int64(operand.mem.disp);
			break;
		default:
			key(writer, "kind");
			writer.String("invalid");
			break;
	}
	writer.EndObject();
}

struct DecodedInstruction
{
	const cs_insn* instruction = nullptr;
	llvm::BasicBlock* block = nullptr;
};

struct Reference
{
	std::uint64_t source = 0;
	std::uint64_t target = 0;
	std::string kind;

	bool operator<(const Reference& other) const
	{
		return std::tie(source, target, kind)
				< std::tie(other.source, other.target, other.kind);
	}
};

bool sectionContains(
		const fileformat::SecSeg& section,
		std::uint64_t target)
{
	unsigned long long memorySize = 0;
	if (!section.getSizeInMemory(memorySize))
	{
		memorySize = section.getSizeInFile();
	}
	return target >= section.getAddress()
			&& target - section.getAddress() < memorySize;
}

const fileformat::SecSeg* sectionFor(
		const fileformat::FileFormat& format,
		std::uint64_t target)
{
	for (const auto* section : format.getSections())
	{
		if (section != nullptr && sectionContains(*section, target))
		{
			return section;
		}
	}
	return nullptr;
}

void collectReferences(
		const cs_insn& instruction,
		const fileformat::FileFormat& format,
		std::set<Reference>& references)
{
	if (instruction.detail == nullptr)
	{
		return;
	}
	const auto isControlFlow = instruction.id == X86_INS_CALL
			|| instruction.id == X86_INS_JMP
			|| (instruction.id >= X86_INS_JAE && instruction.id <= X86_INS_JS);
	for (std::uint8_t i = 0; i < instruction.detail->x86.op_count; ++i)
	{
		const auto& operand = instruction.detail->x86.operands[i];
		std::uint64_t target = 0;
		if (operand.type == X86_OP_IMM)
		{
			target = static_cast<std::uint64_t>(operand.imm);
		}
		else if (operand.type == X86_OP_MEM
				&& operand.mem.base == X86_REG_INVALID
				&& operand.mem.index == X86_REG_INVALID
				&& operand.mem.disp > 0)
		{
			target = static_cast<std::uint64_t>(operand.mem.disp);
		}
		if (target == 0)
		{
			continue;
		}
		if (format.getImport(target) != nullptr)
		{
			references.insert({instruction.address, target, "import"});
		}
		else if (const auto* section = sectionFor(format, target))
		{
			references.insert({instruction.address, target,
					(isControlFlow || section->isSomeCode()) ? "code" : "data"});
		}
	}
}

std::vector<DecodedInstruction> decodedInstructions(llvm::Module& module)
{
	std::vector<DecodedInstruction> result;
	auto& decoded = AsmInstruction::getLlvmToCapstoneInsnMap(&module);
	result.reserve(decoded.size());
	for (const auto& entry : decoded)
	{
		if (entry.first == nullptr || entry.second == nullptr)
		{
			continue;
		}
		result.push_back({entry.second, entry.first->getParent()});
	}
	std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
		return left.instruction->address < right.instruction->address;
	});
	return result;
}

std::uint64_t blockAddress(llvm::BasicBlock* block)
{
	const auto result = AsmInstruction::getTrueBasicBlockAddress(block);
	return result.isDefined() ? result.getValue() : 0;
}

void writeInstruction(
		Writer& writer,
		csh handle,
		const cs_insn& instruction,
		const fileformat::FileFormat& format,
		std::set<Reference>& references)
{
	writer.StartObject();
	key(writer, "address"); address(writer, instruction.address);
	key(writer, "size"); writer.Uint(instruction.size);
	key(writer, "mnemonic"); writer.String(instruction.mnemonic);
	key(writer, "text"); writer.String(instruction.op_str);
	key(writer, "operands");
	writer.StartArray();
	if (instruction.detail != nullptr)
	{
		for (std::uint8_t i = 0; i < instruction.detail->x86.op_count; ++i)
		{
			writeX86Operand(writer, handle, instruction.detail->x86.operands[i]);
		}
	}
	writer.EndArray();
	writer.EndObject();
	collectReferences(instruction, format, references);
}

void writeSections(Writer& writer, const fileformat::FileFormat& format)
{
	key(writer, "sections");
	writer.StartArray();
	for (const auto* section : format.getSections())
	{
		if (section == nullptr)
		{
			continue;
		}
		unsigned long long memorySize = 0;
		if (!section->getSizeInMemory(memorySize))
		{
			memorySize = section->getSizeInFile();
		}
		std::uint64_t flags = 0;
		if (const auto* pe = dynamic_cast<const fileformat::PeCoffSection*>(section))
		{
			flags = pe->getPeCoffFlags();
		}
		writer.StartObject();
		key(writer, "name"); writer.String(section->getName().c_str());
		key(writer, "address"); address(writer, section->getAddress());
		key(writer, "virtual_size"); writer.Uint64(memorySize);
		key(writer, "file_offset"); writer.Uint64(section->getOffset());
		key(writer, "file_size"); writer.Uint64(section->getSizeInFile());
		key(writer, "flags"); writer.Uint64(flags);
		key(writer, "executable"); writer.Bool(section->isSomeCode());
		key(writer, "writable"); writer.Bool((flags & kPeSectionWritable) != 0);
		writer.EndObject();
	}
	writer.EndArray();
}

void writeImports(Writer& writer, const fileformat::FileFormat& format)
{
	key(writer, "imports");
	writer.StartArray();
	if (const auto* imports = format.getImportTable())
	{
		for (const auto& item : *imports)
		{
			if (item == nullptr)
			{
				continue;
			}
			writer.StartObject();
			key(writer, "address"); address(writer, item->getAddress());
			key(writer, "name"); writer.String(item->getName().c_str());
			key(writer, "library");
			writer.String(imports->getLibrary(item->getLibraryIndex()).c_str());
			writer.EndObject();
		}
	}
	writer.EndArray();
}

void writeImportThunks(
		Writer& writer,
		const std::vector<DecodedInstruction>& instructions,
		const fileformat::FileFormat& format)
{
	key(writer, "import_thunks");
	writer.StartArray();
	const auto* imports = format.getImportTable();
	if (imports != nullptr)
	{
		for (const auto& decoded : instructions)
		{
			const auto& instruction = *decoded.instruction;
			if (instruction.id != X86_INS_JMP || instruction.detail == nullptr
					|| instruction.detail->x86.op_count != 1)
			{
				continue;
			}
			const auto& operand = instruction.detail->x86.operands[0];
			if (operand.type != X86_OP_MEM
					|| operand.mem.base != X86_REG_INVALID
					|| operand.mem.index != X86_REG_INVALID
					|| operand.mem.disp <= 0)
			{
				continue;
			}
			const auto iatAddress = static_cast<std::uint64_t>(operand.mem.disp);
			const auto* imported = format.getImport(iatAddress);
			if (imported == nullptr)
			{
				continue;
			}
			writer.StartObject();
			key(writer, "address"); address(writer, instruction.address);
			key(writer, "iat_address"); address(writer, iatAddress);
			key(writer, "library");
			writer.String(imports->getLibrary(imported->getLibraryIndex()).c_str());
			key(writer, "name"); writer.String(imported->getName().c_str());
			writer.EndObject();
		}
	}
	writer.EndArray();
}

void writeFunctions(
		Writer& writer,
		csh handle,
		llvm::Module& module,
		const std::vector<DecodedInstruction>& instructions,
		const fileformat::FileFormat& format,
		std::set<Reference>& references)
{
	std::unordered_map<llvm::BasicBlock*, std::vector<const cs_insn*>> blockInstructions;
	blockInstructions.reserve(instructions.size());
	for (const auto& decoded : instructions)
	{
		blockInstructions[decoded.block].push_back(decoded.instruction);
	}

	key(writer, "functions");
	writer.StartArray();
	for (auto& function : module.functions())
	{
		auto start = AsmInstruction::getFunctionAddress(&function);
		if (start.isUndefined())
		{
			continue;
		}
		auto end = AsmInstruction::getFunctionEndAddress(&function);
		writer.StartObject();
		key(writer, "name"); writer.String(function.getName().str().c_str());
		key(writer, "address"); address(writer, start.getValue());
		key(writer, "end"); address(writer, end.isDefined() ? end.getValue() : start.getValue());
		key(writer, "basic_blocks");
		writer.StartArray();
		for (auto& block : function)
		{
			const auto startAddress = blockAddress(&block);
			if (startAddress == 0)
			{
				continue;
			}
			writer.StartObject();
			key(writer, "address"); address(writer, startAddress);
			key(writer, "successors");
			writer.StartArray();
			for (auto successor = llvm::succ_begin(&block);
					successor != llvm::succ_end(&block); ++successor)
			{
				const auto successorAddress = blockAddress(*successor);
				if (successorAddress != 0)
				{
					address(writer, successorAddress);
				}
			}
			writer.EndArray();
			key(writer, "instructions");
			writer.StartArray();
			const auto recovered = blockInstructions.find(&block);
			if (recovered != blockInstructions.end())
			{
				for (const auto* instruction : recovered->second)
				{
					writeInstruction(writer, handle, *instruction, format, references);
				}
			}
			writer.EndArray();
			writer.EndObject();
		}
		writer.EndArray();
		writer.EndObject();
	}
	writer.EndArray();
}

void writeReferences(Writer& writer, const std::set<Reference>& references)
{
	key(writer, "references");
	writer.StartArray();
	for (const auto& reference : references)
	{
		writer.StartObject();
		key(writer, "source"); address(writer, reference.source);
		key(writer, "target"); address(writer, reference.target);
		key(writer, "kind"); writer.String(reference.kind.c_str());
		writer.EndObject();
	}
	writer.EndArray();
}

} // anonymous namespace

char AnalysisMetadataWriter::ID = 0;

static llvm::RegisterPass<AnalysisMetadataWriter> X(
		"retdec-write-analysis-metadata",
		"Machine-readable binary analysis metadata generation",
		false,
		true);

AnalysisMetadataWriter::AnalysisMetadataWriter() : llvm::ModulePass(ID) {}

bool AnalysisMetadataWriter::runOnModule(llvm::Module& module)
{
	auto* config = ConfigProvider::getConfig(&module);
	auto* image = FileImageProvider::getFileImage(&module);
	if (config == nullptr || image == nullptr)
	{
		return false;
	}
	const auto& output = config->getConfig().parameters.getOutputAnalysisMetadataFile();
	if (output.empty())
	{
		return false;
	}
	const auto* format = image->getFileFormat();
	if (format == nullptr || !config->getConfig().architecture.isX86())
	{
		throw std::runtime_error("analysis metadata currently requires an x86 input image");
	}

	csh handle = 0;
	const auto mode = config->getConfig().architecture.getBitSize() == 64
			? CS_MODE_64 : CS_MODE_32;
	if (cs_open(CS_ARCH_X86, mode, &handle) != CS_ERR_OK)
	{
		throw std::runtime_error("could not initialize analysis metadata register naming");
	}

	rapidjson::StringBuffer buffer;
	Writer writer(buffer);
	writer.StartObject();
	key(writer, "schema"); writer.String(kSchema);
	key(writer, "input");
	writer.StartObject();
	key(writer, "path"); writer.String(config->getConfig().parameters.getInputFile().c_str());
	key(writer, "size"); writer.Uint64(format->getFileLength());
	key(writer, "sha256"); writer.String(format->getSha256().c_str());
	key(writer, "architecture"); writer.String(config->getConfig().architecture.getName().c_str());
	key(writer, "bits"); writer.Uint64(config->getConfig().architecture.getBitSize());
	std::uint64_t imageBase = 0;
	format->getImageBaseAddress(imageBase);
	key(writer, "image_base"); address(writer, imageBase);
	writer.EndObject();
	writeSections(writer, *format);
	writeImports(writer, *format);
	std::set<Reference> references;
	const auto instructions = decodedInstructions(module);
	writeFunctions(writer, handle, module, instructions, *format, references);
	writeReferences(writer, references);
	writeImportThunks(writer, instructions, *format);
	writer.EndObject();
	cs_close(&handle);

	std::ofstream stream(output, std::ios::binary | std::ios::trunc);
	if (!stream)
	{
		throw std::runtime_error("could not open analysis metadata output: " + output);
	}
	stream.write(buffer.GetString(), static_cast<std::streamsize>(buffer.GetSize()));
	stream << '\n';
	if (!stream)
	{
		throw std::runtime_error("could not write analysis metadata output: " + output);
	}
	return false;
}

} // namespace bin2llvmir
} // namespace retdec
