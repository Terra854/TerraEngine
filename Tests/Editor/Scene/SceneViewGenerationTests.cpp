// These regressions use production resolution/serialization and actual editor
// frames. Generation observation is read-only; toolbar changes use real input.
#include "../TestHarness.h"
#include "Tests/Support/AuthoringFrameFixture.h"
#include "Editor/TerraEngine-Editor/Windows/SceneViewWindow.h"
#include "Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.h"
#include "Engine/Scene/WorldStreamingSystem.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <sstream>

namespace TerraEngine::Scene {
	struct WorldStreamingReviewTestAccess {
		static std::vector<ObserverInterest> TakeSceneViewInterests() {
			auto& system = WorldStreamingSystem::Instance(); std::scoped_lock lock(system.mutex);
			std::vector<ObserverInterest> saved;
			for(const auto& interest : system.observerInterests) if(interest.observerId == "editor-scene-view") saved.push_back(interest);
			std::erase_if(system.observerInterests, [](const auto& interest) { return interest.observerId == "editor-scene-view"; });
			return saved;
		}
		static std::optional<ObserverInterest> LatestSceneViewInterest() {
			auto& system = WorldStreamingSystem::Instance(); std::scoped_lock lock(system.mutex);
			for(auto it = system.observerInterests.rbegin(); it != system.observerInterests.rend(); ++it)
				if(it->observerId == "editor-scene-view") return *it;
			return std::nullopt;
		}
		static void RestoreSceneViewInterests(std::vector<ObserverInterest> prior) {
			auto& system = WorldStreamingSystem::Instance(); std::scoped_lock lock(system.mutex);
			std::erase_if(system.observerInterests, [](const auto& interest) { return interest.observerId == "editor-scene-view"; });
			system.observerInterests.insert(system.observerInterests.end(), prior.begin(), prior.end());
		}
	};
}
namespace TerraEngine::EditorTests {
	namespace Overlay = Editor::SceneViewOverlay;
	struct SceneViewGenerationTestAccess {
		static std::uint64_t Generation(const SceneViewWindow& window) { return window.m_MarkerViewGeneration; }
		static std::optional<Overlay::SceneViewOverlayCacheKey> WidgetKey(const SceneViewWindow& window) { return window.m_OrientationWidgetCache.key; }

		static void ExpectFlyShadowFocusIsolation(TestContext& context)
		{
			SceneViewWindow window{nullptr};
			const auto initial = window.SerializeWorkspaceState();
			// Exercise the production flight-entry step without a GLFW cursor.
			window.CaptureFlyShadowFocus();
			context.Expect(window.SerializeWorkspaceState() == initial && !window.m_CameraFocusInitialized,
				"Capturing a shadow reference must leave the saved camera and navigation pivot alone.");

			const glm::vec3 originalForward = window.GetCameraForward();
			context.Expect(window.TranslateCamera(-48.0 * glm::dvec3(originalForward)),
				"The fly camera must be able to back away from its shadow reference.");
			window.CaptureFlyShadowFocus();
			context.Expect(std::abs(window.ResolveShadowFocusDepth(originalForward) - 66.0f) < 0.001f,
				"Backing away across repeated fly sessions must keep the original shadow reference.");

			window.m_CameraRot.y += glm::radians(180.0f);
			const glm::vec3 turnedForward = window.GetCameraForward();
			context.ExpectEqual(0.0f, window.ResolveShadowFocusDepth(turnedForward),
				"A shadow reference behind the turned camera must be ignored.");
			const auto beforeSwitch = window.SerializeWorkspaceState();
			const auto expectedPivot = Scene::ApplyWorldDelta(window.m_CameraPosition,
				{{}, 18.0 * glm::dvec3(turnedForward)});
			std::string error;
			context.Expect(window.SetAutomationOrientation("orthographic", error), error);
			context.Expect(expectedPivot.Succeeded() && window.m_CameraFocus == expectedPivot.position,
				"Orthographic mode must center ahead of the turned camera.");
			context.Expect(window.SetAutomationOrientation("perspective", error), error);
			const auto afterSwitch = window.SerializeWorkspaceState();
			for (const char* field : {"position", "rotationRadians", "focus", "focusDistance",
				"focusInitialized", "fieldOfViewDegrees", "projection"})
			{
				context.Expect(afterSwitch.at(field) == beforeSwitch.at(field),
					std::string("The projection round trip must restore perspective ") + field);
			}

			window.CaptureFlyShadowFocus();
			context.Expect(std::abs(window.ResolveShadowFocusDepth(turnedForward) - 18.0f) < 0.001f,
				"Starting a new flight after turning must establish a visible shadow reference.");
			context.Expect(window.RestoreWorkspaceState(beforeSwitch, error), error);
			context.ExpectEqual(0.0f, window.ResolveShadowFocusDepth(turnedForward),
				"Workspace restoration must clear the transient shadow reference.");
		}
	};
	namespace {
		struct SceneViewFrames {
			AuthoringFrameFixture frames;
			MainEditorSubsystem editor;
			SceneViewWindow window{&editor};
			Overlay::SceneViewDiagnosticsSnapshot prior = Overlay::GetLatestDiagnosticsSnapshot();
			std::vector<Scene::ObserverInterest> priorInterests = Scene::WorldStreamingReviewTestAccess::TakeSceneViewInterests();
			~SceneViewFrames() {
				// Own only this serialized UI fixture's producer; unrelated observers
				// are neither cleared nor rewritten by cleanup.
				Scene::WorldStreamingReviewTestAccess::RestoreSceneViewInterests(std::move(priorInterests));
				Overlay::PublishLatestDiagnosticsSnapshot(prior);
			}
			void Draw(ImVec2 size = ImVec2(800.0F, 650.0F)) {
				frames.Frame([&] {
					ImGui::SetNextWindowPos(ImVec2(20.0F, 20.0F), ImGuiCond_Always);
					ImGui::SetNextWindowSize(size, ImGuiCond_Always); window.Draw();
				});
			}
			void HideIconsWithToolbar(TestContext& context) {
				Draw(); auto* native = ImGui::FindWindowByName("Scene View");
				context.Expect(native != nullptr, "Production Scene View must submit a real ImGui window.");
				const ImGuiID seed = native->GetID("scene_icons");
				const ImGuiID icon = ImHashStr("##scene_toolbar_icon", 0, seed);
				ImRect bounds;
				frames.Frame([&] {
					AuthoringFrameFixture::PlaceWindow(); auto* draw = ImGui::GetForegroundDrawList(); const int first = draw->VtxBuffer.Size;
					ImGui::DebugLocateItem(icon); window.Draw();
					context.Expect(ImGui::GetCurrentContext()->DebugLocateId == 0 && draw->VtxBuffer.Size >= first + 8, "Real icons toolbar item must be submitted and located.");
					bounds = ImRect(draw->VtxBuffer[first].pos, draw->VtxBuffer[first].pos);
					for(int i = 1; i < 8; ++i) bounds.Add(draw->VtxBuffer[first + i].pos);
				});
				const auto click = bounds.GetCenter(); ImGui::GetIO().AddMousePosEvent(click.x, click.y); Draw();
				ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true); Draw();
				ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false); Draw();
				ImGui::GetIO().AddMousePosEvent(-100.0F, -100.0F); Draw();
				context.Expect(!Overlay::GetLatestDiagnosticsSnapshot().toolState.iconsVisible, "Actual toolbar input must hide icons before generation assertions.");
			}
		};
		void ExpectCurrentFrame(TestContext& context, SceneViewFrames& fixture, std::uint64_t previous, bool changed) {
			const auto generation = SceneViewGenerationTestAccess::Generation(fixture.window);
			const auto key = SceneViewGenerationTestAccess::WidgetKey(fixture.window);
			const auto interest = Scene::WorldStreamingReviewTestAccess::LatestSceneViewInterest();
			const auto snapshot = Overlay::GetLatestDiagnosticsSnapshot(); const auto actual = fixture.window.GetAutomationState();
			const auto forward = actual["camera"]["forward"];
			const glm::vec3 expected(forward[0].get<float>(), forward[1].get<float>(), forward[2].get<float>());
			const bool vectorMatches = glm::length(snapshot.orientationWidget.cameraForward - expected) < 1.0e-5F;
			std::ostringstream diagnostic; diagnostic << "Actual SceneView frame: previous=" << previous << ", generation=" << generation
				<< ", cache=" << (key ? key->viewportGeneration : 0U) << ", lease=" << (interest ? interest->leaseGeneration : 0U)
				<< ", label=" << snapshot.orientationWidget.label << ", forwardMatches=" << vectorMatches;
			context.Expect(changed ? generation > previous : generation == previous, diagnostic.str());
			context.Expect(key && key->viewportGeneration == generation && interest && interest->leaseGeneration == generation, diagnostic.str());
			context.Expect(snapshot.orientationWidget.visible && vectorMatches && snapshot.orientationWidget.projectionMode == snapshot.projectionMode, diagnostic.str());
			context.ExpectEqual(actual.value("projection", "") == "orthographic" ? std::string("Ortho") : std::string("Persp"), snapshot.orientationWidget.label, diagnostic.str());
		}
	}
	void RegisterSceneViewGenerationTests(TestRegistry& registry) {
		for(const auto mode : {Overlay::LargeSceneMode::On, Overlay::LargeSceneMode::Off, Overlay::LargeSceneMode::Automatic}) {
			registry.Add(std::string("Editor.SceneViewGeneration.LargeSceneReason.") + std::string(Overlay::ToString(mode)), [mode](TestContext& context) {
				Overlay::SceneOverlayPreference preference; preference.largeSceneMode = mode; preference.largeSceneObjectThreshold = 100U;
				for(const std::uint32_t count : {99U, 100U, 101U}) {
					Overlay::SceneViewDiagnosticsSnapshot snapshot; snapshot.largeSceneState = Overlay::ResolveLargeSceneState(preference, count);
					const auto serialized = Overlay::SerializeDiagnosticsSnapshot(snapshot)["largeScene"];
					const bool effective = mode == Overlay::LargeSceneMode::On || (mode == Overlay::LargeSceneMode::Automatic && count >= 100U);
					const auto reason = mode == Overlay::LargeSceneMode::On ? "forced_on" : mode == Overlay::LargeSceneMode::Off ? "forced_off" : effective ? "object_threshold" : "below_threshold";
					context.ExpectEqual(effective, serialized["effective"].get<bool>(), "Resolved effective mode must obey explicit/threshold policy.");
					context.ExpectEqual(std::string(reason), serialized["reason"].get<std::string>(), "Explanation must distinguish explicit choice from threshold selection.");
				}
			});
		}
		registry.Add("Editor.SceneViewGeneration.HiddenIconsCameraRotation", [](TestContext& context) {
			SceneViewFrames fixture; fixture.HideIconsWithToolbar(context);
			const auto unchanged = SceneViewGenerationTestAccess::Generation(fixture.window); fixture.Draw(); ExpectCurrentFrame(context, fixture, unchanged, false);
			std::string error; context.Expect(fixture.window.SetAutomationOrientation("right", error), error);
			fixture.Draw(); ExpectCurrentFrame(context, fixture, unchanged, true);
		});
		registry.Add("Editor.SceneViewGeneration.HiddenIconsCameraTranslation", [](TestContext& context) {
			SceneViewFrames fixture; fixture.HideIconsWithToolbar(context); std::string error;
			Scene::WorldPosition position{{}, {10.0, 5.0, 20.0}};
			Scene::WorldPosition focus{{}, {10.0, 5.0, 10.0}};
			context.Expect(fixture.window.SetAutomationCamera(position, focus, error), error);
			fixture.Draw();
			const auto before = SceneViewGenerationTestAccess::Generation(fixture.window);
			const auto priorCamera = fixture.window.GetAutomationState()["camera"];
			// Translate camera and focus together through the public canonical API.
			// The camera-relative view matrix stays unchanged: position alone must
			// invalidate the actual published lease and orientation cache generation.
			position.offsetMetres.x += 17.0; focus.offsetMetres.x += 17.0;
			context.Expect(fixture.window.SetAutomationCamera(position, focus, error), error);
			fixture.Draw(); ExpectCurrentFrame(context, fixture, before, true);
			const auto movedCamera = fixture.window.GetAutomationState()["camera"];
			const auto interest = Scene::WorldStreamingReviewTestAccess::LatestSceneViewInterest();
			context.Expect(interest && interest->position == position,
				"Actual streaming request must carry the translated canonical camera position.");
			context.Expect(movedCamera["position"] != priorCamera["position"]
				&& movedCamera["forward"] == priorCamera["forward"] && movedCamera["up"] == priorCamera["up"],
				"Translation must change the real camera position while preserving orientation.");
			const auto translated = SceneViewGenerationTestAccess::Generation(fixture.window);
			fixture.Draw(); ExpectCurrentFrame(context, fixture, translated, false);
			const auto retainedInterest = Scene::WorldStreamingReviewTestAccess::LatestSceneViewInterest();
			context.Expect(retainedInterest && retainedInterest->position == position,
				"An unchanged next frame must retain the translated request and reuse its generation.");
		});
		registry.Add("Editor.SceneViewGeneration.HiddenIconsProjectionToggle", [](TestContext& context) {
			SceneViewFrames fixture; fixture.HideIconsWithToolbar(context); std::string error;
			const auto before = SceneViewGenerationTestAccess::Generation(fixture.window);
			context.Expect(fixture.window.SetAutomationOrientation("orthographic", error), error); fixture.Draw(); ExpectCurrentFrame(context, fixture, before, true);
			const auto ortho = SceneViewGenerationTestAccess::Generation(fixture.window);
			context.Expect(fixture.window.SetAutomationOrientation("perspective", error), error); fixture.Draw(); ExpectCurrentFrame(context, fixture, ortho, true);
		});
		registry.Add("Editor.SceneViewGeneration.FlyShadowFocusDoesNotMoveNavigationPivot", [](TestContext& context) {
			SceneViewGenerationTestAccess::ExpectFlyShadowFocusIsolation(context);
		});
		registry.Add("Editor.SceneViewGeneration.HiddenIconsViewportResize", [](TestContext& context) {
			SceneViewFrames fixture; fixture.HideIconsWithToolbar(context); const auto before = SceneViewGenerationTestAccess::Generation(fixture.window);
			fixture.Draw(ImVec2(960.0F, 650.0F)); ExpectCurrentFrame(context, fixture, before, true);
		});
	}
}
