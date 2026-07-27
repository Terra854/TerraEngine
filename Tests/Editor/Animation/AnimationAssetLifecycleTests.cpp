// AnimationAssetLifecycleTests.cpp - typed browser lifecycle and worker boundary tests.

#include "TestHarness.h"

#include "Editor/AssetCompilers/AnimationClipCompiler.h"
#include "Editor/TerraEngine-Editor/Assets/AnimationAssetActions.h"
#include "Engine/Animation/Assets/AnimationClipAsset.h"
#include "Engine/Animation/Assets/AnimationControllerAsset.h"
#include "Engine/Animation/Assets/SkeletonAsset.h"
#include "Engine/Utilities/UUID.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace TerraEngine::EditorTests {
	// Keep authoring status references distinct from the engine asset namespace
	// now that this behavioral test also invokes the production controller decoder.
	namespace Animation = TerraEngine::Editor::Animation;

	void RegisterAnimationAssetLifecycleTests(TestRegistry& registry) {
		registry.Add("Editor.Animation.AssetLifecycle.CreateImportInspectAllKinds", [](TestContext& context) {
			using namespace TerraEngine::Editor;
			const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
			const std::filesystem::path workspace = std::filesystem::temp_directory_path()
				/ ("TerraEngineAnimationAllStarters-" + std::to_string(unique));
			Assets::AnimationAssetActions actions(workspace / "Assets");
			constexpr std::array kinds{
				Assets::AnimationAssetKind::Skeleton, Assets::AnimationAssetKind::Clip,
				Assets::AnimationAssetKind::Controller, Assets::AnimationAssetKind::Mask,
				Assets::AnimationAssetKind::RetargetProfile, Assets::AnimationAssetKind::MotionDatabase,
				Assets::AnimationAssetKind::RagdollProfile};
			context.Expect(actions.SupportedKinds().size() == kinds.size(),
				"Every installed animation authoring type must expose one browser action.");
			for(std::size_t index = 0U; index < kinds.size(); ++index) {
				Assets::AnimationAssetEntry created;
				Assets::AnimationAssetValidationRequest request;
				const std::string name = "Asset" + std::to_string(index);
				context.Expect(actions.Create(kinds[index], name, "Animations", created)
					== Animation::AnimationAuthoringStatus::Ok
					&& actions.Inspect(created.id) != nullptr
					&& !created.valid
					&& std::filesystem::is_regular_file(created.sourcePath)
					&& actions.BeginValidation(created.id, request) == Animation::AnimationAuthoringStatus::Ok
					&& std::string(Assets::AnimationAssetActions::Extension(kinds[index])).size() > 1U,
					"Every advertised creation action must retain a typed, inspectable entry.");
				const Assets::AnimationAssetValidationResult validation =
					Assets::AnimationAssetActions::ValidateCandidate(request);
				context.Expect(validation.succeeded
					&& actions.CompleteValidation(validation) == Animation::AnimationAuthoringStatus::Ok,
					std::string{"Every starter must round-trip through its production decoder before publication: "}
						+ Assets::AnimationAssetActions::DisplayName(kinds[index]) + " ("
						+ validation.diagnostic + ").");
			}
			std::error_code error;
			std::filesystem::remove_all(workspace, error);
		});

		registry.Add("Editor.Animation.AssetLifecycle.InverseMoveRenameDuplicateDelete", [](TestContext& context) {
			using namespace TerraEngine::Editor;
			Assets::AnimationAssetActions actions;
			Assets::AnimationAssetEntry clip;
			context.Expect(actions.Create(Assets::AnimationAssetKind::Clip, "Walk", "Animations", clip)
				== Animation::AnimationAuthoringStatus::Ok,
				"A clip creation action must be discoverable.");
			Assets::AnimationAssetEntry copy;
			context.Expect(actions.Duplicate(clip.id, "Walk Copy", copy)
				== Animation::AnimationAuthoringStatus::Ok
				&& copy.kind == clip.kind,
				"Duplication must retain the typed asset kind.");
			context.Expect(actions.Move(copy.id, "Characters") == Animation::AnimationAuthoringStatus::Ok
				&& actions.Rename(copy.id, "Run") == Animation::AnimationAuthoringStatus::Ok
				&& actions.Inspect(copy.id)->folder == "Characters"
				&& actions.Inspect(copy.id)->name == "Run",
				"Move and rename must change only the selected asset.");
			context.Expect(actions.Undo() == Animation::AnimationAuthoringStatus::Ok
				&& actions.Undo() == Animation::AnimationAuthoringStatus::Ok
				&& actions.Inspect(copy.id)->folder == "Animations"
				&& actions.Inspect(copy.id)->name == "Walk Copy"
				&& actions.Redo() == Animation::AnimationAuthoringStatus::Ok
				&& actions.Redo() == Animation::AnimationAuthoringStatus::Ok,
				"Inverse lifecycle entries must restore and reapply one complete action at a time.");
			context.Expect(actions.Delete(clip.id) == Animation::AnimationAuthoringStatus::Ok
				&& actions.Inspect(clip.id) == nullptr
				&& actions.Undo() == Animation::AnimationAuthoringStatus::Ok
				&& actions.Inspect(clip.id) != nullptr,
				"Delete must be reversible through the shared lifecycle history.");
		});

		registry.Add("Editor.Animation.AssetLifecycle.DurableImport", [](TestContext& context) {
			using namespace TerraEngine::Editor;
			const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
			const std::filesystem::path workspace = std::filesystem::temp_directory_path()
				/ ("TerraEngineAnimationLifecycle-" + std::to_string(unique));
			const std::filesystem::path assetRoot = workspace / "Assets";
			std::error_code error;
			std::filesystem::create_directories(workspace, error);
			Assets::AnimationAssetActions sourceActions(workspace / "SourceAssets");
			Assets::AnimationAssetEntry sourceEntry;
			context.Expect(sourceActions.Create(Assets::AnimationAssetKind::Clip,
				"Incoming", "Generated", sourceEntry) == Animation::AnimationAuthoringStatus::Ok,
				"The import fixture must itself be a production-decodable clip document.");
			Assets::AnimationAssetActions actions(assetRoot);
			Assets::AnimationAssetEntry imported;
			const auto result = actions.Import(sourceEntry.sourcePath, "Imported", imported);
			context.Expect(result == Animation::AnimationAuthoringStatus::Ok
				&& imported.kind == Assets::AnimationAssetKind::Clip
				&& std::filesystem::is_regular_file(imported.sourcePath)
				&& imported.sourcePath.parent_path() == assetRoot / "Imported",
				"Import must stage a typed source document beneath the project asset root.");
			std::filesystem::remove_all(workspace, error);
		});

		registry.Add("Editor.Animation.AssetLifecycle.ExtractReadOnlyPackageChildren", [](TestContext& context) {
			using namespace TerraEngine::Editor;
			using namespace TerraEngine::AssetPipeline;
			using namespace TerraEngine::Animation;
			const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
			const std::filesystem::path workspace = std::filesystem::temp_directory_path()
				/ ("TerraEngineAnimationPackageExtraction-" + std::to_string(unique));
			const std::filesystem::path buildRoot = workspace / "Build";
			const std::filesystem::path assetRoot = workspace / "Assets";
			const std::filesystem::path skeletonPath = buildRoot / "Fox_Generated" / "Fox.teskel";
			const std::filesystem::path clipPath = buildRoot / "Fox_Generated" / "Walk.teanim";
			const AnimationGuid sourceSkeletonId{TerraEngine::GenerateUUID()};
			const AnimationGuid sourceClipId{TerraEngine::GenerateUUID()};
			constexpr AnimationCompatibilityFingerprint compatibility = 0xF0A043U;

			// Use production serializers for the package fixture. Three joints make
			// this regression catch a copy path that accidentally drops or reorders
			// hierarchy data while changing the editable asset's own identity.
			AnimationTransform bindTransform{};
			bindTransform.rotation = {0.0F, 0.0F, 0.0F, 1.0F};
			bindTransform.scale = {1.0F, 1.0F, 1.0F};
			AnimationMatrix4x4 inverseBind{};
			inverseBind.values = {1.0F, 0.0F, 0.0F, 0.0F,
				0.0F, 1.0F, 0.0F, 0.0F,
				0.0F, 0.0F, 1.0F, 0.0F,
				0.0F, 0.0F, 0.0F, 1.0F};
			SkeletonAsset sourceSkeleton;
			sourceSkeleton.assetId = sourceSkeletonId;
			sourceSkeleton.compatibilityFingerprint = compatibility;
			sourceSkeleton.referencePoseHash = 0x71234U;
			sourceSkeleton.source.sourcePackagePath = "Characters/Fox.glb";
			sourceSkeleton.joints = {
				{101U, -1, bindTransform, true, "Root", "/root", "root"},
				{102U, 0, bindTransform, true, "Spine", "/root/spine", "spine"},
				{103U, 1, bindTransform, true, "Head", "/root/spine/head", "head"}};
			sourceSkeleton.inverseBindMatrices.assign(sourceSkeleton.joints.size(), inverseBind);
			std::vector<std::uint8_t> skeletonBytes;
			context.Expect(SerializeSkeletonAsset(sourceSkeleton, skeletonBytes) == AnimationStatus::Ok,
				"The generated-skeleton fixture must use the production cooked format.");

			// Compile a non-empty clip so the extraction proof covers real track and
			// stream tables, not merely a blank starter document.
			AnimationClipCompileInput clipInput;
			clipInput.assetId = sourceClipId;
			clipInput.skeletonAssetId = sourceSkeletonId;
			clipInput.compatibilityFingerprint = compatibility;
			clipInput.durationTicks = 60;
			clipInput.sampleRate = 60U;
			AnimationSourceTrack translation;
			translation.stableTargetId = 102U;
			translation.kind = AnimationClipTrackKind::Translation;
			translation.keys = {{0, {0.0F, 0.0F, 0.0F}}, {60, {1.0F, 0.0F, 0.0F}}};
			AnimationSourceTrack rootTranslation;
			rootTranslation.stableTargetId = 101U;
			rootTranslation.kind = AnimationClipTrackKind::RootTranslation;
			rootTranslation.keys = {{0, {0.0F, 0.0F, 0.0F}}, {60, {2.0F, 0.0F, 0.0F}}};
			clipInput.tracks = {translation, rootTranslation};
			AnimationClipAsset sourceClip;
			AnimationClipCompileReport compileReport;
			context.Expect(CompileAnimationClip(clipInput, {}, sourceClip, compileReport)
				== AnimationStatus::Ok,
				"The generated-clip fixture must compile through the production codec.");
			std::vector<std::uint8_t> clipBytes;
			context.Expect(SerializeAnimationClipAsset(sourceClip, clipBytes) == AnimationStatus::Ok,
				"The generated-clip fixture must serialize a checked production envelope.");
			std::filesystem::create_directories(skeletonPath.parent_path());
			{
				std::ofstream skeletonOutput(skeletonPath, std::ios::binary | std::ios::trunc);
				skeletonOutput.write(reinterpret_cast<const char*>(skeletonBytes.data()),
					static_cast<std::streamsize>(skeletonBytes.size()));
				std::ofstream clipOutput(clipPath, std::ios::binary | std::ios::trunc);
				clipOutput.write(reinterpret_cast<const char*>(clipBytes.data()),
					static_cast<std::streamsize>(clipBytes.size()));
				context.Expect(static_cast<bool>(skeletonOutput) && static_cast<bool>(clipOutput),
					"The generated package fixture must publish complete source payloads.");
			}

			Assets::AnimationAssetActions actions(assetRoot);
			Assets::AnimationAssetEntry editableSkeleton;
			Assets::AnimationAssetEntry editableClip;
			Assets::AnimationAssetEntry secondEditableClip;
			context.Expect(actions.ExtractReadOnlyPackageChild(Assets::AnimationAssetKind::Skeleton,
				skeletonPath, buildRoot, "Fox Skeleton", "Validation/T281", editableSkeleton)
				== Animation::AnimationAuthoringStatus::Ok
				&& actions.ExtractReadOnlyPackageChild(Assets::AnimationAssetKind::Clip,
					clipPath, buildRoot, "Fox Walk", "Validation/T281", editableClip)
					== Animation::AnimationAuthoringStatus::Ok
				&& actions.ExtractReadOnlyPackageChild(Assets::AnimationAssetKind::Clip,
					clipPath, buildRoot, "Fox Walk", "Validation/T281", secondEditableClip)
					== Animation::AnimationAuthoringStatus::Ok
				&& editableClip.sourcePath != secondEditableClip.sourcePath,
				"Extracted package children must publish unique loose paths without overwriting a prior copy.");

			auto readBytes = [](const std::filesystem::path& path) {
				std::ifstream input(path, std::ios::binary);
				return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
					std::istreambuf_iterator<char>());
			};
			SkeletonAsset copiedSkeleton;
			AnimationClipAsset copiedClip;
			context.Expect(DeserializeSkeletonAsset(readBytes(editableSkeleton.sourcePath), compatibility,
				copiedSkeleton) == AnimationStatus::Ok
				&& DeserializeAnimationClipAsset(readBytes(editableClip.sourcePath), compatibility,
					copiedClip) == AnimationStatus::Ok
				&& copiedSkeleton.assetId != sourceSkeleton.assetId
				&& copiedSkeleton.compatibilityFingerprint == sourceSkeleton.compatibilityFingerprint
				&& copiedSkeleton.joints == sourceSkeleton.joints
				&& copiedClip.assetId != sourceClip.assetId
				&& copiedClip.skeletonAssetId == sourceClip.skeletonAssetId
				&& copiedClip.compatibilityFingerprint == sourceClip.compatibilityFingerprint
				&& copiedClip.tracks == sourceClip.tracks,
				"Extraction must create new durable identities while preserving decoded skeleton and clip data.");

			const std::filesystem::path outsideSource = workspace / "Outside.teanim";
			{
				std::ofstream outsideOutput(outsideSource, std::ios::binary | std::ios::trunc);
				outsideOutput.write(reinterpret_cast<const char*>(clipBytes.data()),
					static_cast<std::streamsize>(clipBytes.size()));
			}
			Assets::AnimationAssetEntry rejected;
			context.Expect(actions.ExtractReadOnlyPackageChild(Assets::AnimationAssetKind::Clip,
				outsideSource, buildRoot, "Escaped", "Validation/T281", rejected)
				== Animation::AnimationAuthoringStatus::InvalidArgument
				&& !std::filesystem::exists(assetRoot / "Validation" / "T281" / "Escaped.teanim"),
				"Extraction must reject a source outside the generated build root before it changes Assets.");
			std::error_code error;
			std::filesystem::remove_all(workspace, error);
		});

		registry.Add("Editor.Animation.AssetLifecycle.DurableDuplicateDeleteHistoryIsExactAndAtomic", [](TestContext& context) {
			using namespace TerraEngine::Editor;
			const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
			const std::filesystem::path workspace = std::filesystem::temp_directory_path()
				/ ("TerraEngineAnimationDurableHistory-" + std::to_string(unique));
			Assets::AnimationAssetActions actions(workspace / "Assets");
			Assets::AnimationAssetEntry original;
			Assets::AnimationAssetEntry duplicate;
			context.Expect(actions.Create(Assets::AnimationAssetKind::Clip,
				"Walk", "Animations", original) == Animation::AnimationAuthoringStatus::Ok
				&& actions.Duplicate(original.id, "Walk Copy", duplicate)
					== Animation::AnimationAuthoringStatus::Ok,
				"A durable duplicate must be created from the complete checked source document.");
			auto readBytes = [](const std::filesystem::path& path) {
				std::ifstream input(path, std::ios::binary);
				return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
					std::istreambuf_iterator<char>());
			};
			const std::vector<std::uint8_t> originalBytes = readBytes(original.sourcePath);
			const std::vector<std::uint8_t> duplicateBytes = readBytes(duplicate.sourcePath);
			TerraEngine::Animation::AnimationClipAsset originalClip;
			TerraEngine::Animation::AnimationClipAsset duplicateClip;
			context.Expect(!originalBytes.empty() && !duplicateBytes.empty()
				&& TerraEngine::Animation::DeserializeAnimationClipAsset(
					originalBytes, 0U, originalClip) == TerraEngine::Animation::AnimationStatus::Ok
				&& TerraEngine::Animation::DeserializeAnimationClipAsset(
					duplicateBytes, 0U, duplicateClip) == TerraEngine::Animation::AnimationStatus::Ok
				&& duplicateClip.assetId.IsValid() && duplicateClip.assetId != originalClip.assetId,
				"Duplicate must publish one complete checked document with a distinct asset GUID.");
			const Animation::AnimationAuthoringStatus deleteStatus = actions.Delete(duplicate.id);
			context.Expect(deleteStatus == Animation::AnimationAuthoringStatus::Ok,
				std::string{"Delete must accept the complete duplicated document (status "}
					+ std::to_string(static_cast<unsigned int>(deleteStatus)) + ", existed "
					+ (std::filesystem::exists(duplicate.sourcePath) ? "true" : "false") + ").");
			context.Expect(!std::filesystem::exists(duplicate.sourcePath),
				"Delete must remove the durable duplicated document.");
			context.Expect(actions.Undo() == Animation::AnimationAuthoringStatus::Ok,
				"Delete undo must restore one durable document.");
			context.Expect(readBytes(duplicate.sourcePath) == duplicateBytes,
				"Delete undo must restore the exact complete document bytes.");
			context.Expect(actions.Redo() == Animation::AnimationAuthoringStatus::Ok,
				"Delete redo must remove one durable document.");
			context.Expect(!std::filesystem::exists(duplicate.sourcePath),
				"Delete redo must leave the durable document absent.");
			context.Expect(actions.Undo() == Animation::AnimationAuthoringStatus::Ok,
				"A second delete undo must restore one durable document.");
			context.Expect(readBytes(duplicate.sourcePath) == duplicateBytes,
				"A second delete undo must restore the same exact complete document bytes.");
			bool foundStagingFile = false;
			for(const auto& entry : std::filesystem::recursive_directory_iterator(workspace)) {
				foundStagingFile = foundStagingFile
					|| entry.path().filename().string().find(".animation-action-") != std::string::npos;
			}
			context.Expect(!foundStagingFile,
				"Successful lifecycle publication must not leave a staging artifact in the project.");
			std::error_code error;
			std::filesystem::remove_all(workspace, error);
		});

		registry.Add("Editor.Animation.AssetLifecycle.ControllerCreationWritesDecodableDocument", [](TestContext& context) {
			using namespace TerraEngine::Editor;
			const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
			const std::filesystem::path workspace = std::filesystem::temp_directory_path()
				/ ("TerraEngineControllerCreation-" + std::to_string(unique));
			Assets::AnimationAssetActions actions(workspace / "Assets");
			Assets::AnimationAssetEntry controller;
			context.Expect(actions.Create(Assets::AnimationAssetKind::Controller,
				"Locomotion", "Animations", controller) == Animation::AnimationAuthoringStatus::Ok,
				"Controller creation should publish its type-specific starter document.");
			std::ifstream input(controller.sourcePath, std::ios::binary);
			const std::vector<std::uint8_t> bytes(
				(std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
			TerraEngine::Animation::AnimationControllerAsset decoded;
			context.Expect(TerraEngine::Animation::DeserializeAnimationControllerAsset(bytes, 0U, decoded)
				== TerraEngine::Animation::AnimationStatus::Ok
				&& decoded.assetId.IsValid() && decoded.states.size() == 1U,
				"The created .teanimctrl must be immediately loadable by the production decoder.");
			std::error_code error;
			std::filesystem::remove_all(workspace, error);
		});

		registry.Add("Editor.Animation.AssetLifecycle.ValidationGenerationCancelAtomicity", [](TestContext& context) {
			using namespace TerraEngine::Editor;
			const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
			const std::filesystem::path workspace = std::filesystem::temp_directory_path()
				/ ("TerraEngineAnimationValidation-" + std::to_string(unique));
			std::error_code error;
			Assets::AnimationAssetActions actions(workspace / "Assets");
			Assets::AnimationAssetEntry controller;
			context.Expect(actions.Create(Assets::AnimationAssetKind::Controller, "Locomotion", "Animations", controller)
				== Animation::AnimationAuthoringStatus::Ok,
				"Validation starts from a stable authoring entry.");
			Assets::AnimationAssetValidationRequest request;
			context.Expect(actions.BeginValidation(controller.id, request) == Animation::AnimationAuthoringStatus::Ok,
				"A tracked durable controller should produce an immutable validation request.");
			const Assets::AnimationAssetValidationResult lateResult =
				Assets::AnimationAssetActions::ValidateCandidate(request);
			context.Expect(actions.CancelValidation(request) == Animation::AnimationAuthoringStatus::Canceled
				&& actions.CompleteValidation(lateResult) == Animation::AnimationAuthoringStatus::Stale
				&& actions.Inspect(controller.id)->generation == controller.generation,
				"Canceled worker validation must not publish a late result or bump generation.");
			context.Expect(actions.BeginValidation(controller.id, request) == Animation::AnimationAuthoringStatus::Ok,
				"A second validation request should supersede the canceled request.");
			{
				std::ofstream corrupt(controller.sourcePath, std::ios::binary | std::ios::trunc);
				corrupt << "not an animation document";
			}
			const Assets::AnimationAssetValidationResult invalidResult =
				Assets::AnimationAssetActions::ValidateCandidate(request);
			context.Expect(actions.CompleteValidation(invalidResult)
					== Animation::AnimationAuthoringStatus::ValidationFailed
				&& !actions.Inspect(controller.id)->valid
				&& !actions.Inspect(controller.id)->diagnostic.empty(),
				"A rejected compile must retain the last good entry and expose diagnostics.");
			std::filesystem::remove_all(workspace, error);
			Assets::AnimationAssetActions restoredActions(workspace / "Assets");
			Assets::AnimationAssetEntry restored;
			context.Expect(restoredActions.Create(Assets::AnimationAssetKind::Controller,
				"Restored", "Animations", restored) == Animation::AnimationAuthoringStatus::Ok
				&& restoredActions.BeginValidation(restored.id, request) == Animation::AnimationAuthoringStatus::Ok,
				"A fresh valid document should remain independently publishable.");
			const Assets::AnimationAssetValidationResult validResult =
				Assets::AnimationAssetActions::ValidateCandidate(request);
			context.Expect(restoredActions.CompleteValidation(validResult) == Animation::AnimationAuthoringStatus::Ok
				&& restoredActions.Inspect(restored.id)->valid
				&& restoredActions.Inspect(restored.id)->generation == restored.generation + 1U,
				"Only a matching generation may atomically publish a successful validation.");
			std::filesystem::remove_all(workspace, error);
		});
	}
}
