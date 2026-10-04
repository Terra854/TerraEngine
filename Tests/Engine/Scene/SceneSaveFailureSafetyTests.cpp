// Scene saving must preserve the last complete file and report serialization failures.
#include "../TestHarness.h"
#include "../Runtime/CorrectnessTestSupport.h"
#include "Core/Object/Component.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/SceneManager.h"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <array>
#include <limits>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace TerraEngine::Tests {
	namespace {
		// Faults belong to this registered component instance, not a global serializer
		// override. The getter also supports the existing reflected serialization path.
		struct SaveFaultComponent final : Component<SaveFaultComponent> {
			static constexpr std::string_view kComponentName = "SceneSaveFaultFixtureComponent";
			int fault = 0;
			std::string value = "last good authored value";
			float number = 1.0f;
			std::string GetValue() const {
				if(fault == 1) throw std::runtime_error("Injected authored serialization failure.");
				if(fault == 2) return std::string(1U, static_cast<char>(0xFF));
				return value;
			}
			void SetValue(std::string incoming) { value = std::move(incoming); }
			float GetNumber() const { return fault == 3 ? std::numeric_limits<float>::quiet_NaN() : number; }
			void SetNumber(float incoming) { number = incoming; }
			static nlohmann::json SerializeAuthored(const SaveFaultComponent& component) {
				return {{"value", component.GetValue()}, {"number", component.GetNumber()}};
			}
			static void reflect(entt::meta_factory<SaveFaultComponent>& factory) {
				TerraEngine::Reflection::RegisterAccessorProperty<&SaveFaultComponent::SetValue, &SaveFaultComponent::GetValue>(factory, "value");
				TerraEngine::Reflection::RegisterAccessorProperty<&SaveFaultComponent::SetNumber, &SaveFaultComponent::GetNumber>(factory, "number");
			}
		};
		std::string ReadSavedBytes(const std::filesystem::path& path) {
			std::ifstream input(path, std::ios::binary);
			return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
		}
		struct SaveFixture final {
			Correctness::DisposableFixturePath files;
			std::filesystem::path oldRoot = SceneManager::Instance().GetSceneRoot();
			std::filesystem::path destination;
			TerraEngine::UUID sceneId = TerraEngine::EMPTY_UUID;
			SaveFaultComponent* component = nullptr;
			SaveFixture(TestContext& context) {
				SaveFaultComponent::registerType();
				auto& scenes = SceneManager::Instance();
				scenes.BeginLoadSession(); ObjectManager::instance().clear(); scenes.SetSceneRoot(files.Root());
				destination = files.Child("last-good.tescene");
				const nlohmann::json document{{"format", "TerraSceneUnboundedV1"}, {"name", "Save failure fixture"}, {"objects", nlohmann::json::array()}};
				{ std::ofstream output(destination); output << document.dump(4); context.Expect(output.good(), "The disposable scene must be written."); }
				sceneId = scenes.LoadScene(destination);
				context.Expect(sceneId != TerraEngine::EMPTY_UUID && scenes.SetActiveScene(sceneId), "The actual SceneManager must load and activate the fresh scene.");
				const auto object = ObjectManager::instance().createObject("Save serialization fault owner");
				component = ObjectManager::instance().AddComponent<SaveFaultComponent>(object);
				context.Expect(component != nullptr, "The authored fault component must be part of the real live scene.");
				scenes.ForceMarkActiveSceneDirty();
				context.Expect(scenes.SaveScene(), "A valid production save must establish a last-good destination and cache.");
			}
			~SaveFixture() {
				SceneManager::Instance().UnloadAll(); ObjectManager::instance().clear(); SceneManager::Instance().SetSceneRoot(oldRoot);
			}
		};
		void VerifyRejectedSave(TestContext& context, int fault, bool newDestination) {
			SaveFixture fixture(context);
			auto& scenes = SceneManager::Instance();
			const auto lastBytes = ReadSavedBytes(fixture.destination);
			const auto lastCache = scenes.GetScene(fixture.sceneId)->jsonContents;
			const auto target = newDestination ? fixture.files.Child("new-destination.tescene") : fixture.destination;
			fixture.component->fault = fault; scenes.ForceMarkActiveSceneDirty();
			bool escaped = false; bool saved = true;
			try { saved = scenes.SaveScene(target); } catch(const std::exception&) { escaped = true; }
			// Inspect disk/cache first so a pre-fix truncate is reported directly even
			// when dump also throws. A failed new save must not leave a scene filename.
			context.Expect(ReadSavedBytes(fixture.destination) == lastBytes, "Rejected production saving must retain byte-identical last-good scene bytes.");
			context.Expect(!newDestination || !std::filesystem::exists(target), "A failed new save must not leave an incomplete scene at its destination.");
			context.Expect(scenes.GetScene(fixture.sceneId)->jsonContents == lastCache && scenes.IsActiveSceneDirty(),
				"Rejected saving must preserve the persisted cache and leave the live scene dirty.");
			context.Expect(!escaped && !saved, "Production SaveScene must return failure instead of letting serialization exceptions escape.");
			fixture.component->fault = 0; fixture.component->value = "successful retry";
			context.Expect(scenes.SaveScene(), "A later valid save must retry successfully.");
			const auto retryBytes = ReadSavedBytes(fixture.destination);
			context.Expect(retryBytes != lastBytes && !scenes.IsActiveSceneDirty(), "Successful retry publishes complete new bytes and clears the unchanged revision.");
			context.Expect(scenes.ReloadScene(fixture.destination) != TerraEngine::EMPTY_UUID, "The successfully retried scene must reopen through the production loader.");
		}
		// Every injected failure still travels through real SaveScene and the real
		// native writer. Checkpoints reject only the named filesystem operation.
		void VerifyFilesystemStages(TestContext& context, bool newDestination) {
			using namespace TerraEngine::Utilities;
			constexpr std::array stages{AtomicFileWriteStage::Capture, AtomicFileWriteStage::Serialize,
				AtomicFileWriteStage::Open, AtomicFileWriteStage::Write, AtomicFileWriteStage::Flush,
				AtomicFileWriteStage::Finalize, AtomicFileWriteStage::Close, AtomicFileWriteStage::Replace};
			for(const auto rejectedStage : stages) {
				SaveFixture fixture(context);
				auto& scenes = SceneManager::Instance();
				const auto lastBytes = ReadSavedBytes(fixture.destination);
				const auto lastCache = scenes.GetScene(fixture.sceneId)->jsonContents;
				const auto target = newDestination ? fixture.files.Child("new-stage-target.tescene") : fixture.destination;
				fixture.component->value = "new authored stage value"; scenes.ForceMarkActiveSceneDirty();
				unsigned rejected = 0;
				AtomicFileWriteOptions options;
				options.checkpoint = [&](AtomicFileWriteStage stage, const std::filesystem::path&) {
					if(stage == rejectedStage) { ++rejected; return std::string("Injected native filesystem checkpoint rejection."); }
					return std::string{};
				};
				context.Expect(!scenes.SaveScene(target, options), "A rejected save stage must return failure.");
				const auto result = scenes.GetLastSaveResult();
				context.Expect(rejected == 1 && !result.committed && result.stage == rejectedStage
					&& result.reason.find("checkpoint rejection") != std::string::npos && result.cleanupError.empty(),
					"Failure diagnostics must identify the rejected stage and successful staging cleanup.");
				context.Expect(ReadSavedBytes(fixture.destination) == lastBytes && (!newDestination || !std::filesystem::exists(target)),
					"Every precommit stage preserves old bytes and leaves a new destination absent.");
				context.Expect(scenes.GetScene(fixture.sceneId)->jsonContents == lastCache && scenes.IsActiveSceneDirty(),
					"Every precommit rejection preserves persisted cache and unsaved authoring state.");
				for(const auto& entry : std::filesystem::directory_iterator(fixture.files.Root())) {
					context.Expect(entry.path().filename().string().find(".terra-save-") == std::string::npos,
						"Owned staging artifacts must be removed after a rejected save.");
				}
				context.Expect(scenes.ReloadScene(fixture.destination) != TerraEngine::EMPTY_UUID,
					"The byte-identical last-good scene must remain loadable after every rejected stage.");
				context.Expect(scenes.SaveScene(target), "A rejected stage must permit a subsequent successful retry.");
				context.Expect(!scenes.IsActiveSceneDirty() && scenes.GetLastSaveResult().committed,
					"A successful retry clears the unchanged captured revision and reports committed success.");
				context.Expect(!newDestination || scenes.GetScene(fixture.sceneId)->jsonContents == lastCache,
					"Save As retains the original source document cache and path semantics.");
				context.Expect(ReadSavedBytes(target) != lastBytes && scenes.ReloadScene(target) != TerraEngine::EMPTY_UUID,
					"The successful retry must publish complete changed bytes which reopen through SceneManager.");
			}
		}
		void VerifyCaptureAndLaterRevision(TestContext& context) {
			using namespace TerraEngine::Utilities;
			SaveFixture fixture(context);
			auto& scenes = SceneManager::Instance();
			const auto lastBytes = ReadSavedBytes(fixture.destination);
			const auto lastCache = scenes.GetScene(fixture.sceneId)->jsonContents;
			fixture.component->value = "captured authored value"; scenes.ForceMarkActiveSceneDirty();
			AtomicFileWriteOptions incoherent;
			incoherent.checkpoint = [&](AtomicFileWriteStage stage, const std::filesystem::path&) {
				if(stage == AtomicFileWriteStage::Serialize) { fixture.component->value = "revision during capture"; scenes.ForceMarkActiveSceneDirty(); }
				return std::string{};
			};
			context.Expect(!scenes.SaveScene({}, incoherent), "An edit during capture must reject the incoherent save before file I/O.");
			context.Expect(ReadSavedBytes(fixture.destination) == lastBytes && scenes.GetScene(fixture.sceneId)->jsonContents == lastCache
				&& scenes.IsActiveSceneDirty(), "An incoherent capture retains old disk/cache and dirty state.");
			fixture.component->value = "captured before filesystem edit"; scenes.ForceMarkActiveSceneDirty();
			AtomicFileWriteOptions laterEdit;
			laterEdit.checkpoint = [&](AtomicFileWriteStage stage, const std::filesystem::path&) {
				if(stage == AtomicFileWriteStage::Replace) { fixture.component->value = "newer live authored value"; scenes.ForceMarkActiveSceneDirty(); }
				return std::string{};
			};
			context.Expect(scenes.SaveScene({}, laterEdit), "A coherent older captured version may commit while later edits remain live.");
			const auto savedBytes = ReadSavedBytes(fixture.destination);
			context.Expect(savedBytes.find("captured before filesystem edit") != std::string::npos
				&& savedBytes.find("newer live authored value") == std::string::npos,
				"Replacement publishes exactly the complete captured version rather than torn newer values.");
			context.Expect(scenes.GetScene(fixture.sceneId)->jsonContents == savedBytes && scenes.IsActiveSceneDirty(),
				"The persisted cache matches committed bytes while newer authored edits remain dirty.");
			context.Expect(scenes.SaveScene() && !scenes.IsActiveSceneDirty(), "Saving the newer unchanged revision must subsequently clear dirty state.");
			context.Expect(scenes.ReloadScene(fixture.destination) != TerraEngine::EMPTY_UUID, "Both committed revisions must retain a loadable authored envelope.");
		}
		void VerifyStaleDocumentOwner(TestContext& context) {
			using namespace TerraEngine::Utilities;
			SaveFixture fixture(context);
			auto& scenes = SceneManager::Instance();
			const auto lastCache = scenes.GetScene(fixture.sceneId)->jsonContents;
			const auto oldIdentity = scenes.GetScene(fixture.sceneId)->saveIdentity;
			fixture.component->value = "old owner's coherent captured version"; scenes.ForceMarkActiveSceneDirty();
			AtomicFileWriteOptions options;
			options.checkpoint = [&](AtomicFileWriteStage stage, const std::filesystem::path&) {
				if(stage == AtomicFileWriteStage::Replace) {
					context.Expect(scenes.ReloadScene(fixture.destination) == fixture.sceneId, "Reload deliberately reuses the cache UUID.");
					scenes.ForceMarkActiveSceneDirty();
				}
				return std::string{};
			};
			context.Expect(scenes.SaveScene({}, options), "An old owned ticket can finish its already coherent disk version after document reload.");
			context.Expect(scenes.GetScene(fixture.sceneId)->saveIdentity != oldIdentity && scenes.GetScene(fixture.sceneId)->jsonContents == lastCache,
				"A stale ticket must not overwrite a replacement document's persisted cache just because its UUID matches.");
			context.Expect(scenes.IsActiveSceneDirty(), "A stale ticket must not clear the replacement owner's dirty state.");
			context.Expect(ReadSavedBytes(fixture.destination).find("old owner's coherent captured version") != std::string::npos,
				"The disk result truthfully reports the captured version which replacement committed.");
			context.Expect(scenes.SaveScene() && !scenes.IsActiveSceneDirty(), "The current document owner must be able to retry and adopt its own save.");
		}
		void VerifyTargetCoordination(TestContext& context) {
			using namespace TerraEngine::Utilities;
			SaveFixture fixture(context);
			const auto firstBytes = ReadSavedBytes(fixture.destination);
			auto secondDocument = nlohmann::json::parse(firstBytes);
			secondDocument["name"] = "Later serialized writer";
			const auto secondBytes = secondDocument.dump(4);
			const auto unrelated = fixture.files.Child("unrelated-target.tescene");
			struct WorkerOwner {
				bool owned = false;
				WorkerOwner() {
					if(ThreadPool::instance().getThreadCount() == 0) { owned = true; ThreadPool::instance().initialize(2); }
				}
				~WorkerOwner() { if(owned) ThreadPool::instance().shutdown(); }
			} workers;
			context.Expect(ThreadPool::instance().getThreadCount() >= 2, "Target coordination requires two admitted native writer jobs.");
			Correctness::ControlledGate publication;
			struct TaskSettlement {
				Correctness::ControlledGate& publication;
				std::shared_future<void> first, second;
				// Assertion unwinding must settle callbacks before fixture paths are
				// removed, even when this test borrowed an existing worker pool.
				~TaskSettlement() {
					publication.Cancel();
					if(first.valid()) first.wait();
					if(second.valid()) second.wait();
				}
			} settlement{publication};
			Correctness::CompletionCounter secondStarted;
			// Real staging commits first, but its transaction remains alive while
			// the modeled owner publication boundary is held. Another target must
			// progress during that interval; the same target must wait.
			auto first = ThreadPool::instance().createTask([target = fixture.destination, bytes = firstBytes, wait = publication.MakeWaiter()] {
				AtomicFileWriteTransaction transaction(target);
				const auto result = transaction.Commit(bytes);
				if(!result.committed) throw std::runtime_error(result.reason);
				if(wait() != Correctness::GateResult::Released) throw std::runtime_error("Writer publication gate was not released.");
			});
			auto firstFuture = first->getFuture().share(); ThreadPool::instance().enqueue(first); settlement.first = firstFuture;
			context.Expect(publication.WaitForArrivals(1), "The first native replacement must finish while its target lock remains owned.");
			auto second = ThreadPool::instance().createTask([target = fixture.destination, bytes = secondBytes, started = secondStarted] {
				started.Increment();
				AtomicFileWriteTransaction transaction(target);
				const auto result = transaction.Commit(bytes);
				if(!result.committed) throw std::runtime_error(result.reason);
			});
			auto secondFuture = second->getFuture().share(); ThreadPool::instance().enqueue(second); settlement.second = secondFuture;
			context.Expect(secondStarted.WaitForAtLeast(1), "The competing native writer must be admitted before releasing publication.");
			{
				AtomicFileWriteTransaction independent(unrelated);
				context.Expect(independent.Commit(firstBytes).committed, "An unrelated target must make progress while another target retains its publication lock.");
			}
			context.Expect(secondFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready,
				"Same-target replacement cannot finish before the prior owner's publication lock is released.");
			publication.Release();
			context.Expect(firstFuture.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready
				&& secondFuture.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready,
				"Both controlled writer jobs must settle after the owner releases publication.");
			firstFuture.get(); secondFuture.get();
			context.Expect(ReadSavedBytes(fixture.destination) == secondBytes && ReadSavedBytes(unrelated) == firstBytes,
				"Target coordination preserves ordered complete versions without blocking unrelated saves.");
			context.Expect(SceneManager::Instance().ReloadScene(fixture.destination) != TerraEngine::EMPTY_UUID,
				"The ordered final native writer bytes must reopen through the production scene loader.");
		}

		void VerifyRealSharingFailure(TestContext& context) {
#if defined(_WIN32)
			SaveFixture fixture(context);
			auto& scenes = SceneManager::Instance();
			const auto lastBytes = ReadSavedBytes(fixture.destination);
			const auto lastCache = scenes.GetScene(fixture.sceneId)->jsonContents;
			fixture.component->value = "sharing failure retry value"; scenes.ForceMarkActiveSceneDirty();
			struct SharedReadHandle {
				HANDLE handle;
				~SharedReadHandle() { if(handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
			} held{CreateFileW(fixture.destination.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
			context.Expect(held.handle != INVALID_HANDLE_VALUE, "A real read handle must deny destination deletion/replacement.");
			context.Expect(!scenes.SaveScene(), "Real Windows sharing protection must reject replacement without damaging old bytes.");
			context.Expect(scenes.GetLastSaveResult().stage == TerraEngine::Utilities::AtomicFileWriteStage::Replace
				&& !scenes.GetLastSaveResult().reason.empty(), "The real native error must be retained as a replacement failure.");
			context.Expect(ReadSavedBytes(fixture.destination) == lastBytes && scenes.GetScene(fixture.sceneId)->jsonContents == lastCache
				&& scenes.IsActiveSceneDirty(), "A real sharing error preserves old disk/cache and unsaved edits.");
			context.Expect(CloseHandle(held.handle) != 0, "The fixture must release sharing protection before retry."); held.handle = INVALID_HANDLE_VALUE;
			context.Expect(scenes.SaveScene() && !scenes.IsActiveSceneDirty() && scenes.ReloadScene(fixture.destination) != TerraEngine::EMPTY_UUID,
				"A save must retry and reopen successfully after the blocking native handle is released.");
#else
			// POSIX rename permits replacement of an open file; supported failures
			// are still covered by the deterministic native operation checkpoints.
			VerifyFilesystemStages(context, false);
#endif
		}

		void VerifyFailedReplacement(TestContext& context, int failure, bool repair) {
			SaveFixture fixture(context);
			auto& scenes = SceneManager::Instance();
			const auto goodBytes = ReadSavedBytes(fixture.destination);
			const auto identity = scenes.GetActiveScene()->saveIdentity;
			const auto document = scenes.GetActiveScene()->parsedJson;
			const auto settings = Serializer::SerializeSceneRenderSettings(scenes.GetActiveSceneRenderSettings());
			fixture.component->value = "Unsaved replacement recovery edit";
			scenes.ForceMarkActiveSceneDirty();
			// Removing the owned source is a reproducible unavailable-file failure.
			// Other cases fail interpretation without any native filesystem timing.
			if(failure == 0) std::filesystem::remove(fixture.destination);
			else {
				std::ofstream output(fixture.destination, std::ios::trunc);
				if(failure == 1) output << "{ malformed scene";
				else { auto wrong = document; wrong["name"] = 42; output << wrong.dump(); }
			}
			TerraEngine::UUID result = TerraEngine::EMPTY_UUID;
			bool unexpected = false;
			try { result = scenes.ReloadScene(fixture.destination); } catch(...) { unexpected = true; }
			context.Expect(!unexpected && result == TerraEngine::EMPTY_UUID, "Ordinary failed replacement must report no usable identity without throwing.");
			const auto* retained = scenes.GetScene(fixture.sceneId);
			context.Expect(retained != nullptr, "Failed replacement must retain the last usable document.");
			context.Expect(retained->saveIdentity == identity && retained->parsedJson == document
				&& scenes.GetActiveSceneId() == fixture.sceneId && scenes.IsActiveSceneDirty()
				&& Serializer::SerializeSceneRenderSettings(scenes.GetActiveSceneRenderSettings()) == settings
				&& fixture.component->value == "Unsaved replacement recovery edit",
				"Failure must preserve document ownership, active state, settings and unsaved live authored values.");
			if(repair) {
				{ std::ofstream output(fixture.destination); output << goodBytes; }
				context.Expect(scenes.SaveScene() && !scenes.IsActiveSceneDirty(), "Normal save must remain available after source repair.");
				context.Expect(ReadSavedBytes(fixture.destination).find("Unsaved replacement recovery edit") != std::string::npos,
					"The repaired target must receive the retained live edit.");
				context.Expect(scenes.ReloadScene(fixture.destination) == fixture.sceneId
					&& scenes.GetScene(fixture.sceneId)->saveIdentity != identity,
					"Explicit repaired reload must publish a fresh document under the retained source identity.");
			}
		}
		void VerifyFirstLoadFailure(TestContext& context) {
			SaveFixture fixture(context);
			auto& scenes = SceneManager::Instance();
			const auto before = scenes.GetLoadedSceneIds();
			const auto missing = fixture.files.Child("never-loaded.tescene");
			context.Expect(scenes.LoadScene(missing) == TerraEngine::EMPTY_UUID
				&& scenes.GetLoadedSceneIds() == before && scenes.GetScene(fixture.sceneId) != nullptr,
				"First-load failure must expose no usable UUID and must preserve unrelated loaded documents.");
		}
	}
	void RegisterSceneSaveFailureSafetyTests(TestRegistry& registry) {
		registry.Add("Scene.SaveFailureSafety.UnreadableReplacementPreservesEditableScene", [](TestContext& context) { VerifyFailedReplacement(context, 0, false); });
		registry.Add("Scene.SaveFailureSafety.MalformedReplacementPreservesEditableScene", [](TestContext& context) { VerifyFailedReplacement(context, 1, false); });
		registry.Add("Scene.SaveFailureSafety.WrongTypedReplacementPreservesEditableScene", [](TestContext& context) { VerifyFailedReplacement(context, 2, false); });
		registry.Add("Scene.SaveFailureSafety.FirstLoadFailureHasNoUsableIdentity", VerifyFirstLoadFailure);
		registry.Add("Scene.SaveFailureSafety.RepairedReloadAndNormalSave", [](TestContext& context) { VerifyFailedReplacement(context, 0, true); });
		registry.Add("Scene.SaveFailureSafety.ThrowingAuthoredCaptureReturnsFailure", [](TestContext& context) { VerifyRejectedSave(context, 1, false); });
		registry.Add("Scene.SaveFailureSafety.InvalidTextRetainsLastGoodDestination", [](TestContext& context) { VerifyRejectedSave(context, 2, false); });
		registry.Add("Scene.SaveFailureSafety.NonfiniteAuthoredValueRetainsLastGoodDestination", [](TestContext& context) { VerifyRejectedSave(context, 3, false); });
		registry.Add("Scene.SaveFailureSafety.InvalidTextLeavesNewDestinationAbsent", [](TestContext& context) { VerifyRejectedSave(context, 2, true); });
		registry.Add("Scene.SaveFailureSafety.FilesystemStagesPreserveOldDestination", [](TestContext& context) { VerifyFilesystemStages(context, false); });
		registry.Add("Scene.SaveFailureSafety.FilesystemStagesLeaveNewDestinationAbsent", [](TestContext& context) { VerifyFilesystemStages(context, true); });
		registry.Add("Scene.SaveFailureSafety.CaptureAndLaterRevisions", VerifyCaptureAndLaterRevision);
		registry.Add("Scene.SaveFailureSafety.StaleReloadedDocumentOwner", VerifyStaleDocumentOwner);
		registry.Add("Scene.SaveFailureSafety.RealWindowsSharingRejection", VerifyRealSharingFailure);
		registry.Add("Scene.SaveFailureSafety.TargetLockRetainsPublicationOrder", VerifyTargetCoordination);

	}
}
