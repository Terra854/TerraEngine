// RendererRecoveryEditorTestFixtures.h - editor-facing recovery test helpers.
//
// Editor recovery UI and automation tests need stable labels for the same
// renderer states used by engine tests. Keeping the helper CPU-only avoids any
// dependency on ImGui, GLFW, or a live graphics backend.

#pragma once

#include "Renderer/RendererRecovery.h"

namespace TerraEngine::EditorTests
{
	inline TerraEngine::Renderer::RendererRecoverySnapshot MakeEditorRendererRecoverySnapshot()
	{
		return TerraEngine::Renderer::RecordDeviceLossIncident(
			TerraEngine::Renderer::MakeActiveRecoverySnapshot(11u),
			"VK_ERROR_DEVICE_LOST",
			"editor present",
			99u,
			11u,
			TerraEngine::Renderer::RendererRecoveryAffectedView::SceneView,
			"scene-view-target",
			"Scene View Target");
	}
}
