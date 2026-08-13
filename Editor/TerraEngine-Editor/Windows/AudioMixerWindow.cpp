// AudioMixerWindow.cpp - immutable mix snapshot editor.

#include "AudioMixerWindow.h"
#include "Engine/Audio/AudioSystem.h"
#include "../Core/EditorContext.h"
#include "../Project/ProjectAudioSettingsIO.h"
#include "../Project/ProjectManager.h"
#include "../Subsystems/MainEditorSubsystem.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstring>

AudioMixerWindow::AudioMixerWindow(MainEditorSubsystem* ownerValue)
	: EditorWindow("Audio Mixer", false, "Audio"), owner(ownerValue) {}

void AudioMixerWindow::OnGui() {
	auto& system = TerraEngine::Audio::AudioSystem::Instance();
	std::filesystem::path projectRoot;
	if(owner != nullptr && owner->GetContext() != nullptr && owner->GetContext()->projectManager
		&& owner->GetContext()->projectManager->HasActiveProject()) {
		projectRoot = owner->GetContext()->projectManager->GetProjectRoot();
	}
	if(projectRoot != loadedProjectRoot) {
		loadedProjectRoot = projectRoot;
		persistenceDiagnostic.clear();
		if(!projectRoot.empty()) {
			auto loaded = system.LatestMixSnapshot();
			if(TerraEngine::Editor::ProjectSettings::ReadProjectAudioMixSettings(projectRoot, loaded, persistenceDiagnostic))
				(void)system.ReplaceMixSnapshot(std::move(loaded), persistenceDiagnostic);
		}
	}
	const auto persist = [&]() {
		if(!loadedProjectRoot.empty())
			(void)TerraEngine::Editor::ProjectSettings::PersistProjectAudioMixSettings(
				loadedProjectRoot, system.LatestMixSnapshot(), persistenceDiagnostic);
	};
	if(!persistenceDiagnostic.empty())
		ImGui::TextWrapped("%s", persistenceDiagnostic.c_str());
	auto snapshot = system.LatestMixSnapshot();
	float master = snapshot.master.volume;
	bool masterMuted = snapshot.master.muted;
	if(ImGui::SliderFloat("Master Volume", &master, 0.0F, 1.0F) || ImGui::Checkbox("Master Mute", &masterMuted)) {
		std::string diagnostic;
		if(system.SetMasterMix(master, masterMuted, diagnostic)) persist();
	}
	ImGui::Separator();
	ImGui::InputText("New Group", newGroupName.data(), newGroupName.size());
	if(ImGui::Button("Add Group") && newGroupName[0] != '\0') {
		TerraEngine::Audio::AudioMixGroupState group;
		group.id = TerraEngine::GenerateUUID(); group.name = newGroupName.data();
		snapshot.groups.push_back(std::move(group));
		std::string diagnostic;
		if(system.ReplaceMixSnapshot(std::move(snapshot), diagnostic)) { newGroupName.fill('\0'); persist(); }
	}
	for(const auto& group : system.LatestMixSnapshot().groups) {
		ImGui::PushID(TerraEngine::ToHexString(group.id).c_str());
		std::array<char, 65U> name{};
		std::memcpy(name.data(), group.name.data(), std::min(group.name.size(), name.size() - 1U));
		if(ImGui::InputText("Name", name.data(), name.size())) {
			std::string diagnostic;
			if(system.RenameMixGroup(group.id, name.data(), diagnostic)) persist();
		}
		float volume = group.volume; bool muted = group.muted;
		if(ImGui::SliderFloat("Volume", &volume, 0.0F, 1.0F)) {
			std::string diagnostic; if(system.SetMixGroupVolume(group.id, volume, diagnostic)) persist();
		}
		ImGui::SameLine();
		if(ImGui::Checkbox("Mute", &muted)) { std::string diagnostic; if(system.SetMixGroupMuted(group.id, muted, diagnostic)) persist(); }
		ImGui::PopID();
	}
}
