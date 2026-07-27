// MotionDatabaseWindow.h - checked motion-match sample and candidate surface.
#pragma once

#include "../Framework/EditorWindow.h"
#include "MotionDatabaseAuthoringHost.h"

#include <memory>

class MainEditorSubsystem;

class MotionDatabaseWindow final : public EditorWindow {
public:
	explicit MotionDatabaseWindow(MainEditorSubsystem* owner = nullptr);
	~MotionDatabaseWindow() override;
	void SetAuthoringHost(TerraEngine::Editor::Animation::MotionDatabaseAuthoringHost* host);
	// Runtime diagnostics can bind their copied query/result packet here. The
	// window retains a complete snapshot, including generation and score details.
	void SetRuntimeQueryPreview(
		TerraEngine::AssetPipeline::MotionDatabaseQueryPreviewState snapshot);

protected:
	void OnGui() override;

private:
	struct State;
	MainEditorSubsystem* owner_ = nullptr;
	TerraEngine::Editor::Animation::MotionDatabaseAuthoringHost* authoringHost_ = nullptr;
	std::unique_ptr<State> state_;
};
