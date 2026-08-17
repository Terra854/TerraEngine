// TerrainAutomationProvider.cpp -- Canonical terrain automation descriptors.
#include "TerrainAutomationProvider.h"
#include <utility>
namespace TerraEngine::Editor::Automation
{
	TerrainAutomationProvider::TerrainAutomationProvider(AutomationHandlerResolver resolver) : resolveHandler(std::move(resolver)) {}
	std::string_view TerrainAutomationProvider::GetProviderId() const { return "terrain"; }
	std::vector<AutomationCapabilityRegistration> TerrainAutomationProvider::GetCapabilities()
	{
		std::vector<AutomationCapabilityRegistration> result;
		// Each route resolves through the same typed adapter/window boundary. The
		// provider names a capability only when the dispatcher can return a truthful
		// receipt or an explicit prerequisite failure; discovery never invents work.
		for(const std::string_view name : {"terrain.asset.inspect", "terrain.create", "terrain.import_heightmap",
			"terrain.session.begin", "terrain.session.get", "terrain.session.cancel", "terrain.session.commit",
			"terrain.stroke.apply", "terrain.layers.set", "terrain.holes.set", "terrain.operation.start",
			"terrain.rebuild", "terrain.job.get", "terrain.job.cancel", "terrain.diagnostics.get",
			"terrain.validate", "terrain.export", "terrain.save"})
		{
			AutomationCapabilityDescriptor descriptor; descriptor.name=std::string(name); descriptor.category="terrain"; descriptor.description="Canonical, generation-checked terrain operation.";
			descriptor.inputSchema={{"type","object"},{"additionalProperties",false}}; descriptor.resultSchema={{"type","object"}};
			const bool read=name=="terrain.asset.inspect" || name=="terrain.session.get"
				|| name=="terrain.job.get" || name=="terrain.diagnostics.get";
			descriptor.permission=read?AutomationPermission::Read:AutomationPermission::ProjectWrite;
			descriptor.approvalPolicy=(name=="terrain.import_heightmap"||name=="terrain.export"||name=="terrain.rebuild")?AutomationApprovalPolicy::HighImpact:AutomationApprovalPolicy::None;
			descriptor.mutationMode=read?MutationMode::ReadOnly:MutationMode::AssetMutation; descriptor.availability.requiresProject=true;
			descriptor.execution.workerPreparation=!read; descriptor.execution.protectedCommit=!read; descriptor.execution.visibilityBoundary=read?"immutable_terrain_snapshot":"terrain_authoring_publish";
			descriptor.execution.preparationReads={"TerrainAssetRoots"}; descriptor.execution.commitWrites=read?std::vector<std::string>{}:std::vector<std::string>{"TerrainPreparedTransactions","TerrainPublishedTransactions"};
			result.push_back({std::move(descriptor), resolveHandler(name), {}});
		}
		return result;
	}
}
