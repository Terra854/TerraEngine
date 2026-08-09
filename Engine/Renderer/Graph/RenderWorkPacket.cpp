#include "RenderWorkPacket.h"

namespace TerraEngine::Renderer
{
	const char* ToString(RenderBackendWorkKind workKind)
	{
		switch (workKind)
		{
		case RenderBackendWorkKind::Geometry: return "geometry";
		case RenderBackendWorkKind::Shadow: return "shadow";
		case RenderBackendWorkKind::UI: return "ui";
		case RenderBackendWorkKind::Culling: return "culling";
		case RenderBackendWorkKind::Descriptor: return "descriptor";
		case RenderBackendWorkKind::Upload: return "upload";
		case RenderBackendWorkKind::Transition: return "transition";
		case RenderBackendWorkKind::Validation: return "validation";
		case RenderBackendWorkKind::Stats: return "stats";
		default: return "unknown";
		}
	}
}
