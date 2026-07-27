// AnimationControllerWindow.h - checked controller graph authoring catalogue.
#pragma once

#include "../Framework/EditorWindow.h"

#include <memory>
#include <string>
#include <vector>

class MainEditorSubsystem;

class AnimationControllerWindow final : public EditorWindow {
public:
	explicit AnimationControllerWindow(MainEditorSubsystem* owner = nullptr);
	~AnimationControllerWindow() override;
	void SetGraphNodes(std::vector<std::string> nodes);

protected:
	void OnGui() override;

private:
	struct State;
	MainEditorSubsystem* owner_ = nullptr;
	std::unique_ptr<State> state_;
};
