// Native custom chain construction and worker-prepared immutable frame packets.
#include "VulkanPostProcessCustomChain.h"
#include "File Formats/TerraEngineShaderFile.h"
#include <algorithm>
#include <stdexcept>

namespace TerraEngine::Renderer::Vulkan
{
	struct CustomPostProcessChain
	{
		struct Pass
		{
			std::uint32_t ordinal=0;
			std::shared_ptr<const PreparedPostProcessCustomPass> prepared;
			std::shared_ptr<CustomPostProcessPass> native;
			CustomPostProcessImage source,output;
			std::vector<CustomPostProcessBinding> bindings;
		};
		std::shared_ptr<const PreparedPostProcessCustomPassSet> prepared;
		std::array<CustomPostProcessImage,3> stageSources{};
		std::vector<Pass> passes;
	};
	struct CustomPostProcessChainFrame
	{
		struct Pass
		{
			std::vector<std::byte> parameters;
			CustomPostProcessImage source,output;
			std::vector<CustomPostProcessBinding> bindings;
		};
		std::shared_ptr<const CustomPostProcessChain> chain;
		std::uint64_t frameId=0,parameterRevision=0;
		PostProcessHistoryScope scope;
		std::array<CustomPostProcessImage,3> stageSources{};
		std::vector<Pass> passes;
	};
	namespace
	{
		void Require(bool condition,const char* reason) {if(!condition)throw std::runtime_error(reason);}
		std::uint32_t Stage(CustomPostProcessStage stage) noexcept {return static_cast<std::uint32_t>(stage);}
		bool SameOwner(const std::shared_ptr<const void>& a,const std::shared_ptr<const void>& b) noexcept
		{
			return a&&b&&a.get()==b.get()&&!a.owner_before(b)&&!b.owner_before(a);
		}
		// Frame IDs can advance while actual retained descriptor allocations stay
		// identical. A recook, resize, scope transition or replaced owner rebuilds.
		bool SameAllocation(const CustomPostProcessImage& a,const CustomPostProcessImage& b) noexcept
		{
			return a.image&&a.view&&a.image==b.image&&a.view==b.view&&a.format==b.format&&a.usage==b.usage
				&&a.layout==b.layout&&a.samples==b.samples&&a.viewType==b.viewType
				&&a.extent.width==b.extent.width&&a.extent.height==b.extent.height&&a.scope==b.scope
				&&a.targetGeneration==b.targetGeneration&&a.depthEncoding==b.depthEncoding&&a.motionEncoding==b.motionEncoding
				&&a.signedViewNormals==b.signedViewNormals&&a.perceptualRoughness==b.perceptualRoughness&&SameOwner(a.owner,b.owner);
		}
		bool Current(const CustomPostProcessImage& image,const PostProcessHistoryScope& scope,std::uint64_t frame) noexcept
		{
			return image.image&&image.view&&image.owner&&image.frameId==frame&&frame&&image.scope==scope
				&&image.targetGeneration&&image.extent.width&&image.extent.height&&scope.target.view
				&&scope.target.view->lifetimeGeneration&&scope.lifecycle.rendererSessionGeneration
				&&scope.lifecycle.backendResourceGeneration;
		}
		bool LinearSource(const CustomPostProcessImage& image,CustomPostProcessStage stage) noexcept
		{
			const bool hdr=image.format==VK_FORMAT_R16G16B16A16_SFLOAT||image.format==VK_FORMAT_R32G32B32A32_SFLOAT;
			return image.layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&(image.usage&VK_IMAGE_USAGE_SAMPLED_BIT)
				&&image.samples==VK_SAMPLE_COUNT_1_BIT&&image.viewType==VK_IMAGE_VIEW_TYPE_2D
				&&(hdr||(stage!=CustomPostProcessStage::HdrScene&&(image.format==VK_FORMAT_R8G8B8A8_UNORM||image.format==VK_FORMAT_B8G8R8A8_UNORM)));
		}
		CustomPostProcessImage Produced(CustomPostProcessImage image,std::uint64_t frame) noexcept
		{
			// This describes the PLANNED output allocation. Only a successful native
			// draw later emits a recorded producer token for this same allocation.
			image.frameId=frame;image.layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;return image;
		}
		bool ValidateFrame(const CustomPostProcessChainFrame& frame) noexcept
		{
			if(!frame.chain||frame.passes.size()!=frame.chain->passes.size())return false;
			if(frame.passes.empty())return true;
			if(!frame.frameId||!frame.parameterRevision)return false;
			for(std::size_t i=0;i<frame.passes.size();++i)
			{
				const auto& pass=frame.passes[i];const auto& native=frame.chain->passes[i];const auto stage=Stage(native.prepared->descriptor.stage);
				if(stage>=3||!native.native||pass.parameters.size()!=native.prepared->descriptor.parameterBytes
					||!Current(pass.source,frame.scope,frame.frameId)||!Current(pass.output,frame.scope,frame.frameId)
					||!SameAllocation(pass.source,native.source)||!SameAllocation(pass.output,Produced(native.output,frame.frameId))
					||pass.bindings.size()!=native.bindings.size())return false;
				for(std::size_t j=0;j<pass.bindings.size();++j)
				{
					const auto& binding=pass.bindings[j];const auto& expected=native.bindings[j];
					if(binding.binding!=expected.binding||binding.kind!=expected.kind||binding.assetId!=expected.assetId
						||!SameAllocation(binding.image,expected.image))return false;
					if(binding.kind!=CustomPostProcessInputKind::AssetTexture&&!Current(binding.image,frame.scope,frame.frameId))return false;
				}
			}
			return true;
		}
	}
	std::shared_ptr<const CustomPostProcessChain> PrepareCustomPostProcessChain(
		const CustomPostProcessChainCreateInfo& info,std::string& reason) noexcept
	{
		try
		{
			Require(bool(info.prepared)&&info.prepared->passes.size()<=256,"Custom chain needs a bounded validated pass set.");
			auto candidate=std::make_shared<CustomPostProcessChain>();candidate->prepared=info.prepared;candidate->stageSources=info.stageSources;
			std::array<CustomPostProcessImage,3> sources=info.stageSources;
			std::array<bool,3> used{};const CustomPostProcessImage* firstSource=nullptr;
			std::uint32_t priorStage=0;
			for(std::size_t ordinal=0;ordinal<info.prepared->passes.size();++ordinal)
			{
				const auto& prepared=info.prepared->passes[ordinal];const auto& d=prepared.descriptor;const auto stage=Stage(d.stage);
				Require(stage<3&&stage>=priorStage,"Custom chain insertion points are out of validated order.");priorStage=stage;
				for(std::size_t j=0;j<ordinal;++j)
				{
					const auto& old=info.prepared->passes[j].descriptor;
					Require(d.assetId!=old.assetId&&d.stableId!=old.stableId,"Custom chain repeats a pass identity.");
				}
				for(const auto& dependency:d.after)
					Require(std::any_of(info.prepared->passes.begin(),info.prepared->passes.begin()+ordinal,
						[&](const auto& p){return p.descriptor.stableId==dependency;}),"Custom chain dependency does not precede its consumer.");
				if(!prepared.enabled)continue;
				Require(info.device&&info.physicalDevice&&info.selectOutput,"Enabled custom chain requires native output selection.");
				if(!used[stage])
				{
					const auto& source=sources[stage];
					Require(Current(source,source.scope,source.frameId)&&LinearSource(source,d.stage),"Custom insertion point has no exact current linear source.");
					if(firstSource)Require(source.scope==firstSource->scope&&source.frameId==firstSource->frameId
						&&source.targetGeneration==firstSource->targetGeneration,"Custom insertion points belong to different view lifetimes/frames.");
					else firstSource=&info.stageSources[stage];used[stage]=true;
				}
				CustomPostProcessChain::Pass pass;pass.ordinal=static_cast<std::uint32_t>(ordinal);
				pass.prepared=std::shared_ptr<const PreparedPostProcessCustomPass>(info.prepared,&prepared);pass.source=sources[stage];
				if(!info.selectOutput(info.context,prepared,pass.source,pass.output,reason))
				{if(reason.empty())reason="Custom output selector rejected its actual allocation.";return {};}
				Require(Current(pass.output,pass.source.scope,pass.source.frameId)&&pass.output.targetGeneration==pass.source.targetGeneration
					&&pass.output.extent.width==pass.source.extent.width&&pass.output.extent.height==pass.source.extent.height,
					"Custom selected output does not match its exact source view/frame/extent.");
				pass.bindings.reserve(d.inputs.size());
				for(const auto& input:d.inputs)
				{
					CustomPostProcessBinding binding;
					if(input.kind==CustomPostProcessInputKind::SourceColor)binding={input.binding,input.kind,input.assetId,pass.source};
					else {Require(info.resolveInput,"Custom chain lacks a declared input resolver.");if(!info.resolveInput(info.context,prepared,input,binding,reason)){if(reason.empty())reason="Custom native input resolver rejected its actual binding.";return {};}}
					Require(binding.binding==input.binding&&binding.kind==input.kind&&binding.assetId==input.assetId,"Custom resolver returned a different declared binding.");
					pass.bindings.push_back(std::move(binding));
				}
				CustomPostProcessCreateInfo create;create.device=info.device;create.physicalDevice=info.physicalDevice;
				create.prepared=pass.prepared;create.fullscreenVertex=info.fullscreenVertex;create.stage=d.stage;create.domain=d.domain;
				create.bindings=pass.bindings;create.output=pass.output;create.samplingFilter=info.samplingFilter;
				pass.native=PrepareCustomPostProcessPass(create,reason);if(!pass.native)return {};
				sources[stage]=Produced(pass.output,pass.source.frameId);candidate->passes.push_back(std::move(pass));
			}
			reason.clear();return candidate;
		}
		catch(const std::exception& error){try{reason=error.what();}catch(...){}return {};}
		catch(...){try{reason="Custom chain candidate preparation failed.";}catch(...){}return {};}
	}
	std::shared_ptr<const CustomPostProcessChainFrame> PrepareCustomPostProcessChainFrame(
		std::shared_ptr<const CustomPostProcessChain> chain,const CustomPostProcessChainFrameRequest& request,std::string& reason) noexcept
	{
		try
		{
			Require(bool(chain)&&request.parameters.size()==chain->passes.size(),"Custom frame needs one exact snapshot per enabled pass.");
			auto frame=std::make_shared<CustomPostProcessChainFrame>();frame->chain=std::move(chain);frame->frameId=request.frameId;
			frame->parameterRevision=request.parameterRevision;frame->scope=request.scope;frame->stageSources=request.stageSources;
			if(frame->chain->passes.empty()){reason.clear();return frame;}
			Require(request.frameId&&request.parameterRevision,"Custom frame/parameter revision is missing.");
			std::array<CustomPostProcessImage,3> sources=request.stageSources;std::array<bool,3> used{};
			frame->passes.reserve(frame->chain->passes.size());
			for(const auto& native:frame->chain->passes)
			{
				const auto& prepared=*native.prepared;const auto& d=prepared.descriptor;const auto stage=Stage(d.stage);
				if(!used[stage])
				{
					Require(Current(sources[stage],request.scope,request.frameId)&&SameAllocation(sources[stage],frame->chain->stageSources[stage]),
						"Custom current insertion source was replaced, resized, stale or changed view scope.");used[stage]=true;
				}
				CustomPostProcessChainFrame::Pass pass;pass.source=sources[stage];pass.output=Produced(native.output,request.frameId);
				const CustomChainParameterSnapshot* parameters=nullptr;
				for(const auto& snapshot:request.parameters)if(snapshot.passAssetId==d.assetId)
				{Require(!parameters,"Custom parameter snapshot identity is duplicated.");parameters=&snapshot;}
				Require(parameters&&parameters->descriptorRevision==d.revision,"Custom parameter snapshot names a different native schema revision.");
				if(!PackCustomPostProcessParameters(d,parameters->values,pass.parameters,reason))return {};
				pass.bindings.reserve(d.inputs.size());
				for(const auto& input:d.inputs)
				{
					CustomPostProcessBinding binding;
					if(input.kind==CustomPostProcessInputKind::SourceColor)binding={input.binding,input.kind,input.assetId,pass.source};
					else {Require(request.resolveInput,"Custom current-frame input resolver is missing.");if(!request.resolveInput(request.context,prepared,input,binding,reason)){if(reason.empty())reason="Custom current input resolver rejected its actual binding.";return {};}}
					pass.bindings.push_back(std::move(binding));
				}
				sources[stage]=pass.output;frame->passes.push_back(std::move(pass));
			}
			Require(ValidateFrame(*frame),"Custom current inputs no longer match retained descriptors or exact producer frame.");
			reason.clear();return frame;
		}
		catch(const std::exception& error){try{reason=error.what();}catch(...){}return {};}
		catch(...){try{reason="Custom immutable frame preparation failed.";}catch(...){}return {};}
	}
	CustomChainRecordResult RecordCustomPostProcessChainStage(VkCommandBuffer command,
		const CustomPostProcessChainFrame& frame,CustomPostProcessStage stage,CustomChainStageRecordedUse& output,
		CustomChainRecordObserver observer,void* context) noexcept
	{
		if(Stage(stage)>=3||!ValidateFrame(frame))return CustomChainRecordResult::Rejected;
		const auto count=std::count_if(frame.chain->passes.begin(),frame.chain->passes.end(),[&](const auto& pass){return pass.prepared->descriptor.stage==stage;});
		if(!count)return CustomChainRecordResult::Skipped;
		if(!command)return CustomChainRecordResult::Rejected;
		CustomChainStageRecordedUse recorded;recorded.frameId=frame.frameId;recorded.parameterRevision=frame.parameterRevision;recorded.stage=stage;
		for(std::size_t i=0;i<frame.chain->passes.size();++i)
		{
			const auto& pass=frame.chain->passes[i];const auto& d=pass.prepared->descriptor;if(d.stage!=stage)continue;
			CustomChainPassRecordEvent event;event.command=command;event.stableId=d.stableId;event.assetId=d.assetId;event.stage=stage;
			event.ordinal=pass.ordinal;event.frameId=frame.frameId;event.parameterRevision=frame.parameterRevision;event.descriptorRevision=d.revision;
			event.shaderPublicationGeneration=pass.prepared->shader?pass.prepared->shader->publicationGeneration:0;
			event.scope=frame.scope;event.targetGeneration=frame.passes[i].output.targetGeneration;
			event.destinationImage=frame.passes[i].output.image;event.extent=frame.passes[i].output.extent;
			if(observer)observer(context,event);
			auto token=RecordCustomPostProcessPass(command,*pass.native,frame.passes[i].parameters);
			if(!token){event.status=CustomChainPassRecordStatus::Failed;if(observer)observer(context,event);return CustomChainRecordResult::Rejected;}
			// Native descriptors reference persistent allocations. The complete plan
			// proved these exact current-frame owners before ANY draw; publish the
			// actual successful draw with current provenance, never cached frame IDs.
			token->image=frame.passes[i].output;event.output=token;event.status=CustomChainPassRecordStatus::Recorded;
			if(observer)observer(context,event);recorded.output=std::move(token);++recorded.passCount;
		}
		output=std::move(recorded);return CustomChainRecordResult::Recorded;
	}
}
