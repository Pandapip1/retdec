/**
 * @file src/bin2llvmir/optimizations/writer_analysis_metadata/writer_analysis_metadata.cpp
 * @brief Versioned machine-readable binary analysis metadata writer.
 */

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
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
#include "retdec/fileformat/types/export_table/export_table.h"
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

using BlockInstructions = std::unordered_map<
		llvm::BasicBlock*, std::vector<const cs_insn*>>;

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

struct ImportThunk
{
	std::uint64_t address = 0;
	std::uint64_t iatAddress = 0;
	std::string library;
	std::string name;
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

std::uint64_t recoveredBlockAddress(
		llvm::BasicBlock* block,
		const BlockInstructions& instructions)
{
	const auto named = blockAddress(block);
	if (named != 0)
	{
		return named;
	}
	const auto found = instructions.find(block);
	return found == instructions.end() || found->second.empty()
			? 0 : found->second.front()->address;
}

void collectRecoveredSuccessors(
		llvm::BasicBlock* block,
		const BlockInstructions& instructions,
		std::set<llvm::BasicBlock*>& visited,
		std::set<std::uint64_t>& result)
{
	if (block == nullptr || !visited.insert(block).second)
	{
		return;
	}
	const auto recovered = recoveredBlockAddress(block, instructions);
	if (recovered != 0)
	{
		result.insert(recovered);
		return;
	}
	for (auto successor = llvm::succ_begin(block);
			successor != llvm::succ_end(block); ++successor)
	{
		collectRecoveredSuccessors(*successor, instructions, visited, result);
	}
}

std::set<std::uint64_t> recoveredSuccessors(
		llvm::BasicBlock& block,
		const BlockInstructions& instructions)
{
	std::set<std::uint64_t> result;
	for (auto successor = llvm::succ_begin(&block);
			successor != llvm::succ_end(&block); ++successor)
	{
		std::set<llvm::BasicBlock*> visited;
		collectRecoveredSuccessors(*successor, instructions, visited, result);
	}
	return result;
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

void writeExports(Writer& writer, const fileformat::FileFormat& format)
{
	key(writer, "exports");
	writer.StartArray();
	std::uint64_t imageBase = 0;
	format.getImageBaseAddress(imageBase);
	if (const auto* exports = format.getExportTable())
	{
		for (const auto& item : *exports)
		{
			const auto va = item.getAddress();
			std::uint64_t ordinal = 0;
			writer.StartObject();
			key(writer, "name"); writer.String(item.getName().c_str());
			key(writer, "ordinal");
			if (item.getOrdinalNumber(ordinal))
			{
				writer.Uint64(ordinal);
			}
			else
			{
				writer.Null();
			}
			key(writer, "rva");
			writer.Uint64(va >= imageBase ? va - imageBase : va);
			key(writer, "va"); address(writer, va);
			writer.EndObject();
		}
	}
	writer.EndArray();
}

std::vector<ImportThunk> collectImportThunks(
		const std::vector<DecodedInstruction>& instructions,
		const fileformat::FileFormat& format)
{
	std::vector<ImportThunk> result;
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
			result.push_back({
					instruction.address,
					iatAddress,
					imports->getLibrary(imported->getLibraryIndex()),
					imported->getName()});
		}
	}
	return result;
}

void writeImportThunks(
		Writer& writer,
		const std::vector<ImportThunk>& thunks)
{
	key(writer, "import_thunks");
	writer.StartArray();
	for (const auto& thunk : thunks)
	{
		writer.StartObject();
		key(writer, "address"); address(writer, thunk.address);
		key(writer, "iat_address"); address(writer, thunk.iatAddress);
		key(writer, "library"); writer.String(thunk.library.c_str());
		key(writer, "name"); writer.String(thunk.name.c_str());
		writer.EndObject();
	}
	writer.EndArray();
}

enum class ValueLocationKind
{
	Register,
	Stack,
	Memory
};

struct ValueLocation
{
	ValueLocationKind kind = ValueLocationKind::Register;
	std::string name;
	std::int64_t displacement = 0;
	std::string index;
	int scale = 1;

	bool operator<(const ValueLocation& other) const
	{
		return std::tie(kind, name, displacement, index, scale)
				< std::tie(
						other.kind, other.name, other.displacement,
						other.index, other.scale);
	}

	bool operator==(const ValueLocation& other) const
	{
		return kind == other.kind && name == other.name
				&& displacement == other.displacement
				&& index == other.index && scale == other.scale;
	}
};

using DefinitionSet = std::set<std::uint64_t>;
using ReachingState = std::map<ValueLocation, DefinitionSet>;

struct ValueFlowInput
{
	std::string role;
	std::uint64_t definition = 0;
};

struct ValueFlowRecord
{
	std::uint64_t address = 0;
	std::string operation;
	ValueLocation destination;
	unsigned int size = 0;
	std::vector<ValueFlowInput> inputs;
	bool importedCallReturn = false;
	bool hasCallTarget = false;
	std::uint64_t callTarget = 0;
	bool hasStoredImmediate = false;
	std::int64_t storedImmediate = 0;
};

struct ValueFlowAmbiguity
{
	std::uint64_t block = 0;
	ValueLocation location;
	DefinitionSet candidates;
};

struct ValueFlowResult
{
	std::map<std::uint64_t, ValueFlowRecord> records;
	std::vector<ValueFlowAmbiguity> ambiguities;
	std::set<std::uint64_t> interesting;
};

bool isOneOf(
		const std::string& value,
		std::initializer_list<const char*> candidates)
{
	return std::any_of(candidates.begin(), candidates.end(),
			[&](const char* candidate) { return value == candidate; });
}

std::string canonicalRegisterName(
		csh handle,
		unsigned int reg,
		unsigned int bits)
{
	auto name = registerName(handle, reg);
	if (isOneOf(name, {"al", "ah", "ax", "eax", "rax"}))
	{
		return bits == 64 ? "rax" : "eax";
	}
	if (isOneOf(name, {"bl", "bh", "bx", "ebx", "rbx"}))
	{
		return bits == 64 ? "rbx" : "ebx";
	}
	if (isOneOf(name, {"cl", "ch", "cx", "ecx", "rcx"}))
	{
		return bits == 64 ? "rcx" : "ecx";
	}
	if (isOneOf(name, {"dl", "dh", "dx", "edx", "rdx"}))
	{
		return bits == 64 ? "rdx" : "edx";
	}
	if (isOneOf(name, {"sil", "si", "esi", "rsi"}))
	{
		return bits == 64 ? "rsi" : "esi";
	}
	if (isOneOf(name, {"dil", "di", "edi", "rdi"}))
	{
		return bits == 64 ? "rdi" : "edi";
	}
	if (isOneOf(name, {"bpl", "bp", "ebp", "rbp"}))
	{
		return bits == 64 ? "rbp" : "ebp";
	}
	if (isOneOf(name, {"spl", "sp", "esp", "rsp"}))
	{
		return bits == 64 ? "rsp" : "esp";
	}
	if (isOneOf(name, {"ip", "eip", "rip"}))
	{
		return bits == 64 ? "rip" : "eip";
	}
	for (unsigned int index = 8; index <= 15; ++index)
	{
		const auto full = "r" + std::to_string(index);
		if (name == full || name == full + "b" || name == full + "w"
				|| name == full + "d")
		{
			return full;
		}
	}
	return name;
}

ValueLocation registerLocation(
		csh handle,
		unsigned int reg,
		unsigned int bits)
{
	return {ValueLocationKind::Register,
			canonicalRegisterName(handle, reg, bits), 0};
}

bool stackLocation(
		csh handle,
		const cs_x86_op& operand,
		unsigned int bits,
		ValueLocation& result)
{
	if (operand.type != X86_OP_MEM || operand.mem.index != X86_REG_INVALID)
	{
		return false;
	}
	const auto base = canonicalRegisterName(handle, operand.mem.base, bits);
	if (!isOneOf(base, {"ebp", "esp", "rbp", "rsp"}))
	{
		return false;
	}
	result = {ValueLocationKind::Stack, base, operand.mem.disp};
	return true;
}

ValueLocation memoryLocation(
		csh handle,
		const cs_x86_op& operand,
		unsigned int bits)
{
	ValueLocation result;
	result.kind = ValueLocationKind::Memory;
	result.name = canonicalRegisterName(handle, operand.mem.base, bits);
	result.displacement = operand.mem.disp;
	result.index = canonicalRegisterName(handle, operand.mem.index, bits);
	result.scale = operand.mem.scale;
	return result;
}

bool primaryWrittenLocation(
		csh handle,
		const cs_insn& instruction,
		unsigned int bits,
		ValueLocation& result,
		unsigned int& size)
{
	if (instruction.detail == nullptr)
	{
		return false;
	}
	for (std::uint8_t i = 0; i < instruction.detail->x86.op_count; ++i)
	{
		const auto& operand = instruction.detail->x86.operands[i];
		if ((operand.access & CS_AC_WRITE) == 0)
		{
			continue;
		}
		if (operand.type == X86_OP_REG)
		{
			result = registerLocation(handle, operand.reg, bits);
			size = operand.size;
			return true;
		}
		if (stackLocation(handle, operand, bits, result))
		{
			size = operand.size;
			return true;
		}
	}
	return false;
}

void eraseStackLocations(ReachingState& state, const std::string& base)
{
	for (auto item = state.begin(); item != state.end();)
	{
		if (item->first.kind == ValueLocationKind::Stack
				&& item->first.name == base)
		{
			item = state.erase(item);
		}
		else
		{
			++item;
		}
	}
}

bool isStackPointerMutation(
		const cs_insn& instruction,
		const ValueLocation* written)
{
	if (written != nullptr && written->kind == ValueLocationKind::Register
			&& isOneOf(written->name, {"esp", "rsp"}))
	{
		return true;
	}
	return instruction.id == X86_INS_PUSH || instruction.id == X86_INS_POP
			|| instruction.id == X86_INS_PUSHF || instruction.id == X86_INS_PUSHFD
			|| instruction.id == X86_INS_PUSHFQ || instruction.id == X86_INS_POPF
			|| instruction.id == X86_INS_POPFD || instruction.id == X86_INS_POPFQ
			|| instruction.id == X86_INS_ENTER || instruction.id == X86_INS_LEAVE;
}

void eraseCallerSavedRegisters(
		ReachingState& state,
		unsigned int bits)
{
	const std::initializer_list<const char*> x86 = {"eax", "ecx", "edx"};
	const std::initializer_list<const char*> x64 = {
			"rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"};
	for (const auto* name : bits == 64 ? x64 : x86)
	{
		state.erase({ValueLocationKind::Register, name, 0});
	}
}

void transferDefinitions(
		ReachingState& state,
		csh handle,
		const cs_insn& instruction,
		unsigned int bits)
{
	if (instruction.id == X86_INS_CALL)
	{
		eraseCallerSavedRegisters(state, bits);
		const auto result = registerLocation(
				handle, bits == 64 ? X86_REG_RAX : X86_REG_EAX, bits);
		state[result] = {instruction.address};
		return;
	}

	ValueLocation written;
	unsigned int size = 0;
	const auto hasWritten = primaryWrittenLocation(
			handle, instruction, bits, written, size);
	if (isStackPointerMutation(instruction, hasWritten ? &written : nullptr))
	{
		eraseStackLocations(state, bits == 64 ? "rsp" : "esp");
	}
	if (hasWritten)
	{
		state[written] = {instruction.address};
	}
}

ReachingState mergedPredecessors(
		llvm::BasicBlock& block,
		const std::map<llvm::BasicBlock*, ReachingState>& outgoing)
{
	std::vector<llvm::BasicBlock*> predecessors(
			llvm::pred_begin(&block), llvm::pred_end(&block));
	if (predecessors.empty())
	{
		return {};
	}
	if (predecessors.size() == 1)
	{
		const auto found = outgoing.find(predecessors.front());
		return found == outgoing.end() ? ReachingState{} : found->second;
	}

	std::set<ValueLocation> locations;
	for (auto* predecessor : predecessors)
	{
		const auto found = outgoing.find(predecessor);
		if (found == outgoing.end())
		{
			continue;
		}
		for (const auto& item : found->second)
		{
			locations.insert(item.first);
		}
	}

	ReachingState result;
	for (const auto& location : locations)
	{
		auto& definitions = result[location];
		for (auto* predecessor : predecessors)
		{
			const auto state = outgoing.find(predecessor);
			if (state == outgoing.end())
			{
				definitions.insert(0);
				continue;
			}
			const auto definition = state->second.find(location);
			if (definition == state->second.end())
			{
				definitions.insert(0);
			}
			else
			{
				definitions.insert(
						definition->second.begin(), definition->second.end());
			}
		}
	}
	return result;
}

bool uniqueDefinition(
		const ReachingState& state,
		const ValueLocation& location,
		std::uint64_t& definition)
{
	const auto found = state.find(location);
	if (found == state.end() || found->second.size() != 1
			|| *found->second.begin() == 0)
	{
		return false;
	}
	definition = *found->second.begin();
	return true;
}

void addInput(
		ValueFlowRecord& record,
		const ReachingState& state,
		const ValueLocation& location,
		const char* role,
		std::vector<ValueFlowAmbiguity>& ambiguities,
		std::uint64_t block)
{
	std::uint64_t definition = 0;
	if (uniqueDefinition(state, location, definition))
	{
		record.inputs.push_back({role, definition});
		return;
	}
	const auto found = state.find(location);
	if (found == state.end() || found->second.empty())
	{
		return;
	}
	const auto duplicate = std::any_of(
			ambiguities.begin(), ambiguities.end(),
			[&](const ValueFlowAmbiguity& ambiguity) {
				return ambiguity.block == block
						&& ambiguity.location == location
						&& ambiguity.candidates == found->second;
			});
	if (!duplicate)
	{
		ambiguities.push_back({block, location, found->second});
	}
}

void addMemoryInputs(
		ValueFlowRecord& record,
		const ReachingState& state,
		csh handle,
		const cs_x86_op& operand,
		unsigned int bits,
		std::vector<ValueFlowAmbiguity>& ambiguities,
		std::uint64_t block)
{
	if (operand.mem.base != X86_REG_INVALID)
	{
		addInput(record, state,
				registerLocation(handle, operand.mem.base, bits), "base",
				ambiguities, block);
	}
	if (operand.mem.index != X86_REG_INVALID)
	{
		addInput(record, state,
				registerLocation(handle, operand.mem.index, bits), "index",
				ambiguities, block);
	}
}

bool directCallTarget(
		const cs_insn& instruction,
		unsigned int bits,
		std::uint64_t& target)
{
	if (instruction.id != X86_INS_CALL || instruction.detail == nullptr
			|| instruction.detail->x86.op_count != 1)
	{
		return false;
	}
	const auto& operand = instruction.detail->x86.operands[0];
	if (operand.type == X86_OP_IMM && operand.imm != 0)
	{
		target = bits == 32
				? static_cast<std::uint32_t>(operand.imm)
				: static_cast<std::uint64_t>(operand.imm);
		return true;
	}
	if (operand.type == X86_OP_MEM
			&& operand.mem.base == X86_REG_INVALID
			&& operand.mem.index == X86_REG_INVALID
			&& operand.mem.disp != 0)
	{
		target = bits == 32
				? static_cast<std::uint32_t>(operand.mem.disp)
				: static_cast<std::uint64_t>(operand.mem.disp);
		return true;
	}
	return false;
}

ValueFlowRecord describeDefinition(
		const ReachingState& state,
		csh handle,
		const cs_insn& instruction,
		unsigned int bits,
		const std::set<std::uint64_t>& importedTargets,
		std::vector<ValueFlowAmbiguity>& ambiguities,
		std::uint64_t block)
{
	ValueFlowRecord record;
	record.address = instruction.address;
	if (instruction.id == X86_INS_CALL)
	{
		record.operation = "call_return";
		record.destination = registerLocation(
				handle, bits == 64 ? X86_REG_RAX : X86_REG_EAX, bits);
		record.size = bits / 8;
		record.hasCallTarget = directCallTarget(
				instruction, bits, record.callTarget);
		record.importedCallReturn = record.hasCallTarget
				&& importedTargets.count(record.callTarget) != 0;
		return record;
	}
	if (instruction.id == X86_INS_MOV && instruction.detail != nullptr
			&& instruction.detail->x86.op_count >= 2
			&& instruction.detail->x86.operands[0].type == X86_OP_MEM)
	{
		const auto& destination = instruction.detail->x86.operands[0];
		ValueLocation stack;
		if (!stackLocation(handle, destination, bits, stack))
		{
			record.operation = "pointer_store";
			record.destination = memoryLocation(
					handle, destination, bits);
			record.size = destination.size;
			addMemoryInputs(record, state, handle, destination, bits,
					ambiguities, block);
			const auto& source = instruction.detail->x86.operands[1];
			if (source.type == X86_OP_REG)
			{
				addInput(record, state,
						registerLocation(handle, source.reg, bits), "value",
						ambiguities, block);
			}
			else if (source.type == X86_OP_IMM)
			{
				record.hasStoredImmediate = true;
				record.storedImmediate = source.imm;
			}
			return record;
		}
	}

	if (!primaryWrittenLocation(
			handle, instruction, bits, record.destination, record.size))
	{
		return record;
	}
	record.operation = "unknown";
	if (instruction.detail == nullptr)
	{
		return record;
	}
	const auto& x86 = instruction.detail->x86;
	const auto isMove = instruction.id == X86_INS_MOV
			|| instruction.id == X86_INS_MOVSX
			|| instruction.id == X86_INS_MOVZX;
	if (isMove && x86.op_count >= 2)
	{
		const auto& source = x86.operands[1];
		if (record.destination.kind == ValueLocationKind::Stack)
		{
			record.operation = "stack_store";
			if (source.type == X86_OP_REG)
			{
				addInput(record, state,
						registerLocation(handle, source.reg, bits), "value",
						ambiguities, block);
			}
			return record;
		}
		if (source.type == X86_OP_REG)
		{
			record.operation = "copy";
			addInput(record, state,
					registerLocation(handle, source.reg, bits), "value",
					ambiguities, block);
		}
		else if (source.type == X86_OP_MEM)
		{
			ValueLocation stack;
			if (stackLocation(handle, source, bits, stack))
			{
				record.operation = "stack_load";
				addInput(record, state, stack, "value", ambiguities, block);
			}
			else
			{
				record.operation = source.mem.index == X86_REG_INVALID
						? "pointer_load" : "indexed_load";
				addMemoryInputs(record, state, handle, source, bits,
						ambiguities, block);
			}
		}
		else if (source.type == X86_OP_IMM)
		{
			record.operation = "constant";
		}
		return record;
	}
	if (instruction.id == X86_INS_LEA && x86.op_count >= 2
			&& x86.operands[1].type == X86_OP_MEM)
	{
		record.operation = "address";
		addMemoryInputs(record, state, handle, x86.operands[1], bits,
				ambiguities, block);
		return record;
	}
	if (instruction.id == X86_INS_XOR && x86.op_count >= 2
			&& x86.operands[0].type == X86_OP_REG
			&& x86.operands[1].type == X86_OP_REG
			&& canonicalRegisterName(handle, x86.operands[0].reg, bits)
					== canonicalRegisterName(handle, x86.operands[1].reg, bits))
	{
		record.operation = "constant";
		return record;
	}

	record.operation = "transform";
	for (std::uint8_t i = 0; i < x86.op_count; ++i)
	{
		const auto& operand = x86.operands[i];
		if ((operand.access & CS_AC_READ) == 0)
		{
			continue;
		}
		if (operand.type == X86_OP_REG)
		{
			addInput(record, state,
					registerLocation(handle, operand.reg, bits),
					i == 0 ? "value" : "operand", ambiguities, block);
		}
		else if (operand.type == X86_OP_MEM)
		{
			addMemoryInputs(record, state, handle, operand, bits,
					ambiguities, block);
		}
	}
	return record;
}

void writeValueLocation(
		Writer& writer,
		const ValueLocation& location,
		unsigned int size = 0)
{
	writer.StartObject();
	key(writer, "kind");
	writer.String(location.kind == ValueLocationKind::Register
			? "register"
			: (location.kind == ValueLocationKind::Stack ? "stack" : "memory"));
	if (location.kind == ValueLocationKind::Register)
	{
		key(writer, "register"); writer.String(location.name.c_str());
	}
	else
	{
		key(writer, "base"); writer.String(location.name.c_str());
		if (location.kind == ValueLocationKind::Memory)
		{
			key(writer, "index"); writer.String(location.index.c_str());
			key(writer, "scale"); writer.Int(location.scale);
		}
		key(writer, "displacement"); writer.Int64(location.displacement);
	}
	if (size != 0)
	{
		key(writer, "size"); writer.Uint(size);
	}
	writer.EndObject();
}

ValueFlowResult analyzeValueFlow(
		llvm::Function& function,
		csh handle,
		unsigned int bits,
		const BlockInstructions& blockInstructions,
		const fileformat::FileFormat& format,
		const std::vector<ImportThunk>& thunks)
{
	std::set<std::uint64_t> importedTargets;
	if (const auto* imports = format.getImportTable())
	{
		for (const auto& imported : *imports)
		{
			if (imported != nullptr)
			{
				importedTargets.insert(imported->getAddress());
			}
		}
	}
	for (const auto& thunk : thunks)
	{
		importedTargets.insert(thunk.address);
	}

	std::map<llvm::BasicBlock*, ReachingState> incoming;
	std::map<llvm::BasicBlock*, ReachingState> outgoing;
	bool changed = true;
	const auto maxIterations = static_cast<std::size_t>(function.size()) * 8 + 8;
	std::size_t iteration = 0;
	while (changed && iteration++ < maxIterations)
	{
		changed = false;
		for (auto& block : function)
		{
			auto state = mergedPredecessors(block, outgoing);
			if (incoming[&block] != state)
			{
				incoming[&block] = state;
				changed = true;
			}
			const auto found = blockInstructions.find(&block);
			if (found != blockInstructions.end())
			{
				for (const auto* instruction : found->second)
				{
					transferDefinitions(state, handle, *instruction, bits);
				}
			}
			if (outgoing[&block] != state)
			{
				outgoing[&block] = std::move(state);
				changed = true;
			}
		}
	}
	if (changed)
	{
		throw std::runtime_error("analysis metadata value-flow did not converge");
	}

	ValueFlowResult result;
	for (auto& block : function)
	{
		auto state = incoming[&block];
		const auto found = blockInstructions.find(&block);
		if (found == blockInstructions.end())
		{
			continue;
		}
		for (const auto* instruction : found->second)
		{
			auto record = describeDefinition(
					state, handle, *instruction, bits, importedTargets,
					result.ambiguities,
					recoveredBlockAddress(&block, blockInstructions));
			if (!record.operation.empty())
			{
				result.records[record.address] = std::move(record);
			}
			transferDefinitions(state, handle, *instruction, bits);
		}
	}

	for (const auto& item : result.records)
	{
		if (item.second.importedCallReturn)
		{
			result.interesting.insert(item.first);
		}
	}
	bool added = true;
	while (added)
	{
		added = false;
		for (const auto& item : result.records)
		{
			if (result.interesting.count(item.first) != 0)
			{
				continue;
			}
			for (const auto& input : item.second.inputs)
			{
				// A store is an observable effect of a derived pointer only
				// when its address, rather than merely its stored value,
				// descends from an imported return.
				if (item.second.operation == "pointer_store"
						&& input.role == "value")
				{
					continue;
				}
				if (result.interesting.count(input.definition) != 0)
				{
					result.interesting.insert(item.first);
					added = true;
					break;
				}
			}
		}
	}

	// Retain the complete, unambiguous provenance of every operand used by
	// the forward slice. This is deliberately a separate backwards-only
	// closure: operand definitions become visible, but do not seed unrelated
	// downstream effects.
	added = true;
	while (added)
	{
		added = false;
		const auto retained = result.interesting;
		for (const auto address : retained)
		{
			const auto found = result.records.find(address);
			if (found == result.records.end())
			{
				continue;
			}
			for (const auto& input : found->second.inputs)
			{
				if (result.records.count(input.definition) != 0
						&& result.interesting.insert(input.definition).second)
				{
					added = true;
				}
			}
		}
	}

	return result;
}

void writeValueFlow(Writer& writer, const ValueFlowResult& flow)
{
	key(writer, "value_flow");
	writer.StartObject();
	key(writer, "definitions");
	writer.StartArray();
	for (const auto& item : flow.records)
	{
		if (flow.interesting.count(item.first) == 0)
		{
			continue;
		}
		const auto& record = item.second;
		writer.StartObject();
		key(writer, "address"); address(writer, record.address);
		key(writer, "operation"); writer.String(record.operation.c_str());
		key(writer, "destination");
		writeValueLocation(writer, record.destination, record.size);
		if (record.hasCallTarget)
		{
			key(writer, "call_target"); address(writer, record.callTarget);
		}
		if (record.hasStoredImmediate)
		{
			key(writer, "stored_immediate");
			writer.Int64(record.storedImmediate);
		}
		key(writer, "inputs");
		writer.StartArray();
		for (const auto& input : record.inputs)
		{
			writer.StartObject();
			key(writer, "role"); writer.String(input.role.c_str());
			key(writer, "definition"); address(writer, input.definition);
			writer.EndObject();
		}
		writer.EndArray();
		writer.EndObject();
	}
	writer.EndArray();
	key(writer, "ambiguous_merges");
	writer.StartArray();
	for (const auto& ambiguity : flow.ambiguities)
	{
		const auto hasInteresting = std::any_of(
				ambiguity.candidates.begin(), ambiguity.candidates.end(),
				[&](std::uint64_t definition) {
					return flow.interesting.count(definition) != 0;
				});
		if (!hasInteresting)
		{
			continue;
		}
		writer.StartObject();
		key(writer, "block"); address(writer, ambiguity.block);
		key(writer, "location"); writeValueLocation(writer, ambiguity.location);
		key(writer, "candidate_definitions");
		writer.StartArray();
		for (const auto definition : ambiguity.candidates)
		{
			if (definition != 0)
			{
				address(writer, definition);
			}
		}
		writer.EndArray();
		key(writer, "includes_undefined");
		writer.Bool(ambiguity.candidates.count(0) != 0);
		writer.EndObject();
	}
	writer.EndArray();
	writer.EndObject();
}

void writeFunctions(
		Writer& writer,
		csh handle,
		llvm::Module& module,
		const std::vector<DecodedInstruction>& instructions,
		const fileformat::FileFormat& format,
		const std::vector<ImportThunk>& thunks,
		unsigned int bits,
		std::set<Reference>& references)
{
	BlockInstructions blockInstructions;
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
			const auto startAddress = recoveredBlockAddress(&block, blockInstructions);
			if (startAddress == 0)
			{
				continue;
			}
			writer.StartObject();
			key(writer, "address"); address(writer, startAddress);
			key(writer, "successors");
			writer.StartArray();
			for (const auto successorAddress :
					recoveredSuccessors(block, blockInstructions))
			{
				address(writer, successorAddress);
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
		writeValueFlow(writer, analyzeValueFlow(
				function, handle, bits, blockInstructions, format, thunks));
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
	writeExports(writer, *format);
	std::set<Reference> references;
	const auto instructions = decodedInstructions(module);
	const auto thunks = collectImportThunks(instructions, *format);
	writeFunctions(writer, handle, module, instructions, *format, thunks,
			config->getConfig().architecture.getBitSize(), references);
	writeReferences(writer, references);
	writeImportThunks(writer, thunks);
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
