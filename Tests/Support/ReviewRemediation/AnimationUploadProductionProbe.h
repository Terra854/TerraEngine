#pragma once

#include "Renderer/Graph/RenderFramePublication.h"
#include <filesystem>

class VulkanContext;
namespace TerraEngine::Tests::Review
{
	// The implementation lives with the one existing VulkanTextureReviewAccess
	// friend. Callers receive copied facts, never private native owner pointers.
	struct AnimationUploadProductionObservation
	{
		bool published = false;
		bool recorded = false;
		bool completed = false;
		bool uploadAndWorkValuesMatch = false;
		bool deformedValuesMatch = false;
		std::uint32_t workCount = 0;
	};
	AnimationUploadProductionObservation ExerciseAnimationUploadProduction(
		VulkanContext& context,
		const Renderer::RenderBackendWorkPublication& publication,
		const std::filesystem::path& artifacts);
}
