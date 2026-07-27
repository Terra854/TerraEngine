// SkeletonEditorWindow.h - checked skeleton hierarchy inspection surface.
#pragma once

#include "../Framework/EditorWindow.h"

#include <memory>
#include <string>
#include <vector>

class MainEditorSubsystem;

class SkeletonEditorWindow final : public EditorWindow {
public:
	explicit SkeletonEditorWindow(MainEditorSubsystem* owner = nullptr);
	~SkeletonEditorWindow() override;
	void SetJoints(std::vector<std::string> joints);
	[[nodiscard]] const std::vector<std::string>& Joints() const noexcept;

protected:
	void OnGui() override;

private:
	struct State;
	MainEditorSubsystem* owner_ = nullptr;
	std::unique_ptr<State> state_;
};
