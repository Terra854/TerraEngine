#include "TestHarness.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "TEPAK/PackReader.h"
#include "TEPAK/PackValidator.h"
#include "TEPAK/PackWriter.h"
#include "archive/ArchiveWorkflow.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
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

			void WriteBytes(const std::filesystem::path& relativePath, const std::vector<std::uint8_t>& contents) const {
				const std::filesystem::path fullPath = AssetRoot() / relativePath;
				std::filesystem::create_directories(fullPath.parent_path());
				std::ofstream stream(fullPath, std::ios::binary);
				stream.write(reinterpret_cast<const char*>(contents.data()),
					static_cast<std::streamsize>(contents.size()));
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

		std::vector<std::uint8_t> MakePagedTemfExtentFixture() {
			constexpr std::uint64_t kFirstPageBytes = 40u;
			constexpr std::uint64_t kSecondPageBytes = 56u;
			const std::uint64_t pageTableOffset = sizeof(TemfProductionHeader);
			const std::uint64_t blockTableOffset = pageTableOffset + 2u * sizeof(TemfPageDescriptor);
			const std::uint64_t payloadOffset = blockTableOffset + 2u * sizeof(TemfBlockDescriptor);
			std::vector<std::uint8_t> bytes(static_cast<std::size_t>(
				payloadOffset + kFirstPageBytes + kSecondPageBytes), 0u);

			TemfProductionHeader header{};
			header.logicalPageSizeBytes = kTemfPageSize64KiB;
			header.pageCount = 2u;
			header.blockCount = 2u;
			header.pageTableOffset = pageTableOffset;
			header.blockTableOffset = blockTableOffset;
			header.payloadOffset = payloadOffset;
			header.payloadBytes = kFirstPageBytes + kSecondPageBytes;
			header.directoryBytes = payloadOffset - sizeof(TemfProductionHeader);
			std::memcpy(bytes.data(), &header, sizeof(header));

			TemfPageDescriptor pages[2]{};
			pages[0].logicalPageId = 3u;
			pages[0].firstBlockIndex = 0u;
			pages[0].blockCount = 1u;
			pages[0].attributedStoredBytes = kFirstPageBytes;
			pages[1].logicalPageId = 4u;
			pages[1].firstBlockIndex = 1u;
			pages[1].blockCount = 1u;
			pages[1].attributedStoredBytes = kSecondPageBytes;
			std::memcpy(bytes.data() + pageTableOffset, pages, sizeof(pages));

			TemfBlockDescriptor blocks[2]{};
			blocks[0].logicalPageId = pages[0].logicalPageId;
			blocks[0].encodedPayloadOffset = payloadOffset;
			blocks[0].encodedBytes = kFirstPageBytes;
			blocks[1].logicalPageId = pages[1].logicalPageId;
			blocks[1].encodedPayloadOffset = payloadOffset + kFirstPageBytes;
			blocks[1].encodedBytes = kSecondPageBytes;
			std::memcpy(bytes.data() + blockTableOffset, blocks, sizeof(blocks));
			for(std::size_t index = static_cast<std::size_t>(payloadOffset); index < bytes.size(); ++index) {
				bytes[index] = static_cast<std::uint8_t>((index * 17u) & 0xffu);
			}
			return bytes;
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

		registry.Add("Editor.PackRoundTrip.PagedTemfUsesMetadataAndLogicalPageExtents", [](TestContext& context) {
			TempPackWorkspace workspace;
			const std::vector<std::uint8_t> temfBytes = MakePagedTemfExtentFixture();
			workspace.WriteBytes("Meshes/Paged.temf", temfBytes);
			tepak::PackagingRequest request = MakeRequest(workspace, true);
			request.extentSizingBytes = 1024u * 1024u;

			const tepak::PackWriteResult writeResult = tepak::PackWriter::Create(request);
			const tepak::PackReader reader = tepak::PackReader::Open(workspace.PackPath());
			const std::uint64_t payloadOffset = sizeof(TemfProductionHeader)
				+ 2u * sizeof(TemfPageDescriptor) + 2u * sizeof(TemfBlockDescriptor);
			context.ExpectEqual(3u, static_cast<unsigned int>(writeResult.extents.size()),
				"A paged TEMF must use one metadata extent and one extent per logical page.");
			context.ExpectEqual(payloadOffset, writeResult.extents[0].uncompressedSize,
				"The first TEMF extent must stop exactly at the encoded payload.");
			context.ExpectEqual(40ull, writeResult.extents[1].uncompressedSize,
				"The first page must retain its authored compact byte boundary.");
			context.ExpectEqual(56ull, writeResult.extents[2].uncompressedSize,
				"The second page must retain its authored compact byte boundary.");
			context.Expect(reader.ReadAll("meshes/paged.temf") == temfBytes,
				"Page-aware extent planning must preserve the exact TEMF byte stream.");
			context.Expect(reader.ReadRange("meshes/paged.temf", payloadOffset + 40u, 56u)
				== tepak::ByteBuffer(temfBytes.begin() + static_cast<std::ptrdiff_t>(payloadOffset + 40u), temfBytes.end()),
				"A logical page range must reconstruct from exactly one matching TEPAK extent.");
		});

		registry.Add("Editor.PackRoundTrip.DictionaryDecisionChargesStoredDictionaryBytes", [](TestContext& context) {
			context.Expect(!tepak::archive::IsDictionaryStorageBeneficial(1000u, 900u, 128u),
				"A dictionary must be rejected when its stored bytes exceed the sampled extent savings.");
			context.Expect(tepak::archive::IsDictionaryStorageBeneficial(1000u, 700u, 128u),
				"A dictionary may be accepted after repaying its storage and safety margin.");
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
