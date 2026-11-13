#include "TestHarness.h"
#include "Tests/Support/AuthoringFrameFixture.h"
#include "Editor/TerraEngine-Editor/Windows/AnimationClipEditorWindow.h"
#include "Editor/TerraEngine-Editor/Windows/AudioMixerWindow.h"
#include "Audio/AudioSystem.h"

#include <cmath>
#include <algorithm>
#include <sstream>
#include <string>
#include <utility>

namespace TerraEngine::EditorTests {
	namespace {
		struct RestoreAuthoringMix final {
			Audio::AudioMixSnapshot previous = Audio::AudioSystem::Instance().LatestMixSnapshot();
			~RestoreAuthoringMix() {
				std::string diagnostic;
				(void)Audio::AudioSystem::Instance().ReplaceMixSnapshot(std::move(previous), diagnostic);
			}
		};
	}
	void RegisterAuthoringControlContinuityTests(TestRegistry& registry) {
		registry.Add("Editor.Animation.SubTickForwardTransportRemainsPlaying", [](TestContext& context) {
			AuthoringFrameFixture frames;
			AnimationClipEditorWindow window;
			window.SetDuration(1000);
			window.SetVisible(true);
			AnimationClipEditorTestAccess::Configure(window, 0, 0.001F, false);
			frames.Frame([&] { AuthoringFrameFixture::PlaceWindow(); window.Draw(); }, 0.00001F);
			context.ExpectEqual(std::int64_t{0}, window.Cursor(), "The fixture must remain below one whole tick.");
			context.Expect(AnimationClipEditorTestAccess::Playing(window), "Forward playback at zero must accumulate fractional ticks instead of stopping.");
		});
		registry.Add("Editor.Animation.SubTickReverseTransportRemainsPlaying", [](TestContext& context) {
			AuthoringFrameFixture frames;
			AnimationClipEditorWindow window;
			window.SetDuration(1000);
			window.SetVisible(true);
			AnimationClipEditorTestAccess::Configure(window, 1000, -0.001F, false);
			frames.Frame([&] { AuthoringFrameFixture::PlaceWindow(); window.Draw(); }, 0.00001F);
			context.ExpectEqual(std::int64_t{1000}, window.Cursor(), "The reverse fixture must remain below one whole tick.");
			context.Expect(AnimationClipEditorTestAccess::Playing(window), "Reverse playback at the end must accumulate fractional ticks instead of stopping.");
		});
		registry.Add("Editor.Audio.MasterMuteSubmittedDuringVolumeEdit", [](TestContext& context) {
			AuthoringFrameFixture frames;
			RestoreAuthoringMix restore;
			auto& audio = Audio::AudioSystem::Instance();
			std::string diagnostic;
			context.Expect(audio.SetMasterMix(1.0F, false, diagnostic), "Initialize the actual master volume.");
			AudioMixerWindow window(nullptr);
			window.SetVisible(true);
			frames.Frame([&] { AuthoringFrameFixture::PlaceWindow(); window.Draw(); });
			auto* nativeWindow = ImGui::FindWindowByName("Audio Mixer");
			context.Expect(nativeWindow != nullptr, "Draw must create the production audio window.");
			const ImGuiID slider = nativeWindow->GetID("Master Volume");
			const ImGuiID mute = nativeWindow->GetID("Master Mute");
			ImRect sliderBounds;
			frames.Frame([&] {
				AuthoringFrameFixture::PlaceWindow();
				auto* overlay = ImGui::GetForegroundDrawList();
				const int firstVertex = overlay->VtxBuffer.Size;
				ImGui::DebugLocateItem(slider);
				window.Draw();
				context.Expect(ImGui::GetCurrentContext()->DebugLocateId == 0, "The actual master slider must be submitted during the observation frame.");
				context.Expect(overlay->VtxBuffer.Size >= firstVertex + 8, "ImGui's locator must emit its actual item outline.");
				// DebugLocateItem draws the submitted item's rectangle before its
				// pointer line. With the default one-pixel stroke, the first eight
				// vertices cover its corner extrema with either baked or CPU AA lines.
				// The tiny outline padding is harmless for an interior click.
				sliderBounds = ImRect(overlay->VtxBuffer[firstVertex].pos, overlay->VtxBuffer[firstVertex].pos);
				for (int index = 1; index < 8; ++index) sliderBounds.Add(overlay->VtxBuffer[firstVertex + index].pos);
			});
			context.Expect(sliderBounds.GetWidth() > 40.0F && sliderBounds.GetHeight() > 4.0F, "Observed slider bounds must support an interior mouse interaction.");
			const ImVec2 click(sliderBounds.Min.x + sliderBounds.GetWidth() * 0.2F, sliderBounds.GetCenter().y);
			// A hover-only frame establishes window routing before the real press.
			ImGui::GetIO().AddMousePosEvent(click.x, click.y);
			frames.Frame([&] { AuthoringFrameFixture::PlaceWindow(); window.Draw(); });
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			bool submittedOnChange = false;
			bool changed = false;
			ImGuiID active = 0;
			const auto editFrame = [&] {
				const float before = audio.LatestMixSnapshot().master.volume;
				AuthoringFrameFixture::PlaceWindow();
				ImGui::DebugLocateItem(mute);
				window.Draw();
				active = ImGui::GetCurrentContext()->ActiveId;
				if (std::abs(audio.LatestMixSnapshot().master.volume - before) > 0.01F) {
					changed = true;
					submittedOnChange = ImGui::GetCurrentContext()->DebugLocateId == 0;
				}
			};
			frames.Frame(editFrame);
			// Also exercise held-pointer motion, rather than relying exclusively
			// on click-to-position behavior of a specific slider configuration.
			ImGui::GetIO().AddMousePosEvent(sliderBounds.Min.x + sliderBounds.GetWidth() * 0.4F, click.y);
			frames.Frame(editFrame);
			std::ostringstream evidence;
			evidence << "Actual slider edit premise: id=" << slider << ", active=" << active
				<< ", bounds=" << sliderBounds.Min.x << ',' << sliderBounds.Min.y << ':' << sliderBounds.Max.x << ',' << sliderBounds.Max.y
				<< ", mouse=" << ImGui::GetIO().MousePos.x << ',' << ImGui::GetIO().MousePos.y
				<< ", down=" << ImGui::GetIO().MouseDown[0] << ", volume=" << audio.LatestMixSnapshot().master.volume;
			context.Expect(active == slider, evidence.str());
			context.Expect(changed, evidence.str());
			context.Expect(submittedOnChange, "Master Mute must be submitted on the same frame as an edited master slider.");
		});
	}
}
