// AudioDiagnosticsWindow.cpp - snapshot-only provider and workload diagnostics.

#include "AudioDiagnosticsWindow.h"
#include "Engine/Audio/AudioDiagnostics.h"

#include <imgui.h>

AudioDiagnosticsWindow::AudioDiagnosticsWindow(MainEditorSubsystem* ownerValue)
	: EditorWindow("Audio Diagnostics", false, "Audio"), owner(ownerValue) {}

void AudioDiagnosticsWindow::OnGui() {
	const auto snapshot = TerraEngine::Audio::AcquireAudioDiagnosticsSnapshot();
	if(!snapshot) { ImGui::TextUnformatted("Audio has not published a diagnostic snapshot yet."); return; }
	ImGui::Text("Provider: %s (%s)", snapshot->provider.displayName.c_str(), snapshot->provider.providerId.c_str());
	ImGui::Text("Device available: %s", snapshot->deviceAvailable ? "yes" : "safe silence");
	ImGui::Text("Listeners: %u  Audible: %u  Virtual: %u", snapshot->listenerCandidates, snapshot->audibleVoices, snapshot->virtualVoices);
	if(ImGui::CollapsingHeader("Failures")) for(const auto& failure : snapshot->failures)
		ImGui::BulletText("%s x%llu: %s", failure.category.c_str(), static_cast<unsigned long long>(failure.count), failure.message.c_str());
	if(ImGui::CollapsingHeader("Scheduler")) for(const auto& workload : snapshot->scheduler)
		ImGui::BulletText("%s [%s] %s", workload.workload.c_str(), workload.phase.c_str(), workload.boundary.c_str());
}
