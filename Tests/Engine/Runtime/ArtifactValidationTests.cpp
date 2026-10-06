#include "TestHarness.h"
#include "Runtime/ArtifactValidation.h"
#include "ArtifactValidationTestCorpus.h"
#include "../Resource/TextureTestFixtures.h"
#include "File Formats/TerraEngineTextureFile.h"
#include <cstring>
#include <limits>
#include <array>
#include <string_view>

namespace TerraEngine::Tests {
	namespace {
		Runtime::ArtifactValidationResult Inspect(const ArtifactCorpus::Buffer& bytes, Runtime::NativeImageTarget target) {
			return Runtime::ValidateArtifact({bytes, Runtime::ArtifactKind::NativeGameplay, target, "Managed/Assembly-CSharp"});
		}
		// One real TETF envelope with a complete one-mip descriptor and one
		// UAST block lets negative cases alter metadata instead of using text.
		ArtifactCorpus::Buffer TextureEnvelope() {
			TETFHeader header{}; header.magic = kMagic; header.version = kVersion;
			header.headerSize = sizeof(header); header.chunkCount = 2; header.tocOffset = sizeof(header);
			const auto dataStart = sizeof(header) + 2 * sizeof(TETFChunkEntry);
			TETFChunkEntry descEntry{kChunkDESC, 0, dataStart, sizeof(TETFDesc) + sizeof(TETFLevelEntry)};
			TETFChunkEntry payload{kChunkUAST, 0, dataStart + descEntry.size, 16};
			header.totalSize = payload.offset + payload.size;
			ArtifactCorpus::Buffer bytes(header.totalSize);
			TETFDesc desc{}; desc.width = 4; desc.height = 4; desc.depth = 1; desc.layers = 1;
			desc.mipCount = 1; desc.faces = 1; desc.blockWidth = 4; desc.blockHeight = 4; desc.blockBytes = 16;
			desc.payloadChunkIndex = 1; desc.userMetadataChunk = TETF_INVALID_CHUNK_INDEX; desc.thumbnailChunk = TETF_INVALID_CHUNK_INDEX;
			const TETFLevelEntry level{0, 16};
			std::memcpy(bytes.data(), &header, sizeof(header));
			std::memcpy(bytes.data() + header.tocOffset, &descEntry, sizeof(descEntry));
			std::memcpy(bytes.data() + header.tocOffset + sizeof(descEntry), &payload, sizeof(payload));
			std::memcpy(bytes.data() + descEntry.offset, &desc, sizeof(desc));
			std::memcpy(bytes.data() + descEntry.offset + sizeof(desc), &level, sizeof(level));
			return bytes;
		}
	}
	void RegisterArtifactValidationTests(TestRegistry& registry) {
		registry.Add("Engine.ArtifactValidation.PeBootstrapCorpus", [](TestContext& context) {
			for(const std::string_view name : {"InitManagedRuntimeV4", "InitManagedRuntimeV3", "InitManagedRuntime"}) {
				const auto result = Inspect(ArtifactCorpus::Pe(name), Runtime::NativeImageTarget::WindowsX64);
				context.Expect(result.IsValid() && result.bootstrap == name, "Every supported PE bootstrap contract must be inspected without loading code.");
			}
		});
		registry.Add("Engine.ArtifactValidation.ElfBootstrapCorpus", [](TestContext& context) {
			for(const std::string_view name : {"InitManagedRuntimeV4", "InitManagedRuntimeV3", "InitManagedRuntime"}) {
				for(const bool gnu : {false, true}) {
					const auto result = Inspect(ArtifactCorpus::Elf(name, gnu), Runtime::NativeImageTarget::LinuxX64);
					context.Expect(result.IsValid() && result.bootstrap == name, "SysV/GNU hash ELF exports must accept every supported bootstrap contract.");
				}
			}
		});
		registry.Add("Engine.ArtifactValidation.NativeBoundsCorpus", [](TestContext& context) {
			// Every prefix of an otherwise valid image exercises truncated headers,
			// mapped tables, and symbol names. No count-driven out-of-bounds read is legal.
			for(const bool pe : {true, false}) {
				const auto image = pe ? ArtifactCorpus::Pe() : ArtifactCorpus::Elf();
				for(std::size_t size = 0; size < image.size(); ++size) {
					auto prefix = image; prefix.resize(size);
					context.Expect(!Inspect(prefix, pe ? Runtime::NativeImageTarget::WindowsX64 : Runtime::NativeImageTarget::LinuxX64).IsValid(), "Truncated native images must fail closed.");
				}
			}
			auto overstated = ArtifactCorpus::Pe(); ArtifactCorpus::Put(overstated, 0x98 + 108, 0xffffffff, 4);
			context.Expect(!Inspect(overstated, Runtime::NativeImageTarget::WindowsX64).IsValid(), "PE directory count must fit the optional-header bytes.");
			auto conflicting = ArtifactCorpus::Elf();
			ArtifactCorpus::Put(conflicting, 152, 112, 8); ArtifactCorpus::Put(conflicting, 160, 112, 8);
			ArtifactCorpus::Put(conflicting, 0x180 + 5 * 16, 5, 8); ArtifactCorpus::Put(conflicting, 0x188 + 5 * 16, 0x400281, 8);
			context.Expect(!Inspect(conflicting, Runtime::NativeImageTarget::LinuxX64).IsValid(), "Conflicting ELF dynamic table addresses must not use last-value-wins parsing.");

			const auto inspectTexture = [](const ArtifactCorpus::Buffer& bytes) {
				return Runtime::ValidateArtifact({bytes, Runtime::ArtifactKind::Texture, Runtime::NativeImageTarget::Unsupported, "Textures/Bounds.tetf"});
			};
			// Legal partial and full chains use fresh legal UASTC blocks. The gate
			// remains structural: successful inspection does not promise decoding.
			for(const unsigned mips : {2U,4U}) {
				const auto fixture=TextureFixtures::Make(8,4,mips);
				context.Expect(Runtime::ValidateArtifact({fixture.bytes,Runtime::ArtifactKind::Texture,Runtime::NativeImageTarget::Unsupported,"Textures/compiler-control.tetf"}).IsValid(), "Legal partial/full compiler-style mip domains remain supported.");
			}
			for(const unsigned row : {0U,1U,2U,3U}) {
				auto fixture=TextureFixtures::Make(8,8,4); auto desc=fixture.Desc();
				if(row==0) desc.width=0; else if(row==1) desc.height=0; else if(row==2) desc.mipCount=0; else { desc.width=4; desc.height=4; }
				fixture.SetDesc(desc); fixture.Checksum();
				context.Expect(!Runtime::ValidateArtifact({fixture.bytes,Runtime::ArtifactKind::Texture,Runtime::NativeImageTarget::Unsupported,"Textures/mip-domain.tetf"}).IsValid(), "Zero or overlong 2D mip domains must reject before descriptor shifts.");
			}
			const auto completeTexture = TextureEnvelope();
			context.Expect(inspectTexture(completeTexture).IsValid(), "Complete texture metadata must pass nonexecuting inspection.");
			auto texture = completeTexture;
			ArtifactCorpus::Put(texture, sizeof(TETFHeader) + offsetof(TETFChunkEntry, size), sizeof(TETFDesc), 8);
			context.Expect(!inspectTexture(texture).IsValid(), "Descriptor mip count must not accept a missing mip table.");
			texture = completeTexture;
			const auto levelOffset = sizeof(TETFHeader) + 2 * sizeof(TETFChunkEntry) + sizeof(TETFDesc);
			ArtifactCorpus::Put(texture, levelOffset, std::numeric_limits<std::uint64_t>::max(), 8);
			context.Expect(!inspectTexture(texture).IsValid(), "Mip offsets must be bounded without wrapping addition.");
			texture = completeTexture;
			ArtifactCorpus::Put(texture, sizeof(TETFHeader) + sizeof(TETFChunkEntry) + offsetof(TETFChunkEntry, size), 15, 8);
			ArtifactCorpus::Put(texture, levelOffset + offsetof(TETFLevelEntry, size), 15, 8);
			context.Expect(!inspectTexture(texture).IsValid(), "UAST payload must supply all blocks consumed by declared mip dimensions.");
			texture = completeTexture;
			ArtifactCorpus::Put(texture, sizeof(TETFHeader) + offsetof(TETFChunkEntry, offset), 0, 8);
			context.Expect(!inspectTexture(texture).IsValid(), "Texture chunks cannot alias header or table storage.");

		});
		registry.Add("Engine.ArtifactValidation.ArchitectureAndKindCorpus", [](TestContext& context) {
			using namespace ArtifactCorpus;
			auto pe = Pe(); Put(pe, 0x84, 0x14c, 2);
			context.Expect(!Inspect(pe, Runtime::NativeImageTarget::WindowsX64).IsValid(), "PE x86 cannot masquerade as x64 gameplay.");
			pe = Pe(); Put(pe, 0x96, 2, 2);
			context.Expect(!Inspect(pe, Runtime::NativeImageTarget::WindowsX64).IsValid(), "PE executable without DLL identity is not gameplay library.");
			auto elf = Elf(); Put(elf, 18, 183, 2);
			context.Expect(!Inspect(elf, Runtime::NativeImageTarget::LinuxX64).IsValid(), "ELF ARM64 cannot masquerade as x64 gameplay.");
			elf = Elf(); Put(elf, 16, 2, 2);
			context.Expect(!Inspect(elf, Runtime::NativeImageTarget::LinuxX64).IsValid(), "ELF ET_EXEC is not a shared gameplay library.");
		});
		registry.Add("Engine.ArtifactValidation.ExportAuthorityCorpus", [](TestContext& context) {
			using namespace ArtifactCorpus;
			context.Expect(!Inspect(Pe("DifferentEntry"), Runtime::NativeImageTarget::WindowsX64).IsValid(), "Unrelated PE export cannot satisfy bootstrap contract.");
			context.Expect(!Inspect(Elf("DifferentEntry"), Runtime::NativeImageTarget::LinuxX64).IsValid(), "Unrelated ELF symbol cannot satisfy bootstrap contract.");
			auto pe = Pe(); Put(pe, 0x330, 0x1160, 4);
			context.Expect(!Inspect(pe, Runtime::NativeImageTarget::WindowsX64).IsValid(), "A PE forwarded bootstrap must not be accepted as executable code.");
			for(const auto field : std::array<std::pair<std::size_t, unsigned>, 3>{{{0x31c, 0x02}, {0x31d, 2}, {0x31e, 0}}}) {
				auto elf = Elf(); Put(elf, field.first, field.second, 1);
				context.Expect(!Inspect(elf, Runtime::NativeImageTarget::LinuxX64).IsValid(), "ELF local, hidden, or undefined bootstrap symbols must fail.");
			}
		});
		registry.Add("Engine.ArtifactValidation.UnsupportedAndTextCorpus", [](TestContext& context) {
			const auto unsupported = Inspect(ArtifactCorpus::Pe(), Runtime::NativeImageTarget::Unsupported);
			context.Expect(unsupported.disposition == Runtime::ArtifactDisposition::Unsupported && unsupported.code == "native.target.unsupported", "Unsupported native targets need an explicit decision.");
			const std::string_view text = "unfinished native gameplay image";
			const auto result = Runtime::ValidateArtifact({std::as_bytes(std::span(text.data(), text.size())), Runtime::ArtifactKind::NativeGameplay, Runtime::NativeImageTarget::WindowsX64, "Managed/Assembly-CSharp.dll"});
			context.Expect(!result.IsValid() && !result.IsExplicitStub(), "Text is rejected structurally without inventing a stub word.");
		});
	}
}
