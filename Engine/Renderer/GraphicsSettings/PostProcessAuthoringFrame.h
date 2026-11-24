// Immutable, native-prepared authoring state for one exact camera view.
#pragma once
#include "GraphicsSettingsTypes.h"
#include "Renderer/Graph/PostProcessCustomPass.h"
#include <memory>

namespace TerraEngine::Renderer::GraphicsSettings
{
	struct PostProcessAuthoringFrame
	{
		GraphicsTarget target;
		GraphicsLifecycleStamp lifecycle;
		GraphicsRevision authoringRevision = 0u;
		GraphicsRevision inheritedRevision = 0u;
		GraphicsRevision sourceSettingsRevision = 0u;
		std::uint64_t authoredResourceRevision = 0u;
		std::uint64_t preparedFromFrameId = 0u;
		std::shared_ptr<const GraphicsSettingsSnapshot> settings;
		std::shared_ptr<const PreparedPostProcessCustomPassSet> effects;
		bool HasEnabledEffects() const noexcept;
	};
}
