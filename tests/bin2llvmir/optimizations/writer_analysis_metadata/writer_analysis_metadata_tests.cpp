/**
 * @file tests/bin2llvmir/optimizations/writer_analysis_metadata/writer_analysis_metadata_tests.cpp
 * @brief Tests for the machine-readable analysis metadata writer.
 */

#include <array>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <capstone/x86.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/FileSystem.h>
#include <rapidjson/document.h>

#include "retdec/bin2llvmir/optimizations/writer_analysis_metadata/writer_analysis_metadata.h"
#include "retdec/bin2llvmir/providers/config.h"
#include "retdec/bin2llvmir/providers/fileimage.h"
#include "retdec/fileformat/file_format/raw_data/raw_data_format.h"
#include "retdec/fileformat/types/export_table/export.h"
#include "retdec/fileformat/types/export_table/export_table.h"
#include "retdec/fileformat/types/import_table/import.h"
#include "retdec/fileformat/types/import_table/import_table.h"
#include "bin2llvmir/utils/llvmir_tests.h"

namespace retdec {
namespace bin2llvmir {
namespace tests {

class AnalysisMetadataWriterTests : public LlvmIrTests
{
	protected:
		class TestFormat : public fileformat::RawDataFormat
		{
			public:
				TestFormat(const std::uint8_t* bytes, std::size_t size)
					:
					RawDataFormat(bytes, size)
				{
				}

				void addImport(std::uint64_t address)
				{
					importTable = new fileformat::ImportTable();
					importTable->addLibrary("fixture.dll");
					auto imported = std::make_unique<fileformat::Import>();
					imported->setAddress(address);
					imported->setLibraryIndex(0);
					imported->setName("fixture_import");
					importTable->addImport(std::move(imported));
				}

				void addExport(
						std::uint64_t address,
						std::uint64_t ordinal,
						const std::string& name)
				{
					exportTable = new fileformat::ExportTable();
					fileformat::Export exported;
					exported.setAddress(address);
					exported.setOrdinalNumber(ordinal);
					exported.setName(name);
					exportTable->addExport(exported);
				}

				bool getImageBaseAddress(std::uint64_t& imageBase) const override
				{
					imageBase = 0x1000;
					return true;
				}
		};

		struct SyntheticInstruction
		{
			cs_insn instruction = {};
			cs_detail detail = {};

			SyntheticInstruction()
			{
				instruction.detail = &detail;
			}
		};

		void setCall(
				SyntheticInstruction& instruction,
				std::uint64_t address,
				std::uint64_t target)
		{
			instruction.instruction.address = address;
			instruction.instruction.size = 5;
			instruction.instruction.id = X86_INS_CALL;
			instruction.detail.x86.op_count = 1;
			auto& operand = instruction.detail.x86.operands[0];
			operand.type = X86_OP_IMM;
			operand.imm = target;
			operand.size = 4;
		}

		void setMoveRegisterToStack(
				SyntheticInstruction& instruction,
				std::uint64_t address,
				std::int64_t displacement,
				x86_reg source)
		{
			instruction.instruction.address = address;
			instruction.instruction.size = 3;
			instruction.instruction.id = X86_INS_MOV;
			instruction.detail.x86.op_count = 2;
			auto& destination = instruction.detail.x86.operands[0];
			destination.type = X86_OP_MEM;
			destination.mem.base = X86_REG_EBP;
			destination.mem.index = X86_REG_INVALID;
			destination.mem.scale = 1;
			destination.mem.disp = displacement;
			destination.size = 4;
			destination.access = CS_AC_WRITE;
			auto& value = instruction.detail.x86.operands[1];
			value.type = X86_OP_REG;
			value.reg = source;
			value.size = 4;
			value.access = CS_AC_READ;
		}

		void setMoveMemoryToRegister(
				SyntheticInstruction& instruction,
				std::uint64_t address,
				x86_reg destinationRegister,
				x86_reg base,
				x86_reg index,
				int scale,
				std::int64_t displacement)
		{
			instruction.instruction.address = address;
			instruction.instruction.size = 3;
			instruction.instruction.id = X86_INS_MOV;
			instruction.detail.x86.op_count = 2;
			auto& destination = instruction.detail.x86.operands[0];
			destination.type = X86_OP_REG;
			destination.reg = destinationRegister;
			destination.size = 4;
			destination.access = CS_AC_WRITE;
			auto& source = instruction.detail.x86.operands[1];
			source.type = X86_OP_MEM;
			source.mem.base = base;
			source.mem.index = index;
			source.mem.scale = scale;
			source.mem.disp = displacement;
			source.size = 4;
			source.access = CS_AC_READ;
		}

		void setMoveRegister(
				SyntheticInstruction& instruction,
				std::uint64_t address,
				x86_reg destinationRegister,
				x86_reg sourceRegister)
		{
			instruction.instruction.address = address;
			instruction.instruction.size = 2;
			instruction.instruction.id = X86_INS_MOV;
			instruction.detail.x86.op_count = 2;
			auto& destination = instruction.detail.x86.operands[0];
			destination.type = X86_OP_REG;
			destination.reg = destinationRegister;
			destination.size = 4;
			destination.access = CS_AC_WRITE;
			auto& source = instruction.detail.x86.operands[1];
			source.type = X86_OP_REG;
			source.reg = sourceRegister;
			source.size = 4;
			source.access = CS_AC_READ;
		}

		void mapInstructions(const std::vector<cs_insn*>& decoded)
		{
			auto* mapping = module->getGlobalVariable("llvm2asm");
			ASSERT_NE(nullptr, mapping);
			AsmInstruction::setLlvmToAsmGlobalVariable(module.get(), mapping);
			std::size_t index = 0;
			for (auto& function : *module)
			{
				for (auto& block : function)
				{
					for (auto& llvmInstruction : block)
					{
						auto* store = llvm::dyn_cast<llvm::StoreInst>(&llvmInstruction);
						if (store != nullptr
								&& store->getPointerOperand() == mapping)
						{
							ASSERT_LT(index, decoded.size());
							AsmInstruction::getLlvmToCapstoneInsnMap(module.get())
									[store] = decoded[index++];
						}
					}
				}
			}
			ASSERT_EQ(decoded.size(), index);
		}

		rapidjson::Document writeMetadata(
				const std::shared_ptr<fileformat::FileFormat>& format)
		{
			llvm::SmallString<128> outputPath;
			int outputDescriptor = -1;
			EXPECT_FALSE(llvm::sys::fs::createTemporaryFile(
					"retdec-analysis-metadata", "json",
					outputDescriptor, outputPath));
			llvm::sys::fs::closeFile(outputDescriptor);

			retdec::config::Config configDb;
			configDb.architecture.setIsX86();
			configDb.architecture.setBitSize(32);
			configDb.parameters.setInputFile("fixture.bin");
			configDb.parameters.setOutputAnalysisMetadataFile(outputPath.str().str());
			auto* config = ConfigProvider::addConfig(module.get(), configDb);
			EXPECT_NE(nullptr, config);
			EXPECT_NE(nullptr,
					FileImageProvider::addFileImage(module.get(), format, config));

			AnalysisMetadataWriter writer;
			EXPECT_FALSE(writer.runOnModule(*module));
			std::ifstream output(outputPath.c_str(), std::ios::binary);
			EXPECT_TRUE(output);
			const std::string json{
					std::istreambuf_iterator<char>(output),
					std::istreambuf_iterator<char>()};
			llvm::sys::fs::remove(outputPath);
			rapidjson::Document metadata;
			metadata.Parse(json.c_str());
			EXPECT_FALSE(metadata.HasParseError());
			return metadata;
		}
};

TEST_F(AnalysisMetadataWriterTests, InputIdentityDescribesExactAnalyzedBytes)
{
	const std::string input = "RetDec metadata provenance";
	auto format = std::make_shared<fileformat::RawDataFormat>(
			reinterpret_cast<const std::uint8_t*>(input.data()),
			input.size());

	llvm::SmallString<128> outputPath;
	int outputDescriptor = -1;
	ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
			"retdec-analysis-metadata", "json", outputDescriptor, outputPath));
	llvm::sys::fs::closeFile(outputDescriptor);

	retdec::config::Config configDb;
	configDb.architecture.setIsX86();
	configDb.architecture.setBitSize(32);
	configDb.parameters.setInputFile("renamable-input.bin");
	configDb.parameters.setOutputAnalysisMetadataFile(outputPath.str().str());
	auto* config = ConfigProvider::addConfig(module.get(), configDb);
	ASSERT_NE(nullptr, config);
	ASSERT_NE(nullptr, FileImageProvider::addFileImage(module.get(), format, config));

	AnalysisMetadataWriter writer;
	EXPECT_FALSE(writer.runOnModule(*module));

	std::ifstream output(outputPath.c_str(), std::ios::binary);
	ASSERT_TRUE(output);
	const std::string json{
			std::istreambuf_iterator<char>(output),
			std::istreambuf_iterator<char>()};
	llvm::sys::fs::remove(outputPath);

	rapidjson::Document metadata;
	metadata.Parse(json.c_str());
	ASSERT_FALSE(metadata.HasParseError());
	ASSERT_TRUE(metadata.HasMember("input"));
	const auto& identity = metadata["input"];
	ASSERT_TRUE(identity.IsObject());
	ASSERT_TRUE(identity.HasMember("size"));
	EXPECT_EQ(input.size(), identity["size"].GetUint64());
	ASSERT_TRUE(identity.HasMember("sha256"));
	EXPECT_STREQ(
			"b7e34818e9b7698da3027f07573e2b7738b79d77f9c0fac69c69568aa375cee4",
			identity["sha256"].GetString());
}

TEST_F(AnalysisMetadataWriterTests, ExportsIncludeNameOrdinalRvaAndVa)
{
	const std::array<std::uint8_t, 1> input = {{0}};
	auto format = std::make_shared<TestFormat>(input.data(), input.size());
	format->addExport(0x1234, 7, "exported_entry");
	auto metadata = writeMetadata(format);

	ASSERT_TRUE(metadata.HasMember("exports"));
	ASSERT_EQ(1u, metadata["exports"].Size());
	const auto& exported = metadata["exports"][0];
	EXPECT_STREQ("exported_entry", exported["name"].GetString());
	EXPECT_EQ(7u, exported["ordinal"].GetUint64());
	EXPECT_EQ(0x234u, exported["rva"].GetUint64());
	EXPECT_EQ(0x1234u, exported["va"].GetUint64());
}

TEST_F(AnalysisMetadataWriterTests, FlattensResolvedSwitchThroughTranslatorBlocks)
{
	parseInput(R"(
		@llvm2asm = global i64 0
		define void @flow() {
		dec_label_pc_4000:
			store volatile i64 16384, i64* @llvm2asm
			br label %translator_switch
		translator_switch:
			switch i32 0, label %case_zero [
				i32 1, label %case_one
			]
		case_zero:
			store volatile i64 16400, i64* @llvm2asm
			ret void
		case_one:
			store volatile i64 16416, i64* @llvm2asm
			ret void
		}
	)");

	std::array<SyntheticInstruction, 3> decoded;
	setCall(decoded[0], 0x4000, 0x5000);
	setCall(decoded[1], 0x4010, 0x5000);
	setCall(decoded[2], 0x4020, 0x5000);
	mapInstructions({
			&decoded[0].instruction,
			&decoded[1].instruction,
			&decoded[2].instruction});

	const std::array<std::uint8_t, 1> input = {{0}};
	auto metadata = writeMetadata(
			std::make_shared<TestFormat>(input.data(), input.size()));
	const auto& blocks = metadata["functions"][0]["basic_blocks"];
	ASSERT_EQ(3u, blocks.Size());
	EXPECT_EQ(0x4000u, blocks[0]["address"].GetUint64());
	ASSERT_EQ(2u, blocks[0]["successors"].Size());
	EXPECT_EQ(0x4010u, blocks[0]["successors"][0].GetUint64());
	EXPECT_EQ(0x4020u, blocks[0]["successors"][1].GetUint64());
	EXPECT_EQ(0x4010u, blocks[1]["address"].GetUint64());
	EXPECT_EQ(0x4020u, blocks[2]["address"].GetUint64());
}

TEST_F(AnalysisMetadataWriterTests, TracesImportedReturnThroughStackAndIndexedLoads)
{
	parseInput(R"(
		@llvm2asm = global i64 0
		define void @flow() {
		dec_label_pc_1000:
			store volatile i64 4096, i64* @llvm2asm
			store volatile i64 4101, i64* @llvm2asm
			br label %dec_label_pc_1010
		dec_label_pc_1010:
			store volatile i64 4112, i64* @llvm2asm
			store volatile i64 4115, i64* @llvm2asm
			store volatile i64 4118, i64* @llvm2asm
			ret void
		}
	)");

	std::array<SyntheticInstruction, 5> decoded;
	setCall(decoded[0], 0x1000, 0x3000);
	setMoveRegisterToStack(decoded[1], 0x1005, -4, X86_REG_EAX);
	setMoveMemoryToRegister(
			decoded[2], 0x1010, X86_REG_ECX,
			X86_REG_EBP, X86_REG_INVALID, 1, -4);
	setMoveMemoryToRegister(
			decoded[3], 0x1013, X86_REG_EDX,
			X86_REG_ECX, X86_REG_INVALID, 1, 8);
	setMoveMemoryToRegister(
			decoded[4], 0x1016, X86_REG_ESI,
			X86_REG_EDX, X86_REG_EAX, 4, 16);
	mapInstructions({
			&decoded[0].instruction,
			&decoded[1].instruction,
			&decoded[2].instruction,
			&decoded[3].instruction,
			&decoded[4].instruction});

	const std::array<std::uint8_t, 1> input = {{0}};
	auto format = std::make_shared<TestFormat>(input.data(), input.size());
	format->addImport(0x3000);
	auto metadata = writeMetadata(format);

	ASSERT_TRUE(metadata.HasMember("functions"));
	ASSERT_EQ(1u, metadata["functions"].Size());
	const auto& flow = metadata["functions"][0]["value_flow"];
	ASSERT_TRUE(flow.IsObject());
	const auto& definitions = flow["definitions"];
	ASSERT_EQ(5u, definitions.Size());

	EXPECT_EQ(0x1000u, definitions[0]["address"].GetUint64());
	EXPECT_STREQ("call_return", definitions[0]["operation"].GetString());
	EXPECT_EQ(0x3000u, definitions[0]["call_target"].GetUint64());
	EXPECT_STREQ("stack_store", definitions[1]["operation"].GetString());
	EXPECT_EQ(0x1000u,
			definitions[1]["inputs"][0]["definition"].GetUint64());
	EXPECT_STREQ("stack_load", definitions[2]["operation"].GetString());
	EXPECT_EQ(0x1005u,
			definitions[2]["inputs"][0]["definition"].GetUint64());
	EXPECT_STREQ("pointer_load", definitions[3]["operation"].GetString());
	EXPECT_EQ(0x1010u,
			definitions[3]["inputs"][0]["definition"].GetUint64());
	EXPECT_STREQ("indexed_load", definitions[4]["operation"].GetString());
	ASSERT_EQ(2u, definitions[4]["inputs"].Size());
	EXPECT_STREQ("base", definitions[4]["inputs"][0]["role"].GetString());
	EXPECT_EQ(0x1013u,
			definitions[4]["inputs"][0]["definition"].GetUint64());
	EXPECT_STREQ("index", definitions[4]["inputs"][1]["role"].GetString());
	EXPECT_EQ(0x1000u,
			definitions[4]["inputs"][1]["definition"].GetUint64());
	EXPECT_TRUE(flow["ambiguous_merges"].Empty());
}

TEST_F(AnalysisMetadataWriterTests, AmbiguousMergeStopsValueFlow)
{
	parseInput(R"(
		@llvm2asm = global i64 0
		define void @flow() {
		dec_label_pc_2000:
			store volatile i64 8192, i64* @llvm2asm
			br i1 true, label %dec_label_pc_2010, label %dec_label_pc_2020
		dec_label_pc_2010:
			store volatile i64 8208, i64* @llvm2asm
			br label %dec_label_pc_2030
		dec_label_pc_2020:
			store volatile i64 8224, i64* @llvm2asm
			br label %dec_label_pc_2030
		dec_label_pc_2030:
			store volatile i64 8240, i64* @llvm2asm
			ret void
		}
	)");

	std::array<SyntheticInstruction, 4> decoded;
	setCall(decoded[0], 0x2000, 0x3000);
	setMoveRegister(decoded[1], 0x2010, X86_REG_ECX, X86_REG_EAX);
	setMoveRegister(decoded[2], 0x2020, X86_REG_ECX, X86_REG_EAX);
	setMoveRegister(decoded[3], 0x2030, X86_REG_EDX, X86_REG_ECX);
	mapInstructions({
			&decoded[0].instruction,
			&decoded[1].instruction,
			&decoded[2].instruction,
			&decoded[3].instruction});

	const std::array<std::uint8_t, 1> input = {{0}};
	auto format = std::make_shared<TestFormat>(input.data(), input.size());
	format->addImport(0x3000);
	auto metadata = writeMetadata(format);
	const auto& flow = metadata["functions"][0]["value_flow"];
	const auto& definitions = flow["definitions"];
	ASSERT_EQ(3u, definitions.Size());
	EXPECT_EQ(0x2000u, definitions[0]["address"].GetUint64());
	EXPECT_EQ(0x2010u, definitions[1]["address"].GetUint64());
	EXPECT_EQ(0x2020u, definitions[2]["address"].GetUint64());

	const auto& ambiguities = flow["ambiguous_merges"];
	ASSERT_EQ(1u, ambiguities.Size());
	EXPECT_EQ(0x2030u, ambiguities[0]["block"].GetUint64());
	EXPECT_STREQ(
			"ecx", ambiguities[0]["location"]["register"].GetString());
	ASSERT_EQ(2u, ambiguities[0]["candidate_definitions"].Size());
	EXPECT_EQ(0x2010u,
			ambiguities[0]["candidate_definitions"][0].GetUint64());
	EXPECT_EQ(0x2020u,
			ambiguities[0]["candidate_definitions"][1].GetUint64());
	EXPECT_FALSE(ambiguities[0]["includes_undefined"].GetBool());
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
