// Validation-only observation of the unchanged renderer publication owner.
// Compile this header in the collector's single translation unit against the
// complete headers that match its loaded engine DLL, including private layout.
#pragma once

#include "Renderer/RenderSystem.h"
#include "Renderer/Graph/RenderFramePublication.h"

namespace
{
	// C++20 [temp.spec] permits a private name in an explicit-instantiation
	// argument. The ADL friend declaration is required before the bridge defines
	// it. This observes the existing member without changing production classes,
	// calculating offsets, introducing an export, or granting mutable access.
	struct ReplayPublicationTag
	{
		using Member = TerraEngine::Renderer::RenderFramePublicationStore RenderSystem::*;
		friend Member ReplayPublicationMember(ReplayPublicationTag);
	};
	template<ReplayPublicationTag::Member Member>
	struct ReplayPublicationBridge
	{
		friend ReplayPublicationTag::Member ReplayPublicationMember(ReplayPublicationTag)
		{
			return Member;
		}
	};
	template struct ReplayPublicationBridge<&RenderSystem::renderFramePublicationStore>;

	// The collector calls only after a scheduler operation has settled. A strong
	// const root keeps every page alive through projection and held-reader checks;
	// roots can span shutdown for lifetime probes, but must precede DLL unloading.
	[[nodiscard]] std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication>
	LoadReplayRenderPublication(const RenderSystem& renderer)
	{
		return (renderer.*ReplayPublicationMember(ReplayPublicationTag{})).Load();
	}
}
