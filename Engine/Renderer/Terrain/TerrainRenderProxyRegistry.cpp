// TerrainRenderProxyRegistry.cpp -- Cross-module terrain render binding storage.

#include "Renderer/Terrain/TerrainRenderProxyRegistry.h"

namespace TerraEngine::Renderer::Terrain
{
	TerrainRenderProxyRegistry& TerrainRenderProxyRegistry::Instance()
	{
		// This function is compiled and exported by the Engine DLL so native engine
		// systems and editor windows always receive the same process-wide registry.
		static TerrainRenderProxyRegistry registry;
		return registry;
	}
}
