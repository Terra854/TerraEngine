#pragma once
#include <cstdint>
#include <vector>
namespace TerraEngine::Renderer
{
	enum class VirtualGeometryViewPolicy : std::uint8_t
	{
		Camera,
		Shadow,
		Capture
	};
	struct VirtualGeometryInstanceSnapshot
	{
		std::uint64_t instanceId = 0, rootClusterId = 0, generation = 0;
	};
	struct VirtualGeometryWorldHierarchySnapshot
	{
		std::uint64_t frameId = 0;
		VirtualGeometryViewPolicy policy = VirtualGeometryViewPolicy::Camera;
		std::vector<VirtualGeometryInstanceSnapshot> instances;
	};
} // namespace TerraEngine::Renderer
