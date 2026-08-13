// AudioMixerWindow.h - flat provider-neutral project mixing controls.

#pragma once

#include "../Framework/EditorWindow.h"

#include <array>
#include <filesystem>
#include <string>

class MainEditorSubsystem;

class AudioMixerWindow final : public EditorWindow {
public:
	explicit AudioMixerWindow(MainEditorSubsystem* owner);
protected:
	void OnGui() override;
private:
	MainEditorSubsystem* owner = nullptr;
	std::array<char, 64U> newGroupName{};
	std::filesystem::path loadedProjectRoot;
	std::string persistenceDiagnostic;
};
