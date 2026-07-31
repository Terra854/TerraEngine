// SkinnedMeshCookTests.cpp - Deformable TEMF cooking and publication contracts.

#include "TestHarness.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "MeshCompiler.h"
#include "Resource/ResourceManager.h"
#include "Resource/ResourceStreaming.h"
#include "Resource/VirtualGeometryCodec.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace {
		constexpr std::uint64_t kTestSkeletonCompatibilityFingerprint = 0x534B454C45544F4EULL;

		SkinnedMeshCookSettings MakeBoundCookSettings(std::uint32_t jointCount = 4096U) {
			SkinnedMeshCookSettings settings;
			settings.sourceSkeletonJointCount = jointCount;
			settings.skeletonCompatibilityFingerprint = kTestSkeletonCompatibilityFingerprint;
			return settings;
		}

		std::filesystem::path TestRoot() {
			const std::filesystem::path root = std::filesystem::temp_directory_path()
				/ "TerraEngineAnimationSkinnedMeshCookTests";
			std::filesystem::create_directories(root);
			return root;
		}

		SkinnedMeshCookInput MakeTriangleInput() {
			SkinnedMeshCookInput input;
			input.vertices.resize(3U);
			input.vertices[0].baseVertex.position = {0.0F, 0.0F, 0.0F};
			input.vertices[1].baseVertex.position = {1.0F, 0.0F, 0.0F};
			input.vertices[2].baseVertex.position = {0.0F, 1.0F, 0.0F};
			for(SkinnedMeshCookVertex& vertex : input.vertices) {
				vertex.influences = {{0U, 1.0F}};
			}
			input.indices = {0U, 1U, 2U};
			return input;
		}

		SkinnedMeshCookInput MakePartitionInput() {
			SkinnedMeshCookInput input;
			input.vertices.resize(258U);
			input.indices.reserve(input.vertices.size());
			for(std::uint32_t index = 0U; index < input.vertices.size(); ++index) {
				const float triangle = static_cast<float>(index / 3U) * 2.0F;
				const std::uint32_t corner = index % 3U;
				input.vertices[index].baseVertex.position = corner == 0U
					? std::array<float, 3U>{triangle, 0.0F, 0.0F}
					: corner == 1U
						? std::array<float, 3U>{triangle + 1.0F, 0.0F, 0.0F}
						: std::array<float, 3U>{triangle, 1.0F, 0.0F};
				input.vertices[index].influences = {{index, 1.0F}};
				input.indices.push_back(index);
			}
			return input;
		}

		SkinnedMeshCookInput MakeSkinnedGridInput(std::uint32_t quadsPerSide) {
			SkinnedMeshCookInput input;
			const std::uint32_t verticesPerSide = quadsPerSide + 1U;
			input.vertices.reserve(
				static_cast<std::size_t>(verticesPerSide) * verticesPerSide);
			input.indices.reserve(
				static_cast<std::size_t>(quadsPerSide) * quadsPerSide * 6U);
			for(std::uint32_t y = 0U; y < verticesPerSide; ++y) {
				for(std::uint32_t x = 0U; x < verticesPerSide; ++x) {
					const float fx = static_cast<float>(x)
						/ static_cast<float>(quadsPerSide);
					const float fy = static_cast<float>(y)
						/ static_cast<float>(quadsPerSide);
					SkinnedMeshCookVertex vertex;
					vertex.baseVertex.position = {
						fx * 2.0F - 1.0F,
						fy * 2.0F - 1.0F,
						0.05F * std::sin(fx * 12.0F)
							* std::cos(fy * 10.0F) };
					vertex.baseVertex.normal = {0.0F, 0.0F, 1.0F};
					vertex.baseVertex.uv = {fx, fy};
					vertex.influences = {{0U, 1.0F}};
					input.vertices.push_back(std::move(vertex));
				}
			}
			const auto VertexIndex = [verticesPerSide](
				std::uint32_t x,
				std::uint32_t y) {
				return y * verticesPerSide + x;
			};
			for(std::uint32_t y = 0U; y < quadsPerSide; ++y) {
				for(std::uint32_t x = 0U; x < quadsPerSide; ++x) {
					const std::uint32_t a = VertexIndex(x, y);
					const std::uint32_t b = VertexIndex(x + 1U, y);
					const std::uint32_t c = VertexIndex(x, y + 1U);
					const std::uint32_t d = VertexIndex(x + 1U, y + 1U);
					input.indices.insert(
						input.indices.end(),
						{a, c, b, b, c, d});
				}
			}
			return input;
		}

		SkinnedMeshCookInput MakeDeindexedSkinnedGridInput(std::uint32_t quadsPerSide) {
			const SkinnedMeshCookInput indexed = MakeSkinnedGridInput(quadsPerSide);
			SkinnedMeshCookInput deindexed;
			deindexed.vertices.reserve(indexed.indices.size());
			deindexed.indices.reserve(indexed.indices.size());
			for(const std::uint32_t sourceIndex : indexed.indices) {
				deindexed.vertices.push_back(indexed.vertices[sourceIndex]);
				deindexed.indices.push_back(
					static_cast<std::uint32_t>(deindexed.indices.size()));
			}
			return deindexed;
		}

		std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path) {
			std::ifstream input(path, std::ios::binary);
			return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
		}

		void WriteSentinel(const std::filesystem::path& path, const std::string& value) {
			std::filesystem::create_directories(path.parent_path());
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			output.write(value.data(), static_cast<std::streamsize>(value.size()));
		}

		bool PalettesEqual(
			const std::vector<SkinnedMeshPalettePartition>& left,
			const std::vector<SkinnedMeshPalettePartition>& right) {
			if(left.size() != right.size()) {
				return false;
			}
			for(std::size_t index = 0U; index < left.size(); ++index) {
				if(left[index].sourceJointIds != right[index].sourceJointIds) {
					return false;
				}
			}
			return true;
		}
	}

	void RegisterSkinnedMeshCookTests(TestRegistry& registry) {
		registry.Add("Editor.Animation.SkinnedMeshCook.ReducesToFourInfluencesAndHandlesZeroWeights", [](TestContext& context) {
			SkinnedMeshCookInput input = MakeTriangleInput();
			input.vertices[0].influences = {
				{4U, 0.05F}, {1U, 0.30F}, {3U, 0.10F},
				{0U, 0.40F}, {2U, 0.20F}
			};
			input.vertices[1].influences = {{2U, 0.0F}};
			// Joint four's tiny positive weight survives floating-point reduction
			// but rounds to zero in UNORM8. The encoded joint slot must therefore be
			// canonical zero so the runtime never attempts to sample that palette ID.
			input.vertices[2].influences = {{0U, 0.999999F}, {4U, 0.000001F}};
			const std::filesystem::path outputPath = TestRoot() / "influence-reduction.temf";
			SkinnedMeshCookReport report;
			const bool compiled = CompileSkinnedMeshFromRaw(
				"influence-reduction", outputPath, input, &report,
				MakeBoundCookSettings());
			context.Expect(compiled && report.published && report.failure == SkinnedMeshCookFailure::None,
				"A valid skinned triangle should publish a deformable TEMF generation.");
			context.Expect(report.cookedVertices.size() == input.vertices.size()
				&& report.cookedVertices[0].influences.size() == kTemfMaximumSkinInfluences,
				"Cooking must retain exactly the four strongest influences.");
			context.Expect(std::abs(report.cookedVertices[0].normalizedWeightSum - 1.0F)
				<= report.weightQuantizationUnit,
				"Retained weights must normalize to one within one UNORM8 unit.");
			context.Expect(std::abs(report.cookedVertices[0].discardedWeight - 0.05F) <= 0.00001F,
				"The report must expose the deterministic discarded influence weight.");
			context.Expect(report.zeroWeightVertexCount == 1U
				&& report.cookedVertices[1].usesIdentityDeformation,
				"A zero-weight vertex must use the documented identity-deformation fallback.");
			context.Expect(report.cookedVertices[2].quantizedWeights[1] == 0U
				&& report.cookedVertices[2].paletteJointIndices[1] == 0U,
				"An influence that quantizes to zero must use canonical palette-local joint zero.");
			const AnimationResourceValidationResult runtimeContract = ResourceManager::Instance()
				.ValidateDeformableMeshFile(outputPath, 4096U, kTestSkeletonCompatibilityFingerprint);
			context.Expect(runtimeContract.status == TerraEngine::Animation::AnimationStatus::Ok,
				"The runtime validator must accept a cooked mesh containing a canonicalized sub-UNORM8 influence.");
		});

		registry.Add("Editor.Animation.SkinnedMeshCook.CanonicalDemandedPagesDecodeIndependently", [](TestContext& context) {
			const std::filesystem::path outputPath =
				TestRoot() / "canonical-demanded-pages.temf";
			SkinnedMeshCookInput pageInput = MakeSkinnedGridInput(47u);
			SkinnedMeshCookReport pageReport{};
			const bool pageCompiled = CompileSkinnedMeshFromRaw(
				"canonical-demanded-pages", outputPath, pageInput, &pageReport,
				MakeBoundCookSettings());
			context.Expect(pageCompiled,
				"A skinned cook must publish canonical demanded-page streams: "
					+ pageReport.diagnosticMessage);
			const std::vector<std::uint8_t> bytes = ReadBytes(outputPath);
			const auto validated = TerraEngine::Resource::ValidateTemfProductionFile(bytes, {});
			context.Expect(validated.IsComplete(),
				"Demanded-page regression data must remain a complete TEMFPRD1 file.");
			if (!validated.IsComplete()) return;

			const auto DecodeMetadata = [&validated, &bytes](std::uint64_t subKind) {
				std::vector<std::uint8_t> result;
				for (const TemfBlockDescriptor& descriptor : validated.directory.blocks) {
					if (descriptor.streamKind != static_cast<std::uint16_t>(TemfStreamKind::Metadata)
						|| descriptor.attributeMask != subKind) continue;
					const auto decoded = TerraEngine::Resource::DecodeTemfProductionBlock({
						descriptor,
						std::span<const std::uint8_t>(bytes).subspan(
							static_cast<std::size_t>(descriptor.encodedPayloadOffset),
							static_cast<std::size_t>(descriptor.encodedBytes)),
						validated.directory.quantizationDomains, 0u }, {});
					if (!decoded.IsComplete()) return std::vector<std::uint8_t>{};
					result.insert(result.end(), decoded.bytes.begin(), decoded.bytes.end());
				}
				return result;
			};
			const std::vector<std::uint8_t> directoryBytes = DecodeMetadata(
				kTemfMetadataDeformableDemandedPageDirectory);
			const std::vector<std::uint8_t> entryBytes = DecodeMetadata(
				kTemfMetadataDeformableDemandedPageEntries);
			const std::vector<std::uint8_t> rootBytes = DecodeMetadata(
				kTemfMetadataDeformableDemandedPageRoots);
			const std::vector<std::uint8_t> dependencyBytes = DecodeMetadata(
				kTemfMetadataDeformableDemandedPageDependencies);
			const std::vector<std::uint8_t> blobRecordBytes = DecodeMetadata(
				kTemfMetadataDeformableDemandedPageBlobs);
			TemfDeformableDemandedPageDirectory directory{};
			if (directoryBytes.size() == sizeof(directory)) {
				std::memcpy(&directory, directoryBytes.data(), sizeof(directory));
			}
			context.Expect(directory.version == 1u && directory.entryCount != 0u
				&& directory.rootCount != 0u && directory.blobCount == directory.entryCount,
				"The typed demanded-page directory must declare entries, roots, and one blob per page.");
			context.Expect(entryBytes.size()
				== static_cast<std::size_t>(directory.entryCount)
					* sizeof(TemfDeformableDemandedPageEntry)
				&& rootBytes.size() == static_cast<std::size_t>(directory.rootCount)
					* sizeof(TemfDeformableDemandedPageRoot)
				&& dependencyBytes.size() == static_cast<std::size_t>(directory.dependencyCount)
					* sizeof(TemfDeformableDemandedPageDependency)
				&& blobRecordBytes.size() == static_cast<std::size_t>(directory.blobCount)
					* sizeof(TemfDeformableDemandedPageBlob),
				"Every demanded-page metadata collection must decode independently to its declared typed count.");

			std::vector<TemfDeformableDemandedPageBlob> blobRecords(directory.blobCount);
			if (blobRecordBytes.size() == blobRecords.size() * sizeof(TemfDeformableDemandedPageBlob)) {
				std::memcpy(blobRecords.data(), blobRecordBytes.data(), blobRecordBytes.size());
			}
			std::size_t decodedBlobCount = 0u;
			for (const TemfBlockDescriptor& descriptor : validated.directory.blocks) {
				if (descriptor.streamKind != static_cast<std::uint16_t>(TemfStreamKind::ProceduralUpdate)
					|| descriptor.attributeMask != kTemfMetadataDeformableDemandedPageBlobs) continue;
				const auto decoded = TerraEngine::Resource::DecodeTemfProductionBlock({
					descriptor,
					std::span<const std::uint8_t>(bytes).subspan(
						static_cast<std::size_t>(descriptor.encodedPayloadOffset),
						static_cast<std::size_t>(descriptor.encodedBytes)),
					validated.directory.quantizationDomains, 0u }, {});
				context.Expect(decoded.IsComplete()
					&& decoded.bytes.size() >= sizeof(TemfDeformableDemandedPagePayload),
					"Each demanded page blob must decode through its own production descriptor.");
				if (!decoded.IsComplete() || decodedBlobCount >= blobRecords.size()) continue;
				TemfDeformableDemandedPagePayload blobHeader{};
				std::memcpy(&blobHeader, decoded.bytes.data(), sizeof(blobHeader));
				std::uint32_t sectionMask = 0u;
				if (blobHeader.sectionTableOffset == sizeof(blobHeader)
					&& blobHeader.decodedByteSize == decoded.bytes.size()
					&& static_cast<std::size_t>(blobHeader.sectionCount)
						<= (decoded.bytes.size() - blobHeader.sectionTableOffset)
							/ sizeof(TemfDeformablePageSection))
				{
					for (std::uint32_t sectionIndex = 0u; sectionIndex < blobHeader.sectionCount; ++sectionIndex)
					{
						TemfDeformablePageSection section{};
						std::memcpy(&section, decoded.bytes.data() + blobHeader.sectionTableOffset
							+ static_cast<std::size_t>(sectionIndex) * sizeof(section), sizeof(section));
						sectionMask |= 1u << static_cast<std::uint32_t>(section.kind);
					}
				}
				const std::uint32_t expectedSectionMask =
					((1u << 13u) - 1u)
					& ~((1u << 8u) | (1u << 9u));
				context.Expect(blobHeader.pageId == blobRecords[decodedBlobCount].pageId
					&& sectionMask == expectedSectionMask
					&& descriptor.decodedIntegrity
						== blobRecords[decodedBlobCount].decodedIntegrity,
					"Independent blob decode must retain identity, integrity, and every runtime deformation section.");
				++decodedBlobCount;
			}
			context.ExpectEqual(static_cast<std::size_t>(directory.blobCount), decodedBlobCount,
				"The canonical directory must publish every demanded blob as an independent stream.");
			const TerraEngine::UUID loadedId = ResourceManager::Instance().LoadDeformableMesh(
				outputPath, 4096U, kTestSkeletonCompatibilityFingerprint);
			const Mesh* loaded = ResourceManager::Instance().GetMesh(loadedId);
			context.Expect(loaded != nullptr
				&& loaded->deformableDemandedPageSections.size()
					== loaded->deformableDemandedPagePayloads.size()
				&& std::all_of(loaded->deformableDemandedPageSections.begin(),
					loaded->deformableDemandedPageSections.end(), [](const auto& sections) {
						return sections.size() == 11u;
					}),
				"The canonical loader must retain every validated page-local deformation section for Vulkan consumption.");
			// Corrupt an encoded descriptor-owned blob rather than a retired appended
			// offset.  Integrity validation must fail before a Mesh can be published.
			const auto blobBlock = std::find_if(validated.directory.blocks.begin(),
				validated.directory.blocks.end(), [](const TemfBlockDescriptor& descriptor) {
					return descriptor.streamKind == static_cast<std::uint16_t>(TemfStreamKind::ProceduralUpdate)
						&& descriptor.attributeMask == kTemfMetadataDeformableDemandedPageBlobs;
				});
			if (blobBlock != validated.directory.blocks.end()
				&& blobBlock->encodedPayloadOffset < bytes.size())
			{
				std::vector<std::uint8_t> corruptBytes = bytes;
				corruptBytes[static_cast<std::size_t>(blobBlock->encodedPayloadOffset)] ^= 0x5au;
				const std::filesystem::path corruptPath = TestRoot() / "canonical-demanded-pages-corrupt.temf";
				std::ofstream corruptFile(corruptPath, std::ios::binary | std::ios::trunc);
				corruptFile.write(reinterpret_cast<const char*>(corruptBytes.data()),
					static_cast<std::streamsize>(corruptBytes.size()));
				corruptFile.close();
				const AnimationResourceValidationResult corruptValidation = ResourceManager::Instance()
					.ValidateDeformableMeshFile(corruptPath, 4096U, kTestSkeletonCompatibilityFingerprint);
				context.Expect(corruptValidation.status != TerraEngine::Animation::AnimationStatus::Ok
					&& ResourceManager::Instance().LoadDeformableMesh(corruptPath, 4096U,
						kTestSkeletonCompatibilityFingerprint) == TerraEngine::EMPTY_UUID,
					"A malformed canonical demanded-page descriptor must reject atomically before publication.");
			}
		});

		registry.Add("Editor.Animation.SkinnedMeshCook.RejectsInvalidAndMissingJointWeightsAtomically", [](TestContext& context) {
			const std::filesystem::path outputPath = TestRoot() / "invalid-last-known-good.temf";
			const std::string sentinel = "LAST_KNOWN_GOOD_DEFORMABLE_MESH";
			WriteSentinel(outputPath, sentinel);
			SkinnedMeshCookInput invalid = MakeTriangleInput();
			invalid.vertices[0].influences[0].weight = std::numeric_limits<float>::quiet_NaN();
			SkinnedMeshCookReport invalidReport;
			context.Expect(!CompileSkinnedMeshFromRaw(
				"invalid-weight", outputPath, invalid, &invalidReport, MakeBoundCookSettings())
				&& invalidReport.failure == SkinnedMeshCookFailure::InvalidInfluenceWeight
				&& invalidReport.preservedLastKnownGood,
				"Non-finite weights must reject before publication and preserve the prior generation.");
			const std::vector<std::uint8_t> preserved = ReadBytes(outputPath);
			context.Expect(std::string(preserved.begin(), preserved.end()) == sentinel,
				"A rejected cook must leave the last-known-good bytes unchanged.");

			SkinnedMeshCookInput missingJoint = MakeTriangleInput();
			missingJoint.vertices[0].influences = {{8U, 1.0F}};
			SkinnedMeshCookSettings settings = MakeBoundCookSettings(4U);
			SkinnedMeshCookReport missingReport;
			context.Expect(!CompileSkinnedMeshFromRaw(
				"missing-joint", TestRoot() / "missing-joint.temf", missingJoint, &missingReport, settings)
				&& missingReport.failure == SkinnedMeshCookFailure::MissingJoint,
				"An influence outside the source skeleton must fail with an actionable missing-joint category.");

			settings.skeletonCompatibilityFingerprint = 0U;
			SkinnedMeshCookReport incompatibleReport;
			context.Expect(!CompileSkinnedMeshFromRaw(
				"missing-fingerprint", outputPath, MakeTriangleInput(), &incompatibleReport, settings)
				&& incompatibleReport.failure == SkinnedMeshCookFailure::MissingSkeletonCompatibilityFingerprint
				&& incompatibleReport.preservedLastKnownGood
				&& ReadBytes(outputPath) == preserved,
				"A bound mesh without its skeleton fingerprint must reject atomically and preserve the prior generation.");
		});

		registry.Add("Editor.Animation.SkinnedMeshCook.PartitionsMoreThan256JointsDeterministically", [](TestContext& context) {
			const SkinnedMeshCookInput input = MakePartitionInput();
			SkinnedMeshCookSettings settings = MakeBoundCookSettings(300U);
			SkinnedMeshCookReport first;
			SkinnedMeshCookReport second;
			context.Expect(CompileSkinnedMeshFromRaw(
				"palette-first", TestRoot() / "palette-first.temf", input, &first, settings)
				&& CompileSkinnedMeshFromRaw(
					"palette-second", TestRoot() / "palette-second.temf", input, &second, settings),
				"A mesh referencing 258 joints should be partitioned instead of rejected.");
			context.Expect(first.palettePartitions.size() == 2U
				&& PalettesEqual(first.palettePartitions, second.palettePartitions),
				"Palette partitioning must be byte-order independent and reproducible.");
			for(const SkinnedMeshPalettePartition& partition : first.palettePartitions) {
				context.Expect(partition.sourceJointIds.size() <= kTemfMaximumPaletteJoints,
					"No draw palette may exceed the immutable 256-joint limit.");
			}

			const std::filesystem::path limitedPath = TestRoot() / "palette-limited.temf";
			WriteSentinel(limitedPath, "PALETTE_LAST_KNOWN_GOOD");
			settings.maximumPalettePartitions = 1U;
			SkinnedMeshCookReport limited;
			context.Expect(!CompileSkinnedMeshFromRaw("palette-limited", limitedPath, input, &limited, settings)
				&& limited.failure == SkinnedMeshCookFailure::PalettePartitionLimitExceeded
				&& limited.preservedLastKnownGood,
				"An operational partition limit must reject atomically with an actionable category.");
		});

		registry.Add("Editor.Animation.SkinnedMeshCook.WritesMorphsSkinningAndConservativeEnvelope", [](TestContext& context) {
			// This regression now inspects the production directory rather than a
			// retired appended payload.  Each metadata collection is decoded by the
			// same descriptor API used by the runtime loader.
			SkinnedMeshCookInput canonicalInput = MakeTriangleInput();
			SkinnedMeshMorphTarget canonicalMorph;
			canonicalMorph.stableTargetId = 42U;
			canonicalMorph.positionDeltas = {
				std::array<float, 3U>{-0.1F, 0.0F, 0.0F},
				std::array<float, 3U>{0.2F, 0.1F, 0.0F},
				std::array<float, 3U>{0.0F, 0.3F, 0.0F}};
			canonicalMorph.normalDeltas.assign(3U, {0.0F, 0.0F, 0.0F});
			canonicalMorph.tangentDeltas.assign(3U, {0.0F, 0.0F, 0.0F});
			canonicalInput.morphTargets.push_back(canonicalMorph);
			const std::filesystem::path canonicalPath = TestRoot() / "canonical-deformable-metadata.temf";
			context.Expect(CompileSkinnedMeshFromRaw("canonical-deformable-metadata", canonicalPath,
				canonicalInput, nullptr, MakeBoundCookSettings()),
				"A skinned morph source must publish canonical TEMFPRD1 descriptors.");
			const std::vector<std::uint8_t> canonicalBytes = ReadBytes(canonicalPath);
			const auto canonicalDirectory = TerraEngine::Resource::ValidateTemfProductionFile(canonicalBytes, {});
			context.Expect(canonicalDirectory.IsComplete()
				&& (canonicalDirectory.directory.header.representationFlags & kTemfRepresentationDeformableStreams) != 0u,
				"The cooked mesh must advertise descriptor-owned deformable streams.");
			const AnimationResourceValidationResult canonicalValidation = ResourceManager::Instance()
				.ValidateDeformableMeshFile(canonicalPath, 4096U, kTestSkeletonCompatibilityFingerprint);
			context.Expect(canonicalValidation.status == TerraEngine::Animation::AnimationStatus::Ok
				&& canonicalValidation.diagnosticCode == "CanonicalDeformableMeshValid",
				"The runtime validator must accept the canonical deformable root, streams, and demanded pages.");
			const TerraEngine::UUID canonicalMeshId = ResourceManager::Instance().LoadDeformableMesh(
				canonicalPath, 4096U, kTestSkeletonCompatibilityFingerprint);
			const Mesh* canonicalMesh = ResourceManager::Instance().GetMesh(canonicalMeshId);
			context.Expect(canonicalMesh != nullptr && canonicalMesh->hasDeformablePayload
				&& !canonicalMesh->hasVirtualGeometry
				&& canonicalMesh->deformableDemandedPageDirectory.entryCount == 0u
				&& canonicalMesh->deformableDemandedPagePayloads.empty(),
				"Low-poly deformable meshes must retain canonical metadata while avoiding unnecessary demanded pages.");
		});

		registry.Add("Editor.Animation.SkinnedMeshCook.PreservesAdaptiveLodsAndPerMeshPercentageThresholds", [](TestContext& context) {
			const SkinnedMeshCookInput input = MakeSkinnedGridInput(48U);
			const std::filesystem::path defaultPath =
				TestRoot() / "animated-adaptive-lods.temf";
			SkinnedMeshCookSettings defaultSettings =
				MakeBoundCookSettings(1U);
			context.Expect(CompileSkinnedMeshFromRaw(
				"animated-adaptive-lods",
				defaultPath,
				input,
				nullptr,
				defaultSettings),
				"A deformable mesh should publish with the permissive default LOD recipe.");

			ResourceManager& resources = ResourceManager::Instance();
			resources.ClearCaches();
			const TerraEngine::UUID defaultMeshId =
				resources.LoadDeformableMesh(
					defaultPath,
					1U,
					kTestSkeletonCompatibilityFingerprint);
			const Mesh* defaultMesh = resources.GetMesh(defaultMeshId);
			context.Expect(defaultMesh != nullptr
					&& defaultMesh->virtualGeometryDetailLevels.size() > 1U,
				"The final deformable TEMF must retain generated levels beyond LOD0.");
			if(defaultMesh != nullptr) {
				const bool hasCoarserAnimatedPage = std::any_of(
					defaultMesh->virtualGeometryPages.begin(),
					defaultMesh->virtualGeometryPages.end(),
					[defaultMesh](const VirtualGeometryPageRecord& page) {
						if(page.firstLodIndex == 0U) {
							return false;
						}
						return std::any_of(
							defaultMesh->deformableDemandedPageEntries.begin(),
							defaultMesh->deformableDemandedPageEntries.end(),
							[&page](const TemfDeformableDemandedPageEntry& entry) {
								return entry.pageId == page.pageId;
							});
					});
				context.Expect(hasCoarserAnimatedPage,
					"Animated physical-page data must cover at least one generated LOD beyond LOD0.");
			}

			SkinnedMeshCookSettings thresholdSettings =
				MakeBoundCookSettings(1U);
			thresholdSettings.virtualGeometrySettings =
				MakeDefaultVirtualGeometryAdaptiveCompileSettings();
			thresholdSettings.virtualGeometrySettings
				.minimumUsefulTriangleReduction = 0.95F;
			thresholdSettings.virtualGeometrySettings
				.minimumUsefulMeshletReduction = 0.95F;
			thresholdSettings.virtualGeometrySettings.settingsSource =
				"PerMeshOverride";
			const std::filesystem::path thresholdPath =
				TestRoot() / "animated-threshold-override.temf";
			context.Expect(CompileSkinnedMeshFromRaw(
				"animated-threshold-override",
				thresholdPath,
				input,
				nullptr,
				thresholdSettings),
				"A deformable mesh should accept a per-mesh percentage threshold.");
			resources.ClearCaches();
			const TerraEngine::UUID thresholdMeshId =
				resources.LoadDeformableMesh(
					thresholdPath,
					1U,
					kTestSkeletonCompatibilityFingerprint);
			const Mesh* thresholdMesh = resources.GetMesh(thresholdMeshId);
			context.Expect(thresholdMesh != nullptr
					&& thresholdMesh->virtualGeometryDetailLevels.size() == 1U,
				"A strict per-mesh percentage override should still be able to merge lower LOD candidates.");

			SkinnedMeshCookSettings compactSettings =
				MakeBoundCookSettings(1U);
			compactSettings.virtualGeometrySettings =
				MakeDefaultVirtualGeometryAdaptiveCompileSettings();
			compactSettings.virtualGeometrySettings.targetRatios = {
				1.0F, 0.98F, 0.96F, 0.94F, 0.92F, 0.90F
			};
			const std::filesystem::path compactPath =
				TestRoot() / "animated-compacted-lods.temf";
			context.Expect(CompileSkinnedMeshFromRaw(
				"animated-compacted-lods",
				compactPath,
				input,
				nullptr,
				compactSettings),
				"Closely spaced animated LOD requests should publish after regional duplicate compaction.");
			resources.ClearCaches();
			const TerraEngine::UUID compactMeshId =
				resources.LoadDeformableMesh(
					compactPath,
					1U,
					kTestSkeletonCompatibilityFingerprint);
			const Mesh* compactMesh = resources.GetMesh(compactMeshId);
			context.Expect(compactMesh != nullptr,
				"Regional LOD compaction must not leave a skipped target pointing at a removed detail.");
			if(compactMesh != nullptr) {
				const bool skippedFallbacksValid = std::all_of(
					compactMesh->virtualGeometrySkippedDetails.begin(),
					compactMesh->virtualGeometrySkippedDetails.end(),
					[compactMesh](
						const VirtualGeometrySkippedDetailRecord& skipped) {
						return skipped.nearestGeneratedLodIndex
							< compactMesh->virtualGeometryDetailLevels.size();
					});
				context.Expect(skippedFallbacksValid,
					"Every skipped animated target must reference the compact emitted LOD table.");
			}
		});

		registry.Add("Editor.Animation.SkinnedMeshCook.CanonicalizesDeindexedTopologyWithoutMergingDifferentSkinning", [](TestContext& context) {
			constexpr std::uint32_t kQuadsPerSide = 24U;
			SkinnedMeshCookInput input =
				MakeDeindexedSkinnedGridInput(kQuadsPerSide);

			// Give one otherwise identical interior corner a different joint.
			// A deformation-safe canonicalizer must preserve that record while
			// merging all other byte-identical triangle corners.
			const std::array<float, 3U> interiorPosition =
				input.vertices[2U].baseVertex.position;
			std::size_t changedVertex = input.vertices.size();
			for(std::size_t index = 3U; index < input.vertices.size(); ++index) {
				if(input.vertices[index].baseVertex.position == interiorPosition) {
					changedVertex = index;
					break;
				}
			}
			context.Expect(changedVertex < input.vertices.size(),
				"The deindexed regression fixture must contain a repeated interior corner.");
			if(changedVertex < input.vertices.size()) {
				input.vertices[changedVertex].influences = {{1U, 1.0F}};
			}

			const std::filesystem::path outputPath =
				TestRoot() / "animated-deindexed-topology.temf";
			SkinnedMeshCookReport topologyReport;
			context.Expect(CompileSkinnedMeshFromRaw(
				"animated-deindexed-topology",
				outputPath,
				input,
				&topologyReport,
				MakeBoundCookSettings(2U)),
				"An unindexed deformable mesh should cook after safe deformation-aware canonicalization.");

			const std::uint32_t gridVertexCount =
				(kQuadsPerSide + 1U) * (kQuadsPerSide + 1U);
			context.Expect(topologyReport.cookedVertices.size() == gridVertexCount + 1U,
				"Equivalent corners should weld, while the corner with different skinning must remain distinct.");

			ResourceManager& resources = ResourceManager::Instance();
			resources.ClearCaches();
			const TerraEngine::UUID meshId =
				resources.LoadDeformableMesh(
					outputPath,
					2U,
					kTestSkeletonCompatibilityFingerprint);
			const Mesh* mesh = resources.GetMesh(meshId);
			// This 1,152-triangle fixture is intentionally below the canonical
			// paged-VG threshold. It validates deformation-safe welding through the
			// direct route, so no multi-LOD virtual hierarchy is required here.
			context.Expect(mesh != nullptr,
				"Deindexed animated geometry must remain loadable after deformation-safe canonicalization.");
		});
	}
}
