#include "TestHarness.h"

#include "TEPAK/PackReader.h"
#include "TEPAK/PackValidator.h"
#include "TEPAK/PackWriter.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace {
		class TempPackWorkspace {
		public:
			TempPackWorkspace()
				: root_(std::filesystem::temp_directory_path() / "TerraEngineEditorTests" / "PackRoundTrip") {
				std::filesystem::remove_all(root_);
				std::filesystem::create_directories(AssetRoot());
				std::filesystem::create_directories(OutputRoot());
			}

			~TempPackWorkspace() {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
			}

			[[nodiscard]] std::filesystem::path AssetRoot() const {
				return root_ / "Assets";
			}

			[[nodiscard]] std::filesystem::path OutputRoot() const {
				return root_ / "Output";
			}

			[[nodiscard]] std::filesystem::path PackPath() const {
				return OutputRoot() / "game.tepak";
			}

			void WriteFile(const std::filesystem::path& relativePath, const std::string& contents) const {
				const std::filesystem::path fullPath = AssetRoot() / relativePath;
				std::filesystem::create_directories(fullPath.parent_path());
				std::ofstream stream(fullPath, std::ios::binary);
				stream << contents;
			}

		private:
			std::filesystem::path root_;
		};

		std::string MakeLargeText() {
			std::string text;
			text.reserve(7000U);
			for(int index = 0; index < 7000; ++index) {
				text.push_back(static_cast<char>('A' + (index % 26)));
			}
			return text;
		}

		std::string MakeCompressibleText() {
			std::string text;
			text.reserve(12000U);
			for(int index = 0; index < 12000; ++index) {
				text.push_back((index % 64) == 0 ? '\n' : 'A');
			}
			return text;
		}

		std::string ReadTextFile(const std::filesystem::path& path) {
			std::ifstream stream(path, std::ios::binary);
			return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
		}

		tepak::PackagingRequest MakeRequest(const TempPackWorkspace& workspace, bool enableCompression = false) {
			tepak::PackagingRequest request;
			request.inputRoot = workspace.AssetRoot();
			request.outputPackPath = workspace.PackPath();
			request.extentSizingBytes = 4096U;
			request.workerCount = 2U;
			request.enableCompression = enableCompression;
			request.enableDictionaryTraining = false;
			request.compressionLevel = 9;
			request.overwriteExisting = true;
			request.peakMemoryBudgetBytes = 64ULL * 1024ULL * 1024ULL;
			return request;
		}

		tepak::ByteBuffer ToBytes(const std::string& text) {
			return tepak::ByteBuffer(text.begin(), text.end());
		}

		bool HasChild(const tepak::DirectoryListingResult& listing, const std::string& normalizedPath, tepak::PackNodeKind kind) {
			return std::any_of(listing.children.begin(), listing.children.end(), [&](const tepak::DirectoryChild& child) {
				return child.normalizedPath == normalizedPath && child.kind == kind;
			});
		}

		bool HasFinding(const tepak::ValidationReport& report, const std::string& ruleIdentifier) {
			return std::any_of(report.findings.begin(), report.findings.end(), [&](const tepak::ValidationFinding& finding) {
				return finding.ruleIdentifier == ruleIdentifier;
			});
		}

		bool HasCodec(const tepak::PackWriteResult& result, tepak::StorageMode mode) {
			return std::any_of(result.extents.begin(), result.extents.end(), [&](const tepak::PackExtent& extent) {
				return extent.codec == mode;
			});
		}

		void CorruptFirstStoredByte(const std::filesystem::path& packPath, const tepak::PackWriteResult& result) {
			const auto firstPayload = std::find_if(result.extents.begin(), result.extents.end(), [](const tepak::PackExtent& extent) {
				return extent.storedSize > 0U;
			});
			if(firstPayload == result.extents.end()) {
				throw std::runtime_error("Round-trip test expected at least one non-empty extent.");
			}

			std::fstream stream(packPath, std::ios::binary | std::ios::in | std::ios::out);
			stream.seekg(static_cast<std::streamoff>(firstPayload->storedOffset), std::ios::beg);
			char value = 0;
			stream.read(&value, 1);
			value = static_cast<char>(value ^ 0x01);
			stream.seekp(static_cast<std::streamoff>(firstPayload->storedOffset), std::ios::beg);
			stream.write(&value, 1);
		}
	}

	void RegisterPackRoundTripTests(TestRegistry& registry) {
		registry.Add("Editor.PackRoundTrip.CreateReadListRangeAndValidateRawPack", [](TestContext& context) {
			TempPackWorkspace workspace;
			const std::string storyText = MakeLargeText();
			const std::string textureText = "fake texture payload";
			workspace.WriteFile("Data/Story.TXT", storyText);
			workspace.WriteFile("Textures/Hero.PNG", textureText);

			const tepak::PackWriteResult writeResult = tepak::PackWriter::Create(MakeRequest(workspace));
			const tepak::PackReader reader = tepak::PackReader::Open(workspace.PackPath());
			const tepak::PackInspectResult inspect = reader.Inspect();
			const tepak::DirectoryListingResult rootListing = reader.ListDirectory();
			const tepak::DirectoryListingResult dataListing = reader.ListDirectory("DATA");
			const tepak::ByteBuffer storyBytes = reader.ReadAll(R"(DATA\Story.txt)");
			const tepak::ByteBuffer textureBytes = reader.ReadAll("textures/hero.png");
			const tepak::ByteBuffer rangeBytes = reader.ReadRange("data/story.txt", 4090U, 64U);
			const tepak::ValidationReport validation = tepak::PackValidator::Validate(workspace.PackPath(), true);

			context.Expect(std::filesystem::exists(workspace.PackPath()), "PackWriter should create the requested archive file.");
			context.ExpectEqual(2U, writeResult.descriptor.fileCount, "The writer should record both staged files.");
			context.ExpectEqual(2U, reader.GetDescriptor().fileCount, "The reader descriptor should preserve the written file count.");
			context.ExpectEqual(2u, static_cast<unsigned int>(inspect.entries.size()), "Inspect should expose both file entries.");
			context.Expect(writeResult.descriptor.extentCount >= 3U, "A large file plus a small file should produce multiple raw extents.");
			context.Expect(HasChild(rootListing, "data", tepak::PackNodeKind::Directory), "The root listing should include the data directory.");
			context.Expect(HasChild(rootListing, "textures", tepak::PackNodeKind::Directory), "The root listing should include the textures directory.");
			context.Expect(HasChild(dataListing, "data/story.txt", tepak::PackNodeKind::File), "The data listing should include the staged story file.");
			context.Expect(storyBytes == ToBytes(storyText), "ReadAll should reconstruct the original story bytes.");
			context.Expect(textureBytes == ToBytes(textureText), "ReadAll should reconstruct the original texture bytes.");
			context.Expect(rangeBytes == ToBytes(storyText.substr(4090U, 64U)), "ReadRange should reconstruct bytes that cross an extent boundary.");
			context.Expect(validation.validationStatus, "A freshly written pack should pass deep validation.");
			context.ExpectEqual(0ULL, validation.errorCount, "A freshly written pack should report no validation errors.");
			context.Expect(validation.integrityChecksPerformed, "Deep validation should record that integrity checks were performed.");
		});

		registry.Add("Editor.PackRoundTrip.CompressedPackReadsRangesAndValidates", [](TestContext& context) {
			TempPackWorkspace workspace;
			const std::string compressedText = MakeCompressibleText();
			workspace.WriteFile("Data/Compressed.TXT", compressedText);

			const tepak::PackWriteResult writeResult = tepak::PackWriter::Create(MakeRequest(workspace, true));
			const tepak::PackReader reader = tepak::PackReader::Open(workspace.PackPath());
			const tepak::ByteBuffer fullBytes = reader.ReadAll("data/compressed.txt");
			const tepak::ByteBuffer rangeBytes = reader.ReadRange("data/compressed.txt", 4090U, 128U);
			const tepak::ValidationReport validation = tepak::PackValidator::Validate(workspace.PackPath(), true);
			const std::uint64_t zstdMask = 1ULL << static_cast<std::uint32_t>(tepak::StorageMode::Zstd);

			context.Expect((writeResult.descriptor.codecMask & zstdMask) != 0U, "A compressible pack should record zstd in the descriptor codec mask.");
			context.Expect(HasCodec(writeResult, tepak::StorageMode::Zstd), "At least one extent should be stored with plain zstd compression.");
			context.Expect(writeResult.extents.front().storedSize < writeResult.extents.front().uncompressedSize, "Compressed extents should store fewer bytes than their logical payload.");
			context.Expect(fullBytes == ToBytes(compressedText), "ReadAll should reconstruct compressed file bytes.");
			context.Expect(rangeBytes == ToBytes(compressedText.substr(4090U, 128U)), "ReadRange should reconstruct compressed bytes across an extent boundary.");
			context.Expect(validation.validationStatus, "A compressed pack should pass deep validation.");
			context.ExpectEqual(0ULL, validation.errorCount, "A compressed pack should not report validation errors.");
		});

		registry.Add("Editor.PackRoundTrip.ExtractsSingleFileAndDirectorySubtree", [](TestContext& context) {
			TempPackWorkspace workspace;
			const std::string storyText = MakeLargeText();
			const std::string textureText = "fake texture payload";
			workspace.WriteFile("Data/Story.TXT", storyText);
			workspace.WriteFile("Textures/Hero.PNG", textureText);

			(void)tepak::PackWriter::Create(MakeRequest(workspace));
			const tepak::PackReader reader = tepak::PackReader::Open(workspace.PackPath());
			const std::filesystem::path fileExtractRoot = workspace.OutputRoot() / "ExtractedFile";
			const std::filesystem::path treeExtractRoot = workspace.OutputRoot() / "ExtractedTree";

			reader.Extract("data/story.txt", fileExtractRoot, 1U);
			reader.Extract("textures", treeExtractRoot, 2U);

			context.ExpectEqual(storyText, ReadTextFile(fileExtractRoot / "data" / "story.txt"), "Extracting one file should write the original logical bytes under its normalized path.");
			context.ExpectEqual(textureText, ReadTextFile(treeExtractRoot / "textures" / "hero.png"), "Extracting a directory should write every file in that subtree.");
		});

		registry.Add("Editor.PackRoundTrip.CorruptedRawPayloadFailsDeepValidation", [](TestContext& context) {
			TempPackWorkspace workspace;
			workspace.WriteFile("Data/Story.TXT", MakeLargeText());

			const tepak::PackWriteResult writeResult = tepak::PackWriter::Create(MakeRequest(workspace));
			const tepak::ValidationReport beforeCorruption = tepak::PackValidator::Validate(workspace.PackPath(), true);
			CorruptFirstStoredByte(workspace.PackPath(), writeResult);
			const tepak::ValidationReport shallowValidation = tepak::PackValidator::Validate(workspace.PackPath(), false);
			const tepak::ValidationReport deepValidation = tepak::PackValidator::Validate(workspace.PackPath(), true);

			context.Expect(beforeCorruption.validationStatus, "The generated pack should be valid before corruption.");
			context.Expect(shallowValidation.validationStatus, "Shallow validation should not flag payload-only corruption.");
			context.Expect(!shallowValidation.integrityChecksPerformed, "Shallow validation should record that integrity checks were skipped.");
			context.Expect(!deepValidation.validationStatus, "Deep validation should reject corrupted payload bytes.");
			context.Expect(deepValidation.errorCount > 0U, "Deep validation should report at least one corruption error.");
			context.Expect(HasFinding(deepValidation, "extent-hash") || HasFinding(deepValidation, "entry-hash"), "Deep validation should report a hash-related corruption finding.");
		});
	}
}
