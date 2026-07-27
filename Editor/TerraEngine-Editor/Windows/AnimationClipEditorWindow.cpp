// AnimationClipEditorWindow.cpp - selected cooked clip timeline and diagnostics.

#include "AnimationClipEditorWindow.h"

#include "AnimationAssetWindowSupport.h"
#include "Engine/Animation/Assets/AnimationClipAsset.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <imgui.h>
#include <string>

namespace {
	using TerraEngine::Animation::AnimationClipTrackKind;
	using TerraEngine::Animation::AnimationInterpolation;

	const char* TrackKindName(AnimationClipTrackKind kind) {
		switch(kind) {
		case AnimationClipTrackKind::Translation: return "Translation";
		case AnimationClipTrackKind::Rotation: return "Rotation";
		case AnimationClipTrackKind::Scale: return "Scale";
		case AnimationClipTrackKind::MorphWeight: return "Morph";
		case AnimationClipTrackKind::Curve: return "Curve";
		case AnimationClipTrackKind::RootTranslation: return "Root translation";
		case AnimationClipTrackKind::RootYaw: return "Root yaw";
		}
		return "Unknown";
	}

	const char* InterpolationName(AnimationInterpolation interpolation) {
		switch(interpolation) {
		case AnimationInterpolation::Constant: return "Constant";
		case AnimationInterpolation::Step: return "Step";
		case AnimationInterpolation::Linear: return "Linear";
		case AnimationInterpolation::Cubic: return "Cubic";
		}
		return "Unknown";
	}
}

struct AnimationClipEditorWindow::State {
	TerraEngine::Editor::Animation::CookedAnimationAssetSelection<
		TerraEngine::Animation::AnimationClipAsset> selection;
	std::filesystem::path synchronizedPath;
	std::int64_t fallbackDuration = 0;
	std::int64_t cursor = 0;
	std::int64_t trimStart = 0;
	std::int64_t trimEnd = 0;
	double fractionalTicks = 0.0;
	float speed = 1.0F;
	bool playing = false;
	bool looping = true;
};

AnimationClipEditorWindow::AnimationClipEditorWindow(MainEditorSubsystem* owner)
	: EditorWindow("Animation Clip Editor", false, "Animation/Authoring"),
	  owner_(owner),
	  state_(std::make_unique<State>()) {
}

AnimationClipEditorWindow::~AnimationClipEditorWindow() = default;

void AnimationClipEditorWindow::SetDuration(std::int64_t ticks) noexcept {
	state_->fallbackDuration = std::max<std::int64_t>(0, ticks);
	state_->cursor = std::min(state_->cursor, state_->fallbackDuration);
	state_->trimEnd = state_->fallbackDuration;
}

std::int64_t AnimationClipEditorWindow::Cursor() const noexcept {
	return state_->cursor;
}

void AnimationClipEditorWindow::OnGui() {
	state_->selection.Synchronize(
		owner_, ".teanim", &TerraEngine::Animation::DeserializeAnimationClipAsset);
	const TerraEngine::Animation::AnimationClipAsset* clip = state_->selection.Get();

	if(!state_->selection.IsSelectedForWindow() && clip == nullptr && state_->fallbackDuration == 0) {
		ImGui::TextWrapped("Select a .teanim asset in the Project Browser to inspect and preview its fixed-tick timeline.");
		return;
	}
	if(state_->selection.IsSelectedForWindow()) {
		ImGui::TextWrapped("%s", state_->selection.Path().string().c_str());
		if(clip == nullptr) {
			ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "%s",
				state_->selection.Message().c_str());
			return;
		}
		if(state_->selection.IsStale()) {
			ImGui::TextColored(ImVec4(1.0F, 0.75F, 0.2F, 1.0F),
				"The latest file was rejected; showing the prior complete decoded copy.");
		}
	}

	const std::int64_t duration = clip ? clip->durationTicks : state_->fallbackDuration;
	if(clip && state_->synchronizedPath != state_->selection.Path()) {
		state_->synchronizedPath = state_->selection.Path();
		state_->cursor = 0;
		state_->trimStart = 0;
		state_->trimEnd = duration;
		state_->playing = false;
		state_->fractionalTicks = 0.0;
	}

	if(state_->playing && duration > 0) {
		state_->fractionalTicks += static_cast<double>(ImGui::GetIO().DeltaTime)
			* static_cast<double>(TerraEngine::Animation::kAnimationFixedTicksPerSecond)
			* static_cast<double>(state_->speed);
		const std::int64_t wholeTicks = state_->fractionalTicks >= 0.0
			? static_cast<std::int64_t>(std::floor(state_->fractionalTicks))
			: static_cast<std::int64_t>(std::ceil(state_->fractionalTicks));
		state_->fractionalTicks -= static_cast<double>(wholeTicks);
		state_->cursor += wholeTicks;
		if(state_->looping) {
			const std::int64_t cycle = duration + 1;
			state_->cursor = ((state_->cursor % cycle) + cycle) % cycle;
		} else if(state_->cursor <= 0 || state_->cursor >= duration) {
			state_->cursor = std::clamp<std::int64_t>(state_->cursor, 0, duration);
			state_->playing = false;
		}
	}

	if(ImGui::Button("|<")) {
		state_->cursor = 0;
	}
	ImGui::SameLine();
	if(ImGui::Button("<")) {
		state_->cursor = std::max<std::int64_t>(0, state_->cursor - 1);
	}
	ImGui::SameLine();
	if(ImGui::Button(state_->playing ? "Pause" : "Play")) {
		state_->playing = !state_->playing;
	}
	ImGui::SameLine();
	if(ImGui::Button(">")) {
		state_->cursor = std::min(duration, state_->cursor + 1);
	}
	ImGui::SameLine();
	if(ImGui::Button(">|")) {
		state_->cursor = duration;
	}
	ImGui::SameLine();
	ImGui::Checkbox("Loop", &state_->looping);
	ImGui::SliderFloat("Speed", &state_->speed, -2.0F, 2.0F, "%.2fx");
	const std::int64_t minimumTick = 0;
	ImGui::SliderScalar("Cursor", ImGuiDataType_S64, &state_->cursor,
		&minimumTick, &duration, "%lld");
	ImGui::Text("Fixed tick %lld / %lld (%.3f s)",
		static_cast<long long>(state_->cursor), static_cast<long long>(duration),
		static_cast<double>(state_->cursor) /
			static_cast<double>(TerraEngine::Animation::kAnimationFixedTicksPerSecond));

	if(clip == nullptr || !ImGui::BeginTabBar("AnimationClipEditorTabs")) {
		return;
	}

	if(ImGui::BeginTabItem("Timeline")) {
		ImGui::Text("Sample rate: %u Hz", clip->sampleRate);
		ImGui::Text("Segments: %zu   Tracks: %zu   Stream: %zu bytes",
			clip->segments.size(), clip->tracks.size(), clip->streamData.size());
		if(ImGui::BeginTable("ClipTrackTable", 6,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
			ImVec2(0.0F, 260.0F))) {
			for(const char* heading : {"Target", "Kind", "Interpolation", "Segment", "Samples", "Bits"}) {
				ImGui::TableSetupColumn(heading);
			}
			ImGui::TableHeadersRow();
			for(const auto& track : clip->tracks) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::Text("%llu", static_cast<unsigned long long>(track.stableTargetId));
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(TrackKindName(track.kind));
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(InterpolationName(track.interpolation));
				ImGui::TableNextColumn();
				ImGui::Text("%u", track.segmentIndex);
				ImGui::TableNextColumn();
				ImGui::Text("%u", track.sampleCount);
				ImGui::TableNextColumn();
				ImGui::Text("%u", track.encodedValueBits);
			}
			ImGui::EndTable();
		}
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Trim / Segments")) {
		state_->trimStart = std::clamp<std::int64_t>(state_->trimStart, 0, duration);
		state_->trimEnd = std::clamp<std::int64_t>(state_->trimEnd, state_->trimStart, duration);
		ImGui::SliderScalar("Preview start", ImGuiDataType_S64, &state_->trimStart,
			&minimumTick, &duration, "%lld");
		ImGui::SliderScalar("Preview end", ImGuiDataType_S64, &state_->trimEnd,
			&state_->trimStart, &duration, "%lld");
		ImGui::TextDisabled("Trim handles are a non-destructive preview until an authoring transaction publishes a replacement clip generation.");
		for(std::size_t index = 0U; index < clip->segments.size(); ++index) {
			const auto& segment = clip->segments[index];
			ImGui::BulletText("Segment %zu: %lld..%lld, tracks %u..%u, %llu bytes",
				index, static_cast<long long>(segment.startTick),
				static_cast<long long>(segment.endTick), segment.firstTrack,
				segment.firstTrack + segment.trackCount,
				static_cast<unsigned long long>(segment.streamSize));
		}
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Events / Channels")) {
		ImGui::SeparatorText("Events");
		if(clip->events.empty()) {
			ImGui::TextDisabled("No authored events.");
		}
		for(const auto& event : clip->events) {
			ImGui::BulletText("%lld  id=%llu  payload=%s  %s%s",
				static_cast<long long>(event.tick),
				static_cast<unsigned long long>(event.stableEventId),
				event.payload.empty() ? "(none)" : event.payload.c_str(),
				event.fireForward ? "forward " : "",
				event.fireReverse ? "reverse" : "");
		}
		ImGui::SeparatorText("Named curve / morph channels");
		for(const auto& channel : clip->namedChannels) {
			ImGui::BulletText("%s  (%llu)", channel.name.c_str(),
				static_cast<unsigned long long>(channel.stableId));
		}
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Root / Compression")) {
		ImGui::Text("Root motion: %s", clip->rootMotion.enabled ? "enabled" : "disabled");
		if(clip->rootMotion.enabled) {
			ImGui::Text("Translation track: %u", clip->rootMotion.translationTrackIndex);
			ImGui::Text("Yaw track: %u", clip->rootMotion.yawTrackIndex);
			ImGui::Text("Cycle translation: %.4f  %.4f  %.4f",
				clip->rootMotion.accumulatedCycleTranslation.x,
				clip->rootMotion.accumulatedCycleTranslation.y,
				clip->rootMotion.accumulatedCycleTranslation.z);
			ImGui::Text("Cycle yaw: %.6f rad", clip->rootMotion.accumulatedCycleYawRadians);
		}
		ImGui::SeparatorText("Compression error contract");
		ImGui::Text("Decoder version: %u", clip->compression.decoderVersion);
		ImGui::Text("Translation: %.6f m", clip->compression.translationErrorMeters);
		ImGui::Text("Rotation: %.6f degrees", clip->compression.rotationErrorDegrees);
		ImGui::Text("Scale: %.6f", clip->compression.scaleErrorFraction);
		ImGui::Text("Morph: %.6f", clip->compression.morphError);
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Recording")) {
		if(!clip->recordingProvenance) {
			ImGui::TextDisabled("This imported clip has no recorder provenance chunk.");
		} else {
			const auto& provenance = *clip->recordingProvenance;
			ImGui::Text("Source scene: %s", provenance.sourceSceneIdentity.c_str());
			ImGui::Text("Source session: %s", provenance.sourceSessionIdentity.c_str());
			ImGui::Text("Range: %lld..%lld at %u Hz",
				static_cast<long long>(provenance.sourceFirstTick),
				static_cast<long long>(provenance.sourceLastTick), provenance.sourceSampleRate);
			ImGui::Text("Samples / events: %llu / %llu",
				static_cast<unsigned long long>(provenance.capturedSampleCount),
				static_cast<unsigned long long>(provenance.capturedEventCount));
			ImGui::Text("Keys: %llu source, %llu retained, %llu removed",
				static_cast<unsigned long long>(provenance.sourceKeyCount),
				static_cast<unsigned long long>(provenance.retainedKeyCount),
				static_cast<unsigned long long>(provenance.removedKeyCount));
			for(const auto& channel : provenance.channels) {
				ImGui::BulletText("%s  channel=%llu target=%llu", channel.name.c_str(),
					static_cast<unsigned long long>(channel.stableChannelId),
					static_cast<unsigned long long>(channel.stableTargetId));
			}
		}
		ImGui::EndTabItem();
	}

	ImGui::EndTabBar();
}
