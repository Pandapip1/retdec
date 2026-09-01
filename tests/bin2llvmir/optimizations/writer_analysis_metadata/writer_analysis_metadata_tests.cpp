/**
 * @file tests/bin2llvmir/optimizations/writer_analysis_metadata/writer_analysis_metadata_tests.cpp
 * @brief Tests for the machine-readable analysis metadata writer.
 */

#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <rapidjson/document.h>

#include "retdec/bin2llvmir/optimizations/writer_analysis_metadata/writer_analysis_metadata.h"
#include "retdec/bin2llvmir/providers/config.h"
#include "retdec/bin2llvmir/providers/fileimage.h"
#include "retdec/fileformat/file_format/raw_data/raw_data_format.h"
#include "bin2llvmir/utils/llvmir_tests.h"

namespace retdec {
namespace bin2llvmir {
namespace tests {

class AnalysisMetadataWriterTests : public LlvmIrTests
{
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

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
