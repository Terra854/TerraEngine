// Real production importer derives skeleton identity; no copied fingerprint hash.
#include "AnimatedConsumerCompanion.h"
#include "Editor/AssetCompilers/GltfPackageImporter.h"
#include "Editor/AssetCompilers/AnimationClipCompiler.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <span>
#include <vector>
#include <nlohmann/json.hpp>

namespace TerraEngine::Tests::Review {
	namespace {
		using namespace Animation;
		template<typename T> std::size_t Append(std::vector<std::uint8_t>& bytes, std::span<const T> values) {
			const auto offset = bytes.size(); const auto source = std::as_bytes(values);
			bytes.resize(offset + source.size()); std::memcpy(bytes.data() + offset, source.data(), source.size()); return offset;
		}
		bool Write(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
			std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); return static_cast<bool>(file);
		}
		bool CreateImportSource(const std::filesystem::path& sourceRoot, std::string& diagnostic) {
			std::filesystem::create_directories(sourceRoot);
			// The actual package importer requires renderable geometry. This tiny
			// static triangle admits its real one-joint skin without duplicating the
			// dense skinned mesh that Sol1 cooks separately with the returned identity.
			const std::array<float, 9U> positions{-1.0F, -1.0F, 0.0F, 1.0F, -1.0F, 0.0F, 0.0F, 1.0F, 0.0F};
			const std::array<float, 9U> normals{0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F};
			const std::array<float, 6U> uv{0.0F, 0.0F, 1.0F, 0.0F, 0.5F, 1.0F};
			const std::array<std::uint16_t, 3U> indices{0U, 1U, 2U};
			std::vector<std::uint8_t> bytes;
			const auto positionOffset = Append(bytes, std::span<const float>(positions));
			const auto normalOffset = Append(bytes, std::span<const float>(normals));
			const auto uvOffset = Append(bytes, std::span<const float>(uv));
			const auto indexOffset = Append(bytes, std::span<const std::uint16_t>(indices));
			const auto logicalByteSize = bytes.size();
			// Buffer views preserve their exact extents; tail padding is owned data.
			bytes.resize((bytes.size() + 3U) & ~std::size_t{3U}, 0U);
			nlohmann::json document;
			document["asset"] = {{"version", "2.0"}, {"generator", "TerraEngine AnimatedConsumerCompanion"}};
			document["scene"] = 0U;
			document["scenes"] = nlohmann::json::array({{{"nodes", {0U}}}});
			document["nodes"] = nlohmann::json::array({{{"name", "ReviewRoot"}, {"children", {1U}}}, {{"name", "ReviewTriangle"}, {"mesh", 0U}}});
			document["skins"] = nlohmann::json::array({{{"name", "ReviewSkin"}, {"skeleton", 0U}, {"joints", {0U}}}});
			document["buffers"] = nlohmann::json::array({{{"uri", "AnimatedConsumerCompanion.bin"}, {"byteLength", bytes.size()}}});
			document["bufferViews"] = nlohmann::json::array({
				{{"buffer", 0U}, {"byteOffset", positionOffset}, {"byteLength", sizeof(positions)}, {"target", 34962U}},
				{{"buffer", 0U}, {"byteOffset", normalOffset}, {"byteLength", sizeof(normals)}, {"target", 34962U}},
				{{"buffer", 0U}, {"byteOffset", uvOffset}, {"byteLength", sizeof(uv)}, {"target", 34962U}},
				{{"buffer", 0U}, {"byteOffset", indexOffset}, {"byteLength", sizeof(indices)}, {"target", 34963U}}});
			document["accessors"] = nlohmann::json::array({
				{{"bufferView", 0U}, {"componentType", 5126U}, {"count", 3U}, {"type", "VEC3"}, {"min", {-1.0, -1.0, 0.0}}, {"max", {1.0, 1.0, 0.0}}},
				{{"bufferView", 1U}, {"componentType", 5126U}, {"count", 3U}, {"type", "VEC3"}},
				{{"bufferView", 2U}, {"componentType", 5126U}, {"count", 3U}, {"type", "VEC2"}},
				{{"bufferView", 3U}, {"componentType", 5123U}, {"count", 3U}, {"type", "SCALAR"}}});
			document["materials"] = nlohmann::json::array({{{"name", "ReviewWhite"}, {"pbrMetallicRoughness", {{"baseColorFactor", {1.0, 1.0, 1.0, 1.0}}, {"metallicFactor", 0.0}, {"roughnessFactor", 1.0}}}}});
			document["meshes"] = nlohmann::json::array({{{"name", "ReviewTriangle"}, {"primitives", nlohmann::json::array({{{"attributes", {{"POSITION", 0U}, {"NORMAL", 1U}, {"TEXCOORD_0", 2U}}}, {"indices", 3U}, {"material", 0U}, {"mode", 4U}}})}}});
			std::ofstream source(sourceRoot / "AnimatedConsumerCompanion.gltf"); source << document.dump(2); source.close();
			if(!source || logicalByteSize != 102U || !Write(sourceRoot / "AnimatedConsumerCompanion.bin", bytes)) { diagnostic = "Could not persist the bounded real glTF companion input."; return false; }
			return true;
		}
	}
	Animation::AnimationStatus BakeAnimatedConsumerCompanion(const std::filesystem::path& outputRoot,
		AnimatedConsumerCompanion& output, std::string& diagnostic) {
		using namespace Animation; using namespace AssetPipeline;
		try {
			if(outputRoot.empty()) { diagnostic = "Companion outputRoot is required."; return AnimationStatus::InvalidArgument; }
			std::filesystem::create_directories(outputRoot);
			const auto sourceRoot = outputRoot / ".companion-source";
			if(!CreateImportSource(sourceRoot, diagnostic)) return AnimationStatus::InternalError;
			GltfPackageImportOptions options; options.sourcePath = sourceRoot / "AnimatedConsumerCompanion.gltf";
			options.assetRoot = outputRoot / ".companion-generated"; options.buildRoot = options.assetRoot;
			options.sourceAssetRoot = sourceRoot;
			// This logical spelling and package identity never contain an absolute
			// bake directory, so real importer-derived hashes match independent bakes.
			options.sourceRelativePath = "Review/AnimatedConsumerCompanion.gltf";
			options.packageGuid[0U] = 0x52U; options.packageGuid[15U] = 0x61U;
			options.forceReimport = true;
			const auto imported = ImportGltfPackageWithAnimation(options);
			const auto failed = std::any_of(imported.package.diagnostics.begin(), imported.package.diagnostics.end(), [](const auto& row) { return row.severity == GltfDiagnosticSeverity::Error; });
			if(imported.package.status == GltfImportStatus::Failed || failed || !imported.animation.publishedAtomically
				|| imported.animation.skeletons.size() != 1U || !imported.animation.animationClips.empty()) {
				diagnostic = "Actual companion importer must publish exactly one skeleton without errors. Status=" + std::to_string(static_cast<int>(imported.package.status));
				for(const auto& row : imported.package.diagnostics) if(row.severity == GltfDiagnosticSeverity::Error) diagnostic += " " + row.code + ": " + row.message;
				return AnimationStatus::InvalidArgument;
			}
			AnimatedConsumerCompanion staged; staged.skeleton = imported.animation.skeletons.front().asset;
			if(!staged.skeleton.assetId.IsValid() || staged.skeleton.compatibilityFingerprint == 0U || staged.skeleton.referencePoseHash == 0U
				|| staged.skeleton.joints.size() != 1U || staged.skeleton.rootJointIndex != 0U || staged.skeleton.joints.front().parentIndex != -1
				|| !staged.skeleton.joints.front().deformsMesh || staged.skeleton.joints.front().stableId == 0U) {
				diagnostic = "The real imported companion must be a nonempty coherent one-joint deforming skeleton."; return AnimationStatus::IncompatibleAsset;
			}
			AnimationClipCompileInput input; input.assetId.value[0U] = 0x52U; input.assetId.value[15U] = 0x62U;
			input.skeletonAssetId = staged.skeleton.assetId; input.compatibilityFingerprint = staged.skeleton.compatibilityFingerprint;
			input.durationTicks = 60; input.segmentDurationTicks = 60;
			const auto rootJoint = staged.skeleton.joints.front().stableId;
			for(const auto kind : {AnimationClipTrackKind::Translation, AnimationClipTrackKind::Rotation, AnimationClipTrackKind::Scale, AnimationClipTrackKind::MorphWeight}) {
				AnimationSourceTrack track; track.stableTargetId = kind == AnimationClipTrackKind::MorphWeight ? 1U : rootJoint; track.kind = kind;
				AnimationSourceKey key;
				if(kind == AnimationClipTrackKind::Translation) key.vectorValue = {0.125F, 0.25F, 0.5F};
				if(kind == AnimationClipTrackKind::Scale) key.vectorValue = {1.0F, 1.0F, 1.0F};
				if(kind == AnimationClipTrackKind::MorphWeight) key.scalarValue = 0.25F;
				track.keys.push_back(key); key.tick = 60; track.keys.push_back(key); input.tracks.push_back(std::move(track));
			}
			input.namedChannels.push_back({1U, "ReviewMorph"});
			AnimationClipCompileReport report; const auto compiled = CompileAnimationClip(input, {}, staged.clip, report);
			if(compiled != AnimationStatus::Ok) { diagnostic = "Actual companion clip compiler rejected the input: " + report.diagnostic; return compiled; }
			std::vector<std::uint8_t> skeletonBytes, clipBytes;
			const auto skeletonStatus = SerializeSkeletonAsset(staged.skeleton, skeletonBytes);
			const auto clipStatus = SerializeAnimationClipAsset(staged.clip, clipBytes);
			if(skeletonStatus != AnimationStatus::Ok || clipStatus != AnimationStatus::Ok || skeletonBytes.empty() || clipBytes.empty()) {
				diagnostic = "Actual companion production serialization failed."; return AnimationStatus::InvalidArgument;
			}
			SkeletonAsset decodedSkeleton; AnimationClipAsset decodedClip;
			if(DeserializeSkeletonAsset(skeletonBytes, staged.skeleton.compatibilityFingerprint, decodedSkeleton) != AnimationStatus::Ok
				|| DeserializeAnimationClipAsset(clipBytes, staged.skeleton.compatibilityFingerprint, decodedClip) != AnimationStatus::Ok
				|| decodedSkeleton != staged.skeleton || decodedClip != staged.clip || decodedClip.skeletonAssetId != decodedSkeleton.assetId
				|| decodedClip.compatibilityFingerprint != decodedSkeleton.compatibilityFingerprint) {
				diagnostic = "Actual companion codec readback changed identity or asset content."; return AnimationStatus::ContentHashMismatch;
			}
			staged.skeletonPath = "TerraEngineProductionShapeFixture.teskel"; staged.clipPath = "TerraEngineProductionShapeFixture.teanim";
			if(!Write(outputRoot / staged.skeletonPath, skeletonBytes) || !Write(outputRoot / staged.clipPath, clipBytes)) {
				diagnostic = "Could not persist real cooked companion outputs."; return AnimationStatus::InternalError;
			}
			output = std::move(staged); diagnostic = "Actual one-joint imported skeleton and compiled TRS/morph clip validated and persisted."; return AnimationStatus::Ok;
		} catch(const std::exception& error) { diagnostic = error.what(); return AnimationStatus::InternalError; }
	}
}
