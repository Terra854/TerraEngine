#include "VirtualGeometryViewPolicy.h"
namespace TerraEngine::Renderer
{
	namespace
	{
		bool MayOwnOcclusionHistory(const VirtualGeometryViewKind kind) noexcept
		{
			// Camera and editor views repeatedly render the same interactive target.
			// Shadow, capture, and reflection targets can change projection or content
			// independently, so they always begin with a conservative main pass.
			return kind == VirtualGeometryViewKind::Camera
				|| kind == VirtualGeometryViewKind::Editor;
		}
	}

	VirtualGeometryViewPolicyDecision EvaluateVirtualGeometryViewPolicy(const VirtualGeometryViewPolicyState &v, const VirtualGeometryViewPolicyState &owner)
	{
		VirtualGeometryViewPolicyDecision d{};
		d.sharesGeometry = v.targetGeneration == owner.targetGeneration
			&& (v.geometryGeneration == 0u || owner.geometryGeneration == 0u || v.geometryGeneration == owner.geometryGeneration)
			&& (v.residencyGeneration == 0u || owner.residencyGeneration == 0u || v.residencyGeneration == owner.residencyGeneration);
		d.historyOwnerViewId = v.historyOwnerViewId == 0u ? v.viewId : v.historyOwnerViewId;
		d.ownsHistory = MayOwnOcclusionHistory(v.kind)
			&& d.historyOwnerViewId == v.viewId;
		d.historyValid = d.ownsHistory && d.sharesGeometry && !v.cameraCut
			&& !v.resized && !v.teleported && d.historyOwnerViewId == owner.viewId
			&& v.projectionGeneration == owner.projectionGeneration;
		if(v.cameraCut) d.invalidation = VirtualGeometryHistoryInvalidation::CameraCut;
		else if(v.resized) d.invalidation = VirtualGeometryHistoryInvalidation::Resize;
		else if(v.teleported) d.invalidation = VirtualGeometryHistoryInvalidation::Teleport;
		else if(v.projectionGeneration != owner.projectionGeneration) d.invalidation = VirtualGeometryHistoryInvalidation::ProjectionChanged;
		else if(!d.ownsHistory || d.historyOwnerViewId != owner.viewId) d.invalidation = VirtualGeometryHistoryInvalidation::OwnerChanged;
		else if(!d.sharesGeometry) d.invalidation = VirtualGeometryHistoryInvalidation::GenerationChanged;
		d.pass = d.historyValid ? VirtualGeometryOcclusionPass::Post : VirtualGeometryOcclusionPass::Main;
		return d;
	}
} // namespace TerraEngine::Renderer
