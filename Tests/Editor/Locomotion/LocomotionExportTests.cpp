// LocomotionExportTests.cpp - real staged animation-closure and TEPAK parity.

#include "../TestHarness.h"

#include "AnimationExportDependencies.h"
#include "Engine/Components/AnimatorComponent.h"
#include "Engine/Components/CharacterLocomotionComponent.h"
#include "Engine/Components/Transform.h"
#include "Engine/Core/Object/ComponentRegistry.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Runtime/ContentMount/ContentMount.h"
#include "Engine/Scene/Serializer.h"
#include "Engine/ThreadPool/ThreadPool.h"
#include "Engine/Utilities/UUID.h"
#include "TEPAK/PackWriter.h"
#include "Tests/Engine/Locomotion/LocomotionAnimationFixture.h"
#include "validation/AnimationPackValidator.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace {
		struct LocomotionExportWorkspace final {
			LocomotionExportWorkspace()
				: root(std::filesystem::temp_directory_path()
					/ ("TerraEngineLocomotionExport-" + ToHexString(GenerateUUID())))
				, assets(root / "Assets")
				, staged(root / "Staged")
				, archive(root / "Output" / "Locomotion.tepak")
			{
				std::error_code error;
				if(std::filesystem::create_directory(root, error)) {
					created = true;
					std::filesystem::create_directories(assets, error);
					if(!error) std::filesystem::create_directories(staged, error);
				}
			}

			~LocomotionExportWorkspace()
			{
				if(created) {
					std::error_code ignored;
					std::filesystem::remove_all(root, ignored);
				}
			}

			std::filesystem::path root;
			std::filesystem::path assets;
			std::filesystem::path staged;
			std::filesystem::path archive;
			bool created = false;
		};

		bool WriteBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
		{
			std::error_code error;
			std::filesystem::create_directories(path.parent_path(), error);
			if(error) return false;
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			if(!output) return false;
			output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			return output.good();
		}

		bool WriteJson(const std::filesystem::path& path, const nlohmann::json& value)
		{
			std::error_code error;
			std::filesystem::create_directories(path.parent_path(), error);
			if(error) return false;
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			if(!output) return false;
			output << value.dump(2);
			return output.good();
		}

		bool StageRequiredShaders(const std::filesystem::path& staged)
		{
			// Export staging treats these marker words as opaque packaged shader bytes;
			// deep validation checks their required paths and the cooked dependency graph.
			const std::vector<std::uint8_t> marker{0x03U, 0x02U, 0x23U, 0x07U};
			for(const std::string& path : Export::RequiredAnimationShaderPaths()) {
				std::error_code error;
				std::filesystem::create_directories((staged / path).parent_path(), error);
				if(error) return false;
				std::ofstream output(staged / path, std::ios::binary | std::ios::trunc);
				if(!output) return false;
				output.write(reinterpret_cast<const char*>(marker.data()), static_cast<std::streamsize>(marker.size()));
				if(!output.good()) return false;
			}
			return true;
		}
	}

	void RegisterLocomotionExportTests(TestRegistry& registry)
	{
		registry.Add("Editor.Locomotion.Authoring.Export.ArchivePreservesAuthoredDescendantAndCookedAnimationClosure", [](TestContext& context) {
			// Asset registration schedules production resource work. Prior lifecycle
			// cases intentionally drain the pool, so establish this case's own pool.
			ThreadPool::instance().initialize();
			struct PoolCleanup {
				~PoolCleanup() { ThreadPool::instance().shutdown(); }
			} poolCleanup;
			LocomotionExportWorkspace workspace;
			context.Expect(workspace.created && std::filesystem::exists(workspace.assets)
				&& std::filesystem::exists(workspace.staged),
				"The export parity case must reserve an isolated writable staging workspace.");

			// The shared engine fixture throws on failure; the editor harness also
			// catches std::exception, so construction failures remain test failures.
			TerraEngine::Tests::TestContext assetContext;
			auto animation = TerraEngine::Tests::LocomotionAnimationFixture::Create(assetContext);
			if(!animation) return;

			std::vector<std::uint8_t> skeletonBytes;
			std::vector<std::uint8_t> idleBytes;
			std::vector<std::uint8_t> moveBytes;
			std::vector<std::uint8_t> rootBytes;
			std::vector<std::uint8_t> controllerBytes;
			using namespace Animation;
			context.Expect(SerializeSkeletonAsset(animation->Skeleton(), skeletonBytes) == AnimationStatus::Ok
				&& SerializeAnimationClipAsset(animation->IdleClip(), idleBytes) == AnimationStatus::Ok
				&& SerializeAnimationClipAsset(animation->MoveClip(), moveBytes) == AnimationStatus::Ok
				&& SerializeAnimationClipAsset(animation->RootClip(), rootBytes) == AnimationStatus::Ok
				&& SerializeAnimationControllerAsset(animation->Controller(), controllerBytes) == AnimationStatus::Ok,
				"The authored graph's controller and clips must use real production cooked encoders.");

			const std::filesystem::path content = workspace.staged;
			const bool stagedAssets = WriteBytes(content / "characters/locomotion.teskel", skeletonBytes)
				&& WriteBytes(content / "characters/idle.teanim", idleBytes)
				&& WriteBytes(content / "characters/move.teanim", moveBytes)
				&& WriteBytes(content / "characters/root.teanim", rootBytes)
				&& WriteBytes(content / "controllers/locomotion.teanimctrl", controllerBytes);
			context.Expect(stagedAssets && StageRequiredShaders(content),
				"The staged export must contain only the source-free cooked locomotion dependencies and required shader paths.");

			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object owner = objects.createObject("Locomotion export owner");
			Object animatorObject = objects.createObject("Locomotion descendant animator");
			(void)objects.AddComponent<Transform>(owner);
			(void)objects.AddComponent<Transform>(animatorObject);
			const bool parented = objects.setParent(animatorObject, owner);
			auto* locomotion = objects.AddComponent<CharacterLocomotionComponent>(owner);
			auto* animator = animation->AttachAnimator(objects, animatorObject);
			context.Expect(parented && locomotion != nullptr && animator != nullptr,
				"The parity scene needs an owner, its actual descendant animator, and their authored components.");
			if(locomotion == nullptr || animator == nullptr) {
				objects.clear();
				return;
			}

			locomotion->active = true;
			locomotion->animatorBinding = CharacterLocomotionComponent::kDescendantBinding;
			locomotion->animatorObjectId = objects.getUUID(animatorObject);
			locomotion->maximumSpeed = 7.25;
			locomotion->acceleration = 13.5;
			locomotion->braking = 18.75;
			locomotion->turnRate = 4.5;
			locomotion->speedParameter = "Speed";
			locomotion->localDirectionXParameter = "DirectionX";
			locomotion->localDirectionZParameter = "DirectionZ";
			locomotion->groundedParameter = "Grounded";
			locomotion->movingParameter = "Moving";
			animator->defaultClipAssetId = animation->RootClip().assetId.value;
			const UUID ownerId = objects.getUUID(owner);
			const UUID descendantId = objects.getUUID(animatorObject);
			const double expectedMaximumSpeed = locomotion->maximumSpeed;
			const UUID expectedControllerId = animator->controllerAssetId;
			const UUID expectedRootClipId = animator->defaultClipAssetId;

			const nlohmann::json scene = {
				{"format", Scene::kTerraSceneUnboundedFormat},
				{"objects", nlohmann::json::array({Serializer::SerializeObject(owner)})}
			};
			const bool sceneStaged = WriteJson(content / "Scenes/Locomotion.tescene", scene)
				&& WriteJson(content / "game.json", {{"startupScene", "Scenes/Locomotion.tescene"}});
			context.Expect(sceneStaged,
				"The scene serialized from live authored components must enter the same staged content root as the cooked assets.");
			objects.clear();

			const Export::AnimationExportClosure closure =
				Export::AnimationExportDependencyResolver::Resolve({workspace.assets, content});
			const std::string controllerGuid = ToHexString(expectedControllerId);
			const std::string rootClipGuid = ToHexString(expectedRootClipId);
			const auto hasAsset = [&closure](const std::string& guid, std::string_view logicalPath) {
				for(const Export::AnimationExportAsset& asset : closure.assets) {
					if(asset.assetGuid == guid && asset.path == logicalPath && asset.cookedAnimationEnvelope)
						return true;
				}
				return false;
			};
			context.Expect(closure.Complete() && hasAsset(controllerGuid, "controllers/locomotion.teanimctrl")
				&& hasAsset(rootClipGuid, "characters/root.teanim")
				&& closure.HasAnimationAssets(),
				"The standard staged Animation resolver must discover the referenced cooked controller and root clip by their serialized identities.");
			bool controllerReferencesRoot = false;
			for(const Export::AnimationExportAsset& asset : closure.assets) {
				if(asset.assetGuid != controllerGuid) continue;
				for(const Export::AnimationExportDependency& dependency : asset.dependencies)
					controllerReferencesRoot = controllerReferencesRoot || dependency.assetGuid == rootClipGuid;
			}
			context.Expect(controllerReferencesRoot,
				"The controller's cooked dependency edge must resolve to the staged root clip instead of editor/source fallback.");

			std::string manifestError;
			const bool manifestWritten = closure.Complete()
				&& Export::AnimationExportDependencyResolver::WriteRuntimeManifests(closure, content, manifestError);
			context.Expect(manifestWritten,
				"A complete source-free animation closure must write the normal sanitized runtime manifests: " + manifestError);
			tepak::PackagingRequest request{};
			request.inputRoot = content;
			request.outputPackPath = workspace.archive;
			request.extentSizingBytes = 4096U;
			request.workerCount = 1U;
			request.enableCompression = false;
			request.enableDictionaryTraining = false;
			request.overwriteExisting = true;
			request.peakMemoryBudgetBytes = 16ULL * 1024ULL * 1024ULL;
			std::error_code directoryError;
			std::filesystem::create_directories(workspace.archive.parent_path(), directoryError);
			const tepak::PackWriteResult packaged = directoryError ? tepak::PackWriteResult{}
				: tepak::PackWriter::Create(request);
			const Export::AnimationPackValidationReport archiveValidation =
				Export::AnimationPackValidator::ValidateArchive(workspace.archive);
			context.Expect(packaged.descriptor.fileCount > 0U && archiveValidation.accepted,
				"The normal TEPAK writer and deep animation archive validator must accept this source-free scene-and-cooked-assets package.");

			ContentMount mounted;
			context.Expect(mounted.Mount(workspace.archive),
				"Runtime parity must read the bytes back through the real archive content mount.");
			if(mounted.IsMounted()) {
				const auto sceneRead = mounted.ReadAllChecked("Scenes/Locomotion.tescene");
				const auto controllerRead = mounted.ReadAllChecked("controllers/locomotion.teanimctrl");
				const auto rootRead = mounted.ReadAllChecked("characters/root.teanim");
				const auto archiveEntries = mounted.SnapshotEntries();
				bool sourceFree = true;
				for(const MountedContentEntry& entry : archiveEntries)
					sourceFree = sourceFree && !Export::IsForbiddenAnimationRuntimePath(entry.logicalPath);
				bool authoredParity = false;
				if(sceneRead.Succeeded()) {
					const nlohmann::json exported = nlohmann::json::parse(
						sceneRead.bytes.begin(), sceneRead.bytes.end());
					objects.clear();
					Object loadedOwner = Serializer::DeserializeObject(exported.at("objects").at(0U));
					const auto* loadedLocomotion = objects.GetComponent<CharacterLocomotionComponent>(loadedOwner);
					const auto children = objects.getChildren(loadedOwner);
					const Object* loadedAnimatorObject = children.empty() ? nullptr : &children.front();
					const auto* loadedAnimator = loadedAnimatorObject == nullptr
						? nullptr : objects.GetComponent<AnimatorComponent>(*loadedAnimatorObject);
					authoredParity = loadedLocomotion != nullptr && loadedAnimator != nullptr
						&& objects.getUUID(loadedOwner) == ownerId
						&& objects.getUUID(*loadedAnimatorObject) == descendantId
						&& loadedLocomotion->animatorBinding == CharacterLocomotionComponent::kDescendantBinding
						&& loadedLocomotion->animatorObjectId == descendantId
						&& loadedLocomotion->maximumSpeed == expectedMaximumSpeed
						&& loadedAnimator->controllerAssetId == expectedControllerId
						&& loadedAnimator->defaultClipAssetId == expectedRootClipId;
				}
			context.Expect(sceneRead.Succeeded() && controllerRead.Succeeded() && rootRead.Succeeded()
				&& authoredParity && sourceFree,
				"Mounted archive bytes must preserve locomotion authoring and descendant UUID while containing the cooked controller/root assets and no source fallback entries.");
			objects.clear();
			mounted.Unmount();
			}
		});
	}
}
