// SkinnedMeshCookTests.cpp - Deformable TEMF cooking and publication contracts.

#include "TestHarness.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "MeshCompiler.h"
#include "Resource/ResourceManager.h"

#include <cmath>
#include <cstdint>
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
				&& report.cookedVertices[0].influences.size() == kDeformableMeshMaximumInfluences,
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
				context.Expect(partition.sourceJointIds.size() <= kDeformableMeshMaximumPaletteJoints,
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
			SkinnedMeshCookInput input = MakeTriangleInput();
			SkinnedMeshMorphTarget morph;
			morph.stableTargetId = 42U;
			morph.positionDeltas = {
				std::array<float, 3U>{-0.1F, 0.0F, 0.0F},
				std::array<float, 3U>{0.2F, 0.1F, 0.0F},
				std::array<float, 3U>{0.0F, 0.3F, 0.0F}
			};
			morph.normalDeltas = {
				std::array<float, 3U>{0.0F, 0.1F, 0.0F},
				std::array<float, 3U>{0.0F, 0.0F, 0.2F},
				std::array<float, 3U>{0.1F, 0.0F, 0.0F}
			};
			morph.tangentDeltas = {
				std::array<float, 3U>{0.0F, 0.0F, 0.1F},
				std::array<float, 3U>{0.2F, 0.0F, 0.0F},
				std::array<float, 3U>{0.0F, 0.2F, 0.0F}
			};
			input.morphTargets.push_back(morph);
			const std::filesystem::path outputPath = TestRoot() / "deformable-payload.temf";
			SkinnedMeshCookReport report;
			context.Expect(CompileSkinnedMeshFromRaw(
				"deformable-payload", outputPath, input, &report, MakeBoundCookSettings()),
				"A valid morph and skin payload should publish.");
			context.Expect(report.deformableTemf.hasSkinning
				&& report.deformableTemf.morphTargetCount == 1U
				&& report.deformableTemf.palettePartitionCount == report.palettePartitions.size()
				&& report.deformableTemf.deformationEnvelopeValid,
				"The cook report must expose skinning, morph, palette, and conservative-bound metadata.");

			std::ifstream file(outputPath, std::ios::binary);
			MeshHeader meshHeader{};
			file.read(reinterpret_cast<char*>(&meshHeader), sizeof(meshHeader));
			context.Expect(file && meshHeader.meshletSkinningDataCount == input.vertices.size()
				&& meshHeader.meshletSkinningDataOffset != 0U
				&& meshHeader.virtualGeometryReserved != 0U,
				"The compatible TEMF header must point at both skinning bytes and the deformable extension.");
			DeformableMeshPayloadHeader deformable{};
			file.seekg(static_cast<std::streamoff>(meshHeader.virtualGeometryReserved), std::ios::beg);
			file.read(reinterpret_cast<char*>(&deformable), sizeof(deformable));
			context.Expect(file && deformable.magic == kDeformableMeshPayloadMagicValue
				&& deformable.version == kDeformableMeshPayloadVersion
				&& deformable.skinningVertexCount == input.vertices.size()
				&& deformable.morphTargetCount == 1U && deformable.palettePartitionCount > 0U
				&& deformable.skeletonCompatibilityFingerprint == kTestSkeletonCompatibilityFingerprint
				&& report.deformableTemf.skeletonCompatibilityValidated,
				"The appended deformable payload must use checked versioned counts and tables.");
			const std::uint64_t sidecarOffset = static_cast<std::uint64_t>(meshHeader.virtualGeometryReserved)
				+ deformable.payloadSize;
			DeformableVirtualGeometrySidecarHeader sidecar{};
			file.seekg(static_cast<std::streamoff>(sidecarOffset), std::ios::beg);
			file.read(reinterpret_cast<char*>(&sidecar), sizeof(sidecar));
			context.Expect(file && sidecar.magic == kDeformableVirtualGeometrySidecarMagicValue
				&& sidecar.version == kDeformableVirtualGeometrySidecarVersion
				&& sidecar.clusterCount > 0U
				&& sidecar.morphTargetCount == 1U
				&& sidecar.morphVertexStrideBytes == sizeof(DeformableVirtualGeometryMorphVertex),
				"The versioned TDFV sidecar must describe deterministic cluster coverage and fixed-stride GPU morph records.");
			DeformableVirtualGeometryMorphRecord gpuMorph{};
			file.seekg(static_cast<std::streamoff>(sidecar.morphTableOffset), std::ios::beg);
			file.read(reinterpret_cast<char*>(&gpuMorph), sizeof(gpuMorph));
			DeformableVirtualGeometryMorphVertex gpuVertex{};
			file.seekg(static_cast<std::streamoff>(gpuMorph.vertexDataOffset), std::ios::beg);
			file.read(reinterpret_cast<char*>(&gpuVertex), sizeof(gpuVertex));
			context.Expect(file
				&& gpuMorph.flags == (kDeformableVirtualGeometryMorphFlagPosition
					| kDeformableVirtualGeometryMorphFlagNormal
					| kDeformableVirtualGeometryMorphFlagTangent)
				&& gpuVertex.positionDelta[0] == -0.1F
				&& gpuVertex.normalDelta[1] == 0.1F
				&& gpuVertex.tangentDelta[2] == 0.1F
				&& gpuVertex.positionDelta[3] == 0.0F,
				"TDFV morph records must preserve target-major position, normal, and tangent deltas with zero float4 padding.");
			context.Expect(std::isfinite(deformable.deformationBoundsMin[0])
				&& std::isfinite(deformable.deformationBoundsMax[0])
				&& deformable.deformationBoundsMin[0] <= -0.1F
				&& deformable.deformationBoundsMax[0] >= 1.2F,
				"The conservative deformation envelope must include base geometry plus morph displacement.");
			const AnimationResourceValidationResult validRuntimeContract = ResourceManager::Instance()
				.ValidateDeformableMeshFile(outputPath, 4096U, kTestSkeletonCompatibilityFingerprint);
			context.Expect(validRuntimeContract.status == TerraEngine::Animation::AnimationStatus::Ok
				&& validRuntimeContract.compatibilityFingerprintPresent
				&& validRuntimeContract.actualCompatibilityFingerprint == kTestSkeletonCompatibilityFingerprint,
				"The source-independent runtime validator must accept the exact cooked skeleton fingerprint.");
			const AnimationResourceValidationResult incompatibleRuntimeContract = ResourceManager::Instance()
				.ValidateDeformableMeshFile(outputPath, 4096U, kTestSkeletonCompatibilityFingerprint + 1U);
			context.Expect(incompatibleRuntimeContract.status
					== TerraEngine::Animation::AnimationStatus::IncompatibleAsset
				&& incompatibleRuntimeContract.diagnosticCode == "DeformableSkeletonFingerprintMismatch",
				"The runtime validator must reject a mesh bound to a different skeleton generation.");

			// Validation alone is not enough for rendering. The in-memory mesh must
			// retain the checked compatibility and palette facts so a render binding
			// cannot accidentally pair this payload with another skeleton generation.
			const TerraEngine::UUID loadedMeshId = ResourceManager::Instance().LoadDeformableMesh(
				outputPath, 4096U, kTestSkeletonCompatibilityFingerprint);
			const Mesh* loadedMesh = ResourceManager::Instance().GetMesh(loadedMeshId);
			context.Expect(loadedMeshId != TerraEngine::EMPTY_UUID && loadedMesh != nullptr,
				"The source-independent deformable loader must publish the validated runtime mesh.");
			if(loadedMesh != nullptr) {
				context.Expect(loadedMesh->hasDeformablePayload
					&& loadedMesh->deformableMetadata.skeletonCompatibilityFingerprint
						== kTestSkeletonCompatibilityFingerprint
					&& loadedMesh->deformablePalettes.size() == deformable.palettePartitionCount
					&& loadedMesh->deformableMorphs.size() == deformable.morphTargetCount
					&& loadedMesh->deformableVertexPaletteIndices.size() == input.vertices.size(),
					"A loaded deformable mesh must retain generation-safe skeleton, palette, morph, and vertex-binding metadata.");
				context.Expect(loadedMesh->hasDeformableVirtualGeometrySidecar
					&& loadedMesh->deformableVirtualGeometryClusters.size() == sidecar.clusterCount
					&& loadedMesh->deformableVirtualGeometryMorphs.size() == 1U
					&& loadedMesh->deformableVirtualGeometryMorphVertices.size() == input.vertices.size(),
					"The loader must copy validated cluster coverage and 48-byte GPU morph records atomically into the immutable mesh.");
			}

			const std::filesystem::path corruptPath = TestRoot() / "deformable-sidecar-corrupt.temf";
			std::filesystem::copy_file(outputPath, corruptPath, std::filesystem::copy_options::overwrite_existing);
			std::fstream corrupt(corruptPath, std::ios::binary | std::ios::in | std::ios::out);
			const std::uint32_t invalidSidecarMagic = 0U;
			corrupt.seekp(static_cast<std::streamoff>(sidecarOffset), std::ios::beg);
			corrupt.write(reinterpret_cast<const char*>(&invalidSidecarMagic), sizeof(invalidSidecarMagic));
			corrupt.close();
			const AnimationResourceValidationResult corruptContract = ResourceManager::Instance()
				.ValidateDeformableMeshFile(corruptPath, 4096U, kTestSkeletonCompatibilityFingerprint);
			context.Expect(corruptContract.status != TerraEngine::Animation::AnimationStatus::Ok
				&& ResourceManager::Instance().LoadDeformableMesh(corruptPath, 4096U, kTestSkeletonCompatibilityFingerprint)
					== TerraEngine::EMPTY_UUID,
				"Malformed TDFV sidecar metadata must reject before any partial deformable Mesh state is published.");

			SkinnedMeshCookInput positionOnly = MakeTriangleInput();
			positionOnly.morphTargets.push_back({43U, {
				std::array<float, 3U>{0.1F, 0.0F, 0.0F},
				std::array<float, 3U>{0.0F, 0.1F, 0.0F},
				std::array<float, 3U>{0.0F, 0.0F, 0.1F}
			}});
			const std::filesystem::path positionOnlyPath = TestRoot() / "deformable-position-only-morph.temf";
			context.Expect(CompileSkinnedMeshFromRaw(
				"position-only-morph", positionOnlyPath, positionOnly, nullptr, MakeBoundCookSettings()),
				"A frozen position-only morph source must remain cookable through the versioned TDFV extension.");
			std::ifstream positionOnlyFile(positionOnlyPath, std::ios::binary);
			MeshHeader positionOnlyMeshHeader{};
			DeformableMeshPayloadHeader positionOnlyTdef{};
			DeformableVirtualGeometrySidecarHeader positionOnlySidecar{};
			DeformableVirtualGeometryMorphRecord positionOnlyMorph{};
			DeformableVirtualGeometryMorphVertex positionOnlyVertex{};
			positionOnlyFile.read(reinterpret_cast<char*>(&positionOnlyMeshHeader), sizeof(positionOnlyMeshHeader));
			positionOnlyFile.seekg(static_cast<std::streamoff>(positionOnlyMeshHeader.virtualGeometryReserved), std::ios::beg);
			positionOnlyFile.read(reinterpret_cast<char*>(&positionOnlyTdef), sizeof(positionOnlyTdef));
			positionOnlyFile.seekg(static_cast<std::streamoff>(positionOnlyMeshHeader.virtualGeometryReserved + positionOnlyTdef.payloadSize), std::ios::beg);
			positionOnlyFile.read(reinterpret_cast<char*>(&positionOnlySidecar), sizeof(positionOnlySidecar));
			positionOnlyFile.seekg(static_cast<std::streamoff>(positionOnlySidecar.morphTableOffset), std::ios::beg);
			positionOnlyFile.read(reinterpret_cast<char*>(&positionOnlyMorph), sizeof(positionOnlyMorph));
			positionOnlyFile.seekg(static_cast<std::streamoff>(positionOnlyMorph.vertexDataOffset), std::ios::beg);
			positionOnlyFile.read(reinterpret_cast<char*>(&positionOnlyVertex), sizeof(positionOnlyVertex));
			context.Expect(positionOnlyFile
				&& positionOnlyMorph.flags == kDeformableVirtualGeometryMorphFlagPosition
				&& positionOnlyVertex.normalDelta[0] == 0.0F
				&& positionOnlyVertex.tangentDelta[0] == 0.0F,
				"Absent frozen-v1 normal/tangent channels must round-trip as zero-filled TDFV GPU attributes.");

			for(SkinnedMeshCookVertex& vertex : input.vertices) {
				vertex.influences.clear();
			}
			SkinnedMeshCookSettings morphOnlySettings;
			morphOnlySettings.sourceSkeletonJointCount = 1U;
			SkinnedMeshCookReport morphOnlyReport;
			context.Expect(CompileSkinnedMeshFromRaw(
				"morph-only", TestRoot() / "morph-only.temf", input, &morphOnlyReport, morphOnlySettings)
				&& morphOnlyReport.palettePartitions.size() == 1U
				&& morphOnlyReport.palettePartitions.front().sourceJointIds
					== std::vector<std::uint32_t>{0U}
				&& morphOnlyReport.skeletonCompatibilityFingerprint == 0U,
				"A morph-only mesh must use a nonempty identity palette while remaining unbound to a skeleton fingerprint.");
		});
	}
}
