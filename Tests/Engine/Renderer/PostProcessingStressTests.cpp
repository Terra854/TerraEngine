// Deterministic CPU owner stress. This is not a Vulkan/image-quality fixture:
// the driver supplies completion events to real controller/history owners.
#include "TestHarness.h"
#include "Renderer/Graph/RenderFramePublication.h"
#include "Renderer/GraphicsSettings/GraphicsSettingsController.h"
#include "Renderer/RHI/Vulkan/VulkanPostProcessHistory.h"
#include <algorithm>
#include <array>
#include <memory>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace Renderer;
		using namespace Renderer::GraphicsSettings;
		using namespace Renderer::Vulkan;
		struct StressOwners
		{
			GraphicsSettingsController controller;
			PostProcessHistory history;
			std::array<GraphicsSettingDescriptor,3> catalog{};
			std::array<GraphicsTarget,2> targets{};
			std::array<GraphicsCameraTarget,2> cameras{};
			std::array<PostProcessHistorySignature,2> signatures{};
			GraphicsPreparationContext context;
			std::uint64_t token=0,epoch=0;
			StressOwners()
			{
				catalog[0].id=GraphicsSettingId::FixedExposureCompensation; catalog[0].valueKind=GraphicsValueKind::Scalar;
				catalog[0].defaultValue=0.0f; catalog[0].minimum=-8.0f; catalog[0].maximum=8.0f;
				catalog[1].id=GraphicsSettingId::ColorGradingEnabled; catalog[1].defaultValue=false;
				catalog[2].id=GraphicsSettingId::AntiAliasingMode; catalog[2].valueKind=GraphicsValueKind::Unsigned32;
				catalog[2].defaultValue=std::uint32_t{0}; catalog[2].options={0u,1u,2u,3u}; catalog[2].activation=GraphicsActivation::Rebuild;
				for(auto& descriptor:catalog) { descriptor.available=true; descriptor.gameScope=true; descriptor.cameraScope=true; }
				for(std::size_t index=0;index!=2;++index)
				{
					cameras[index].cameraId[0]=static_cast<std::uint8_t>(index+1u);
					cameras[index].sceneGeneration=cameras[index].objectLifetimeGeneration=cameras[index].componentGeneration=cameras[index].bindingGeneration=1;
					targets[index]={GraphicsScope::Camera,cameras[index],GraphicsViewIdentity{RenderViewType::Game,2u,index+1u,RenderOutputKind::GameView}};
					signatures[index]={640,360,640,360,97,126,83,1,1,1,1,1,1,1};
				}
				context={{1,1,1,1},catalog,targets,1}; controller.Invalidate(context,cameras);
			}
			GraphicsSettingsRequest Request(std::size_t index,float ev,bool grade) const
			{
				GraphicsSettingsRequest request; request.lifecycle=context.lifecycle; request.target=targets[index];
				request.patches={{GraphicsSettingId::FixedExposureCompensation,ev},{GraphicsSettingId::ColorGradingEnabled,grade}}; return request;
			}
			void Seed(TestContext& test,std::size_t index,float ev=0.0f,bool grade=false)
			{
				controller.Enqueue(Request(index,ev,grade)); auto candidate=controller.Prepare(context);
				test.Expect(candidate.has_value(),"Fresh live camera must prepare.");
				test.Expect(controller.Publish(std::move(*candidate),context).state==GraphicsRequestState::Applied,"Fresh camera must publish atomically.");
			}
			PostProcessHistoryScope Scope(std::size_t index) const { return {targets[index],context.lifecycle}; }
			std::shared_ptr<const RenderFramePublication> Frame(std::uint64_t frame) const
			{
				auto publication=std::make_shared<RenderFramePublication>(); publication->frameId=frame;
				publication->rendererSessionGeneration=context.lifecycle.rendererSessionGeneration;
				publication->backendResourceGeneration=context.lifecycle.backendResourceGeneration;
				auto dynamic=std::make_shared<RenderDynamicStateRoot>(); dynamic->generation=frame;
				auto page=std::make_shared<RenderDynamicPage>(); page->generation=frame;
				for(std::size_t index=0;index!=2;++index)
				{
					RenderViewPresentationInput view; view.graphicsSettings=controller.Snapshot(targets[index]);
					view.graphicsCamera=targets[index].camera; view.graphicsView=targets[index].view;
					view.output.kind=RenderOutputKind::GameView; view.output.diagnosticsView=RenderViewType::Game; view.output.outputId=2;
					view.output.targetSize={signatures[index].outputWidth,signatures[index].outputHeight}; publication->presentationInputs.push_back(std::move(view));
					// Typed immutable pose generations stand in for animation ingress,
					// not for an evaluated GPU motion image or animation-system result.
					RenderDynamicLane lane; lane.animation.poseGeneration=frame; lane.animation.stableInstanceId=index+1;
					lane.instance.modelMatrix[3][0]=static_cast<float>(frame+index);
					lane.previousModelMatrix[3][0]=static_cast<float>(frame-1u+index); page->lanes.push_back(lane);
				}
				dynamic->pages.push_back(page); publication->dynamic=dynamic; return publication;
			}
		};
		GraphicsValue Read(const GraphicsSettingsSnapshot& snapshot,GraphicsSettingId id)
		{
			const auto found=std::find_if(snapshot.values.begin(),snapshot.values.end(),[&](const auto& value) { return value.id==id; });
			if(found==snapshot.values.end()) throw TestFailure("Expected committed setting is missing."); return found->value;
		}
	}
	void RegisterPostProcessingStressTests(TestRegistry& registry)
	{
		registry.Add("Renderer.PostProcess.Stress.HundredChangesTwoViewsCpuContract",[](TestContext& test)
		{
			StressOwners owners; owners.Seed(test,0); owners.Seed(test,1);
			struct Captured { std::shared_ptr<const GraphicsSettingsSnapshot> snapshot; std::vector<GraphicsSettingValue> values; };
			std::vector<Captured> snapshots;
			std::vector<std::shared_ptr<const RenderFramePublication>> frames;
			std::uint64_t lastSequence=0; std::size_t applied=0,rejected=0,failed=0;
			for(std::uint32_t change=0;change!=100u;++change)
			{
				std::array<bool,2> resetHistory{change==0u,change==0u};
				owners.context.frameId=change+10u; const std::size_t selected=change%2u,other=1u-selected;
				if(change!=0u && change%25u==0u)
				{
					// A backend recovery cancels a real prepared request and retires
					// history eligibility without mutating captured immutable roots.
					const auto stale=owners.controller.Enqueue(owners.Request(selected,1.0f,true));
					const auto candidate=owners.controller.Prepare(owners.context); test.Expect(candidate.has_value(),"Recovery must begin with actual prepared work.");
					const std::array committed{owners.controller.Snapshot(owners.targets[0]),owners.controller.Snapshot(owners.targets[1])};
					for(std::size_t view=0;view!=2;++view) owners.history.Retire(owners.Scope(view));
					++owners.context.lifecycle.backendResourceGeneration; owners.controller.Invalidate(owners.context,owners.cameras);
					const auto receipt=owners.controller.GetReceipt(stale.requestId);
					test.Expect(receipt && receipt->state==GraphicsRequestState::Cancelled && receipt->reason==GraphicsRequestReason::StaleLifetime,"Recovery cancels old backend work.");
					for(std::size_t view=0;view!=2;++view)
					{
						test.Expect(owners.controller.CameraOwnerSnapshot(owners.cameras[view])->values==committed[view]->values,"Backend recovery retains committed camera intent.");
						owners.Seed(test,view,std::get<float>(Read(*committed[view],GraphicsSettingId::FixedExposureCompensation)),std::get<bool>(Read(*committed[view],GraphicsSettingId::ColorGradingEnabled)));
						resetHistory[view]=true;
					}
				}
				if(change==40u || change==80u)
				{
					owners.history.Retire(owners.Scope(selected)); ++owners.targets[selected].view->lifetimeGeneration;
					owners.signatures[selected].internalWidth=owners.signatures[selected].outputWidth=800;
					owners.signatures[selected].internalHeight=owners.signatures[selected].outputHeight=450;
					owners.controller.Invalidate(owners.context,owners.cameras); owners.Seed(test,selected);
					resetHistory[selected]=true;
				}
				if(change==60u)
				{
					const auto retired=owners.targets[selected]; owners.history.Retire(owners.Scope(selected));
					++owners.cameras[selected].objectLifetimeGeneration; ++owners.cameras[selected].componentGeneration;
					owners.targets[selected].camera=owners.cameras[selected]; ++owners.targets[selected].view->lifetimeGeneration;
					owners.controller.Invalidate(owners.context,owners.cameras);
					test.Expect(!owners.controller.Snapshot(retired),"Retired camera lifetime cannot resolve a replacement owner's publication."); owners.Seed(test,selected);
					resetHistory[selected]=true;
				}
				const auto before=owners.controller.Snapshot(owners.targets[selected]),untouched=owners.controller.Snapshot(owners.targets[other]);
				test.Expect(before && untouched,"Both live camera views must have coherent publications."); snapshots.push_back({before,before->values});
				const bool invalid=change%11u==0u,rebuild=change%13u==0u && !invalid,fail=rebuild && change%26u==0u;
				auto request=owners.Request(selected,invalid ? 99.0f : static_cast<float>(static_cast<int>(change%9u)-4)*.25f,(change/2u)%2u!=0u);
				if(rebuild) request.patches.push_back({GraphicsSettingId::AntiAliasingMode,std::uint32_t{1u+change%3u}});
				const auto admission=owners.controller.Enqueue(request); test.Expect(admission.sequence>lastSequence,"Admission sequence must stay strictly ordered."); lastSequence=admission.sequence;
				std::optional<GraphicsRequestReceipt> probe;
				if(change%10u==0u) probe=owners.controller.Enqueue(owners.Request(other,99.0f,true));
				auto candidate=owners.controller.Prepare(owners.context);
				if(invalid)
				{
					test.Expect(!candidate,"Mixed invalid batch cannot prepare a partial valid grade update."); ++rejected;
					const auto receipt=owners.controller.GetReceipt(admission.requestId);
					test.Expect(receipt && receipt->state==GraphicsRequestState::Rejected && receipt->reason==GraphicsRequestReason::InvalidValue,"Invalid mixed batch gets a terminal typed rejection.");
				}
				else
				{
					test.Expect(candidate && owners.controller.Snapshot(owners.targets[selected])==before,"Preparation cannot make requested values visible.");
					test.Expect(!owners.controller.Prepare(owners.context),"A prepared FIFO head blocks the queued overlap probe.");
					// Only the resource completion outcome is supplied by this CPU
					// driver; real controller certificate/lifetime validation still runs.
					const auto receipt=owners.controller.Publish(std::move(*candidate),owners.context,!fail,
						rebuild && !fail ? owners.context.lifecycle.backendResourceGeneration : 0u);
					if(fail) { ++failed; test.Expect(receipt.state==GraphicsRequestState::Rejected && receipt.reason==GraphicsRequestReason::PreparationFailed,"Failed rebuild reports real controller failure."); }
					else
					{
						++applied; test.Expect(receipt.state==GraphicsRequestState::Applied && receipt.appliedFrameId==owners.context.frameId,"Successful batch publishes only at the declared boundary.");
						if(rebuild) { ++owners.signatures[selected].reconstructionGeneration; resetHistory[selected]=true; }
					}
				}
				if(probe) { owners.controller.Prepare(owners.context); const auto receipt=owners.controller.GetReceipt(probe->requestId); test.Expect(receipt && receipt->state==GraphicsRequestState::Rejected,"Queued invalid overlap cannot change the other camera."); }
				const auto after=owners.controller.Snapshot(owners.targets[selected]);
				test.Expect(owners.controller.Snapshot(owners.targets[other])==untouched,"One camera's ordered batch cannot publish into another camera sharing physical output2.");
				if(invalid || fail) test.Expect(after==before,"Rejected or failed rebuild must preserve the complete old root.");
				else for(const auto& patch:request.patches) test.Expect(Read(*after,patch.id)==patch.value,"Every field of one accepted batch must agree in the same immutable root.");
				const auto frame=owners.Frame(change+1u); frames.push_back(frame);
				for(std::size_t view=0;view!=2;++view)
				{
					const auto scope=owners.Scope(view); const auto previous=owners.history.Previous(scope,owners.signatures[view]);
					test.Expect(resetHistory[view] ? !previous : previous && previous->frameId==change,
						"Only the affected view/resource/reconstruction lifetime resets its compatible predecessor.");
					const auto record=owners.history.Record(++owners.token,scope,owners.signatures[view],frame,static_cast<std::uint32_t>(view),frame->frameId,change%8u);
					test.Expect(record && record->previous==previous && record->current->publication==frame,"History records the exact current root and compatible accepted predecessor.");
					test.Expect(owners.history.Previous(scope,owners.signatures[view])==previous,"Recording alone cannot advance visible history.");
					test.Expect(owners.history.Accept(owners.token),"CPU submission-event driver can accept its exact recording token.");
					test.Expect(owners.history.AttachCompletion(owners.token,static_cast<std::uint32_t>(view),++owners.epoch),"Only the new completion epoch owns this accepted use.");
					owners.history.Complete(static_cast<std::uint32_t>(view),owners.epoch);
				}
			}
			test.Expect(applied+rejected+failed==100u && applied>0 && rejected>0 && failed>0,"Exactly100 primary changes must include successful, rejected, and failed-rebuild paths.");
			for(const auto& saved:snapshots) test.Expect(saved.snapshot->values==saved.values,"Subsequent changes/recovery cannot mutate captured settings roots.");
			for(std::size_t index=0;index!=frames.size();++index)
				test.Expect(frames[index]->dynamic->pages[0]->lanes[0].animation.poseGeneration==index+1u
					&& frames[index]->dynamic->pages[0]->lanes[0].previousModelMatrix[3][0]==static_cast<float>(index),"Retained animated pose generations and previous transforms stay immutable.");
			test.Expect(!owners.controller.HasPendingWork() && owners.history.RetainedUseCount()==0u,"Completed stress leaves no pending controller or completion-owned history use.");
		});
	}
}
