#include "TestHarness.h"
#include "Renderer/RHI/Vulkan/VulkanTextureDescriptorRetirement.h"
#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace TerraEngine::Tests
{
	void RegisterVulkanTextureDescriptorRetirementTests(TestRegistry& registry)
	{
		using namespace Renderer::Vulkan;
		registry.Add("Renderer.Efficiency.TextureRetirement.RewritesEveryFrameBeforeOwnerDestruction", [](TestContext& context) {
			const TextureDescriptorRetirementProof proof{true,true,true,true,true,true,true,3,1024};
			const std::array<std::uint32_t, 2> slots{6,7};
			std::vector<std::pair<std::uint32_t,std::uint32_t>> writes;
			bool destroyed = false;
			if (RetireTextureDescriptorSlots(proof, slots, [&](auto slot, auto frame) {
				writes.emplace_back(slot, frame); return true;
			}, [](auto){return true;})) destroyed = true;
			context.Expect(destroyed && writes.size() == 6, "Every actual frame set must stop naming retired owners before destruction.");
			for (std::uint32_t slot : slots)
				for (std::uint32_t frame=0; frame<3; ++frame)
					context.Expect(std::count(writes.begin(), writes.end(), std::pair{slot,frame}) == 1, "Each retired slot/frame must receive the fallback exactly once.");
			writes.clear(); destroyed = false;
			if (RetireTextureDescriptorSlots(proof, slots, [&](auto slot, auto frame) {
				writes.emplace_back(slot,frame); return frame != 1;
			}, [](auto){return true;})) destroyed = true;
			context.Expect(!destroyed && writes.size() == 2, "A failed descriptor write must prevent owner destruction and stop further writes.");
			std::uint32_t samplerWrites=0;
			context.Expect(RetireTextureDescriptorSlots(proof, {}, [](auto,auto){return true;},
				[&](auto){++samplerWrites; return true;}) && samplerWrites==3,
				"Even zero 2D slots must retire the shared point sampler across every frame set.");
			destroyed=false;
			if (RetireTextureDescriptorSlots(proof, {}, [](auto,auto){return true;}, [](auto){return false;})) destroyed=true;
			context.Expect(!destroyed, "A rejected shared sampler write must prevent point owner destruction.");
		});
		registry.Add("Renderer.Efficiency.TextureRetirement.ShrinkGrowPreserveSemanticSlots", [](TestContext& context) {
			std::uint32_t next=20, allocations=0;
			auto allocate=[&] { ++allocations; return next++; };
			std::array<std::uint32_t,3> assigned{6,7,0};
			for (std::size_t i=0; i<2; ++i)
				assigned[i]=AcquireRetainedTextureDescriptorSlot(assigned[i],1024,allocate);
			context.Expect(assigned[0]==6 && assigned[1]==7 && allocations==0, "Live semantic shadow slots must retain their shader-visible indices.");
			// Shrinking keeps inactive slots reserved and fallback-bound; regrowing
			// the same semantic slot must not steal another texture's heap index.
			context.Expect(AcquireRetainedTextureDescriptorSlot(assigned[1],1024,allocate)==7, "Regrowth must reuse the reserved slot.");
			assigned[2]=AcquireRetainedTextureDescriptorSlot(assigned[2],1024,allocate);
			context.Expect(assigned[2]==20 && allocations==1, "Only a previously unassigned slot may allocate.");
			context.Expect(AcquireRetainedTextureDescriptorSlot(1024,1024,allocate)==0, "Stale out-of-range ownership must reject without allocation.");
			context.Expect(AcquireRetainedTextureDescriptorSlot(0,1024,[]{return 0u;})==0, "Heap exhaustion must not publish the reserved white slot as a shadow assignment.");
		});
		registry.Add("Renderer.Efficiency.TextureRetirement.InvalidFallbackOrCompletionRejectsBeforeWrites", [](TestContext& context) {
			const TextureDescriptorRetirementProof valid{true,true,true,true,true,true,true,3,1024};
			const std::array<std::uint32_t,1> slots{6};
			for (int fault=0; fault<8; ++fault) {
				auto proof=valid;
				if(fault==0) proof.deviceReady=false;
				if(fault==1) proof.completionProven=false;
				if(fault==2) proof.setsReady=false;
				if(fault==3) proof.fallbackImage=false;
				if(fault==4) proof.fallbackView=false;
				if(fault==5) proof.fallbackSampler=false;
				if(fault==6) proof.fallbackShaderReadable=false;
				if(fault==7) proof.frameSetCount=0;
				int writes=0;
				context.Expect(!RetireTextureDescriptorSlots(proof,slots,[&](auto,auto){++writes;return true;},[&](auto){++writes;return true;}) && writes==0, "Invalid actual owner/completion admission must perform no writes.");
			}
			const std::array<std::uint32_t,2> stale{6,1024};
			int writes=0;
			context.Expect(!RetireTextureDescriptorSlots(valid,stale,[&](auto,auto){++writes;return true;},[&](auto){++writes;return true;}) && writes==0, "Validate the entire retirement list before partial writes.");
		});
	}
}
