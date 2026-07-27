// AnimationClipEditorWindow.h - fixed-tick cooked clip timeline surface.
#pragma once

#include "../Framework/EditorWindow.h"

#include <cstdint>
#include <memory>

class MainEditorSubsystem;

class AnimationClipEditorWindow final : public EditorWindow {
public:
	explicit AnimationClipEditorWindow(MainEditorSubsystem* owner = nullptr);
	~AnimationClipEditorWindow() override;
	void SetDuration(std::int64_t ticks) noexcept;
	[[nodiscard]] std::int64_t Cursor() const noexcept;

protected:
	void OnGui() override;

private:
	struct State;
	MainEditorSubsystem* owner_ = nullptr;
	std::unique_ptr<State> state_;
};
