#include "Renderer/RHI/Vulkan/VulkanPostProcessMotionPreparation.h"
#include "Renderer/Graph/RenderFramePublication.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <unordered_map>
#include <utility>

namespace TerraEngine::Renderer::Vulkan
{
	static_assert(sizeof(PostProcessMotionViewData)==192 && offsetof(PostProcessMotionViewData,jitterUV)==128
		&& offsetof(PostProcessMotionViewData,extents)==144 && offsetof(PostProcessMotionViewData,recordingTokens)==160
		&& offsetof(PostProcessMotionViewData,state)==176);
	static_assert(sizeof(PostProcessMotionDrawData)==160 && offsetof(PostProcessMotionDrawData,previousModel)==64
		&& offsetof(PostProcessMotionDrawData,deformationSelectors)==128 && offsetof(PostProcessMotionDrawData,deformationState)==144);
	static_assert(sizeof(PostProcessMotionCameraOnlyData)==64);
	namespace
	{
		bool Failure(std::string& reason,const char* message) noexcept
		{
			try { reason=message; } catch(...) { reason.clear(); } return false;
		}
		PostProcessMotionDrawIdentity EntityIdentity(PostProcessMotionDrawIdentity value) noexcept
		{
			value.meshId=EMPTY_UUID; value.meshGeneration=0; value.selectedLod=value.firstMeshlet=value.meshletCount=0; return value;
		}
		bool Finite(const glm::mat4& value) noexcept
		{
			for(int column=0;column!=4;++column) for(int row=0;row!=4;++row) if(!std::isfinite(value[column][row])) return false;
			return true;
		}
		bool ValidView(const PostProcessHistoryState& state) noexcept
		{
			if(!state.publication || !state.recordingId || !state.frameId || !state.scope.target.view) return false;
			const auto& root=*state.publication; const auto& scope=state.scope; const auto& signature=state.signature;
			// Publication success lives in SealedRenderFramePublication/the store's
			// returned receipt, not root.receipt (which also carries build counters).
			// The real history owner supplies this immutable root; check its tuple
			// directly without treating that diagnostic field as acceptance proof.
			if(!root.generation || root.frameId!=state.frameId || !scope.target.view->lifetimeGeneration
				|| !scope.lifecycle.rendererSessionGeneration || !scope.lifecycle.backendResourceGeneration
				|| (scope.target.scope==GraphicsSettings::GraphicsScope::Camera && !scope.target.camera)
				|| root.rendererSessionGeneration!=scope.lifecycle.rendererSessionGeneration
				|| root.backendResourceGeneration!=scope.lifecycle.backendResourceGeneration
				|| state.presentationIndex>=root.presentationInputs.size() || state.presentationIndex>=root.views.size()) return false;
			const auto& input=root.presentationInputs[state.presentationIndex]; const auto& selection=root.views[state.presentationIndex];
			if(!input.graphicsSettings) return false;
			// Runtime publications can leave the output extent implicit. In that
			// case the consumed settings carry the exact native output extent.
			const bool matchesNativeRuntimeExtent = input.output.kind == RenderOutputKind::MainRuntimeView
				&& input.output.targetSize == glm::uvec2(0u)
				&& input.graphicsSettings->outputWidth == signature.outputWidth
				&& input.graphicsSettings->outputHeight == signature.outputHeight;
			return input.graphicsView==scope.target.view && input.graphicsCamera==scope.target.camera
				&& input.graphicsSettings && input.graphicsSettings->lifecycle==scope.lifecycle
				&& input.output.kind==scope.target.view->outputKind && input.output.diagnosticsView==scope.target.view->viewType
				&& input.output.outputId==scope.target.view->outputId && selection.outputId==input.output.outputId
				&& selection.outputKind==input.output.kind && selection.viewType==input.output.diagnosticsView
				&& (matchesNativeRuntimeExtent || (input.output.targetSize.x==signature.outputWidth
					&& input.output.targetSize.y==signature.outputHeight))
				&& signature.internalWidth && signature.internalHeight && signature.outputWidth && signature.outputHeight
				&& signature.colorFormat && signature.depthFormat && signature.motionFormat && signature.scaleNumerator && signature.scaleDenominator
				&& signature.reconstructionGeneration && signature.projectionGeneration && signature.cutGeneration && signature.worldOriginGeneration && signature.surfaceGeneration
				&& input.referenceFrame.IsValid() && input.referenceFrame.frameId==state.frameId
				&& Finite(input.viewMatrix) && Finite(input.projectionMatrix);
		}
		struct CapturedDraw
		{
			PostProcessMotionDrawIdentity identity;
			RenderItemRecord command{};
			const RenderDynamicLane* dynamic=nullptr;
			bool resolved=false;
		};
		CapturedDraw Capture(const PostProcessHistoryState& state,std::size_t index)
		{
			CapturedDraw result; const auto& root=*state.publication;
			const auto& input=root.presentationInputs[state.presentationIndex]; const auto& selection=root.views[state.presentationIndex];
			if(index>=input.CommandSelectionIndices().size()) return result;
			const auto visible=input.CommandSelectionIndices()[index]; if(visible>=selection.VisibleOrdinals().size()) return result;
			const auto key=selection.VisibleOrdinals()[visible]; if(!key.IsValid() || !root.persistent || !root.dynamic) return result;
			const auto page=key.ordinal/kRenderPublicationPageCapacity,lane=key.ordinal%kRenderPublicationPageCapacity;
			if(page>=root.persistent->pages.size() || page>=root.dynamic->pages.size()
				|| !root.persistent->pages[page] || !root.dynamic->pages[page]) return result;
			const auto& persistent=*root.persistent->pages[page]; const auto& dynamic=*root.dynamic->pages[page];
			if(lane>=persistent.slots.size() || lane>=dynamic.lanes.size()) return result;
			const auto& slot=persistent.slots[lane];
			if(!slot.IsComplete() || slot.key!=key || !dynamic.lanes[lane].Matches(slot)) return result;
			if(!MakeRenderViewCommandResolver(root,state.presentationIndex).Resolve(index,result.command)) return result;
			result.dynamic=&dynamic.lanes[lane];
			result.identity={key.ordinal,key.lifetimeGeneration,result.command.objectId,result.command.meshId,slot.meshGeneration,
				result.command.objectLifetimeGeneration,result.command.selectedVirtualLod,result.command.virtualMeshletOffset,result.command.virtualMeshletCount};
			result.resolved=true; return result;
		}
		struct IdentityHash
		{
			std::size_t operator()(const PostProcessMotionDrawIdentity& key) const noexcept
			{
				std::size_t hash=std::hash<UUID>{}(key.objectId);
				const auto mix=[&](std::size_t value) { hash^=value+0x9e3779b9u+(hash<<6u)+(hash>>2u); };
				mix(std::hash<UUID>{}(key.meshId)); mix(key.ordinal); mix(key.lifetimeGeneration); mix(key.meshGeneration);
				mix(key.entityLifetimeGeneration); mix(key.selectedLod); mix(key.firstMeshlet); mix(key.meshletCount); return hash;
			}
		};
		bool Deformed(const RenderItemRecord& command) noexcept
		{
			return command.animation.deformablePayloadGeneration || command.animation.poseGeneration
				|| command.animationBinding.deformablePayloadGeneration || command.animationBinding.posePublicationGeneration;
		}
		bool CopyPose(const PostProcessHistoryState& state,const RenderItemRecord& command,PostProcessMotionCpuPoseOwner& output) noexcept
		{
			const auto& binding=command.animationBinding; const auto& generation=command.animation;
			if(!binding.HasCpuDeformation() || !generation.stableInstanceId || !binding.animationAuthorityGeneration || !binding.posePublicationGeneration
				|| (binding.source!=AnimationRenderPoseSource::CpuPose && binding.source!=AnimationRenderPoseSource::HeldCpuPose)
				|| !binding.deformablePayloadGeneration || !binding.skeletonCompatibilityFingerprint
				|| generation.poseGeneration!=binding.posePublicationGeneration || generation.deformablePayloadGeneration!=binding.deformablePayloadGeneration
				|| generation.deformationContentIdLow!=binding.cpuDeformation->contentIdLow
				|| generation.deformationContentIdHigh!=binding.cpuDeformation->contentIdHigh) return false;
			const auto& root=*state.publication;
			if(!root.backendWork || !root.backendWork->valid || root.backendWork->frameId!=state.frameId
				|| !root.backendWork->animationWork.valid || root.backendWork->animationWork.frameId!=state.frameId) return false;
			const auto& work=root.backendWork->animationWork;
			const auto& plan=work.cpuDeformationPlan;
			if(!plan || plan->frameId!=state.frameId || plan->rendererGeneration!=root.rendererSessionGeneration
				|| (work.rendererGeneration && work.rendererGeneration!=plan->rendererGeneration)
				|| binding.cpuDeformationRecordIndex>=plan->records.size()) return false;
			// Resolve only this draw's exact record. Revalidating every unrelated
			// pose for every draw would turn linear worker preparation quadratic.
			const auto& record=plan->records[binding.cpuDeformationRecordIndex];
			if(!record.IsComplete() || record.meshId!=command.meshId || record.stableInstanceId!=generation.stableInstanceId
				|| record.poseGeneration!=binding.posePublicationGeneration || record.deformablePayloadGeneration!=binding.deformablePayloadGeneration
				|| record.rendererGeneration!=plan->rendererGeneration || record.skeletonCompatibilityFingerprint!=binding.skeletonCompatibilityFingerprint
				|| record.data!=binding.cpuDeformation) return false;
			for(const auto& matrix:binding.cpuDeformation->paletteMatrices)
				for(const auto* row:{&matrix.row0,&matrix.row1,&matrix.row2}) for(const auto value:*row) if(!std::isfinite(value)) return false;
			for(const auto weight:binding.cpuDeformation->morphWeights) if(!std::isfinite(weight)) return false;
			output={binding.cpuDeformation,binding.posePublicationGeneration,binding.deformablePayloadGeneration,
				binding.skeletonCompatibilityFingerprint,binding.animationAuthorityGeneration,state.frameId,plan->rendererGeneration,
				generation.stableInstanceId,binding.cpuDeformationRecordIndex}; return true;
		}
	}
	bool TryPreparePostProcessMotion(const PostProcessHistoryRecording& recording,glm::vec2 currentJitterUV,
		glm::vec2 previousJitterUV,bool includeCameraOnly,PreparedPostProcessMotion& output,std::string& reason) noexcept
	{
		if(!recording.current || !ValidView(*recording.current)
			|| !std::isfinite(currentJitterUV.x) || !std::isfinite(currentJitterUV.y)
			|| !std::isfinite(previousJitterUV.x) || !std::isfinite(previousJitterUV.y))
			return Failure(reason,"Motion preparation requires an exact sealed current view and finite raster jitter.");
		try
		{
			PreparedPostProcessMotion prepared; prepared.current=recording.current; prepared.previous=recording.previous;
			const auto& current=*recording.current; const auto& currentView=current.publication->presentationInputs[current.presentationIndex];
			prepared.cpuHistoryCompatible=recording.previous && ValidView(*recording.previous)
				&& current.scope==recording.previous->scope && current.signature==recording.previous->signature
				&& recording.previous->frameId<current.frameId && recording.previous->recordingId<current.recordingId;
			prepared.historyReason=prepared.cpuHistoryCompatible ? PostProcessMotionReason::None
				: recording.previous ? PostProcessMotionReason::IncompatibleHistory : PostProcessMotionReason::MissingPrevious;
			prepared.shader.currentRasterViewProjection=currentView.projectionMatrix*currentView.viewMatrix;
			if(!Finite(prepared.shader.currentRasterViewProjection)) return Failure(reason,"Current raster view-projection overflows its finite representation.");
			prepared.shader.extents={current.signature.internalWidth,current.signature.internalHeight,current.signature.outputWidth,current.signature.outputHeight};
			prepared.shader.jitterUV={currentJitterUV.x,currentJitterUV.y,previousJitterUV.x,previousJitterUV.y};
			prepared.shader.recordingTokens={static_cast<std::uint32_t>(current.recordingId),static_cast<std::uint32_t>(current.recordingId>>32u),0u,0u};
			using Lookup=std::unordered_map<PostProcessMotionDrawIdentity,std::pair<std::size_t,std::size_t>,IdentityHash>;
			Lookup prior,priorEntities,currentCounts; std::vector<CapturedDraw> previousDraws,currentDraws;
			const auto count=MakeRenderViewCommandResolver(*current.publication,current.presentationIndex).Count();
			if(count>std::numeric_limits<std::uint32_t>::max()) return Failure(reason,"Motion selection count exceeds typed row indices.");
			currentDraws.reserve(count); prepared.draws.reserve(count); currentCounts.reserve(count);
			for(std::size_t index=0;index<count;++index)
			{
				currentDraws.push_back(Capture(current,index));
				if(currentDraws.back().resolved) ++currentCounts[currentDraws.back().identity].second;
			}
			if(prepared.cpuHistoryCompatible)
			{
				const auto& previous=*recording.previous; const auto& view=previous.publication->presentationInputs[previous.presentationIndex];
				prepared.shader.previousRasterViewProjection=view.projectionMatrix*view.viewMatrix;
				if(!Finite(prepared.shader.previousRasterViewProjection))
				{
					prepared.cpuHistoryCompatible=false; prepared.historyReason=PostProcessMotionReason::IncompatibleHistory;
					prepared.shader.previousRasterViewProjection=glm::mat4(1.0f);
				}
				prepared.shader.recordingTokens.z=static_cast<std::uint32_t>(previous.recordingId);
				prepared.shader.recordingTokens.w=static_cast<std::uint32_t>(previous.recordingId>>32u);
				const auto size=prepared.cpuHistoryCompatible ? MakeRenderViewCommandResolver(*previous.publication,previous.presentationIndex).Count() : 0u;
				previousDraws.reserve(size); prior.reserve(size); priorEntities.reserve(size);
				for(std::size_t index=0;index<size;++index)
				{
					previousDraws.push_back(Capture(previous,index)); if(!previousDraws.back().resolved) continue;
					auto& entry=prior[previousDraws.back().identity]; entry.first=index; ++entry.second;
					auto& entity=priorEntities[EntityIdentity(previousDraws.back().identity)]; entity.first=index; ++entity.second;
				}
			}
			for(std::size_t index=0;index<count;++index)
			{
				const auto& now=currentDraws[index]; PreparedPostProcessMotionDraw draw;
				draw.selectionIndex=static_cast<std::uint32_t>(index); draw.identity=now.identity; draw.reason=prepared.historyReason;
				const auto finish=[&](PostProcessMotionReason failure) { draw.reason=failure; prepared.draws.push_back(std::move(draw)); };
				if(!now.resolved) { finish(PostProcessMotionReason::UnresolvedDraw); continue; }
				if(currentCounts.at(now.identity).second!=1u) { finish(PostProcessMotionReason::DuplicateIdentity); continue; }
				if(!now.dynamic->hasCanonicalWorldPose) { finish(PostProcessMotionReason::MissingCanonicalPose); continue; }
				const auto currentMatrix=BuildLocalizedRenderPoseMatrices(now.dynamic->canonicalWorldPose,currentView.referenceFrame);
				if(!currentMatrix.valid) { finish(PostProcessMotionReason::UnsafeReference); continue; } draw.shader.currentModel=currentMatrix.currentMatrix;
				if(!Finite(draw.shader.currentModel)) { finish(PostProcessMotionReason::NonFiniteMatrix); continue; }
				const bool currentDeformed=Deformed(now.command);
				if(currentDeformed)
				{
					if(!CopyPose(current,now.command,draw.currentPose)) { finish(PostProcessMotionReason::MissingCpuPose); continue; }
					draw.shader.deformationState.x=1u;
				}
				draw.currentGeometryValid=true;
				// Even a first/reset frame retains the exact current pose needed to
				// place geometry; absent prior motion never means current bind pose.
				if(!prepared.cpuHistoryCompatible) { finish(prepared.historyReason); continue; }
				const auto found=prior.find(now.identity);
				if(found==prior.end())
				{
					const auto entity=priorEntities.find(EntityIdentity(now.identity));
					if(entity==priorEntities.end()) finish(PostProcessMotionReason::EntityChanged);
					else
					{
						const auto& previousIdentity=previousDraws[entity->second.first].identity;
						finish(entity->second.second==1u && (previousIdentity.meshId!=now.identity.meshId || previousIdentity.meshGeneration!=now.identity.meshGeneration)
							? PostProcessMotionReason::MeshChanged : PostProcessMotionReason::TopologyChanged);
					}
					continue;
				}
				if(found->second.second!=1u) { finish(PostProcessMotionReason::DuplicateIdentity); continue; }
				const auto& old=previousDraws[found->second.first];
				if(now.command.geometryRoute!=GeometryRoute::DirectMeshlet || old.command.geometryRoute!=GeometryRoute::DirectMeshlet)
				{ finish(PostProcessMotionReason::UnsupportedGeometry); continue; }
				if(!old.dynamic->hasCanonicalWorldPose) { finish(PostProcessMotionReason::MissingCanonicalPose); continue; }
				const auto& previousView=recording.previous->publication->presentationInputs[recording.previous->presentationIndex];
				const auto previousMatrix=BuildLocalizedRenderPoseMatrices(old.dynamic->canonicalWorldPose,previousView.referenceFrame);
				if(!previousMatrix.valid) { finish(PostProcessMotionReason::UnsafeReference); continue; } draw.shader.previousModel=previousMatrix.currentMatrix;
				if(!Finite(draw.shader.currentModel) || !Finite(draw.shader.previousModel)) { finish(PostProcessMotionReason::NonFiniteMatrix); continue; }
				const bool previousDeformed=Deformed(old.command);
				if(currentDeformed!=previousDeformed) { finish(PostProcessMotionReason::UnsupportedDeformation); continue; }
				if(currentDeformed)
				{
					if(!CopyPose(*recording.previous,old.command,draw.previousPose)) { finish(PostProcessMotionReason::MissingCpuPose); continue; }
					if(draw.currentPose.payloadGeneration!=draw.previousPose.payloadGeneration || draw.currentPose.skeletonFingerprint!=draw.previousPose.skeletonFingerprint
						|| now.command.animation.stableInstanceId!=old.command.animation.stableInstanceId || draw.currentPose.poseGeneration<draw.previousPose.poseGeneration
						|| draw.currentPose.authorityGeneration!=draw.previousPose.authorityGeneration || draw.currentPose.data->jointCount!=draw.previousPose.data->jointCount
						|| draw.currentPose.data->paletteMatrices.size()!=draw.previousPose.data->paletteMatrices.size()
						|| draw.currentPose.data->morphWeights.size()!=draw.previousPose.data->morphWeights.size()) { finish(PostProcessMotionReason::ChangedCpuPoseLayout); continue; }
					draw.shader.deformationState.x=draw.shader.deformationState.y=1u;
				}
				if(includeCameraOnly)
				{
					const auto cameraOnly=BuildLocalizedRenderPoseMatrices(now.dynamic->canonicalWorldPose,previousView.referenceFrame);
					if(!cameraOnly.valid || !Finite(cameraOnly.currentMatrix)) { finish(PostProcessMotionReason::UnsafeReference); continue; }
					draw.cameraOnly.currentModelInPreviousReference=cameraOnly.currentMatrix; draw.cameraOnlyAvailable=true;
				}
				draw.cpuPairValid=true; finish(PostProcessMotionReason::None);
			}
			output=std::move(prepared); reason.clear(); return true;
		}
		catch(...)
		{
			// Temporary maps/rows never replace the caller's previous complete owner
			// on allocation failure. No live renderer or scene state is touched.
			return Failure(reason,"Motion CPU preparation could not allocate its complete owned result.");
		}
	}
}
