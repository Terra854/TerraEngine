// RendererRecoveryTestFixtures.h - small CPU-only helpers for recovery tests.
//
// The recovery tests should not require a Vulkan device. These helpers build
// deterministic snapshots so tests can focus on state transitions and policy
// decisions while staying fast inside the headless engine test executable.

#pragma once

#include "Renderer/RendererRecovery.h"

namespace TerraEngine::Tests
{
	inline TerraEngine::Renderer::RendererRecoverySnapshot MakeLatchedRendererDeviceLoss(std::uint64_t frameId = 42u)
	{
		return TerraEngine::Renderer::RecordDeviceLossIncident(
			TerraEngine::Renderer::MakeActiveRecoverySnapshot(7u),
			"VK_ERROR_DEVICE_LOST",
			"frame submit",
			frameId,
			7u,
			TerraEngine::Renderer::RendererRecoveryAffectedView::GameView,
			"test-resource",
			"Test Resource");
	}
}
