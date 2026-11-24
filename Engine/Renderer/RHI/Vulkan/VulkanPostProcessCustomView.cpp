#include "VulkanPostProcessCustomView.h"
#include <algorithm>
#include <stdexcept>

namespace TerraEngine::Renderer::Vulkan
{
	struct CustomPostProcessView
	{
		std::shared_ptr<const CustomPostProcessChain> chain;
		std::array<CustomViewStagePlan,3> stages;
	};
	struct CustomPostProcessViewFrame
	{
		std::shared_ptr<const CustomPostProcessView> view;
		std::shared_ptr<const CustomPostProcessChainFrame> chainFrame;
	};
	namespace
	{
		void Require(bool value,const char* text) {if(!value)throw std::runtime_error(text);}
		void Fail(std::string& reason,const char* text) noexcept {try{reason=text;}catch(...) {}}
		bool Enabled(const PreparedPostProcessCustomPassSet& prepared) noexcept
		{return std::any_of(prepared.passes.begin(),prepared.passes.end(),[](const auto& p){return p.enabled;});}
		struct Selection
		{
			std::array<CustomViewStagePlan,3>* stages=nullptr;
			std::array<std::vector<CustomPostProcessImage>,3> images;
			std::array<std::size_t,3> next{};
		};
		bool SelectOutput(void* context,const PreparedPostProcessCustomPass& pass,
			const CustomPostProcessImage&,CustomPostProcessImage& output,std::string& reason)
		{
			auto& selection=*static_cast<Selection*>(context);const auto stage=static_cast<std::uint32_t>(pass.descriptor.stage);
			if(stage>=3||selection.next[stage]>=(*selection.stages)[stage].destinations.size())
			{reason="Custom view destination plan exhausted.";return false;}
			output=selection.images[stage][(*selection.stages)[stage].destinations[selection.next[stage]++]];return true;
		}
	}
	bool SelectCustomPostProcessViewStage(const PreparedPostProcessCustomPassSet& prepared,
		CustomPostProcessStage stage,const CustomPostProcessImage& source,
		std::span<const CustomPostProcessImage> images,std::array<std::uint32_t,2> pingIndices,
		CustomViewStagePlan& output,std::string& reason) noexcept
	{
		try
		{
			Require(static_cast<std::uint32_t>(stage)<3,"Custom view insertion point is invalid.");
			CustomViewStagePlan candidate;candidate.stage=stage;
			VkImage current=source.image;VkImageView currentView=source.view;
			for(const auto& pass:prepared.passes)
			{
				if(!pass.enabled||pass.descriptor.stage!=stage)continue;
				for(const auto& input:pass.descriptor.inputs)
					Require(input.kind==CustomPostProcessInputKind::SourceColor,"Custom view currently supports SourceColor inputs only.");
				Require(images.size()<=3&&pingIndices[0]<images.size()&&pingIndices[1]<images.size()
					&&pingIndices[0]!=pingIndices[1],"Custom view requires two distinct existing ping indices.");
				const auto first=pingIndices[0],second=pingIndices[1];
				const auto index=images[first].image!=current&&images[first].view!=currentView?first:second;
				const auto& destination=images[index];
				Require(current&&currentView&&destination.image&&destination.view&&destination.image!=current
					&&destination.view!=currentView,"Custom view output aliases its source.");
				candidate.destinations.push_back(index);candidate.finalImageIndex=index;
				current=destination.image;currentView=destination.view;
			}
			output=std::move(candidate);reason.clear();return true;
		}
		catch(const std::exception& e){Fail(reason,e.what());return false;}
		catch(...){Fail(reason,"Custom view stage planning failed.");return false;}
	}
	std::shared_ptr<const CustomPostProcessView> PrepareCustomPostProcessView(const CustomViewCreateInfo& info,std::string& reason) noexcept
	{
		try
		{
			Require(bool(info.prepared),"Custom view requires its validated immutable pass set.");
			auto view=std::make_shared<CustomPostProcessView>();
			// Disabled authoring never binds images, loads modules or creates native
			// objects. Retaining this empty owner is not a producer/capability claim.
			if(!Enabled(*info.prepared)){reason.clear();return view;}
			Require(bool(info.images),"Enabled custom view requires the actual adopted image owner.");
			Selection selection;selection.stages=&view->stages;
			for(std::uint32_t stage=0;stage<3;++stage)
			{
				view->stages[stage].stage=static_cast<CustomPostProcessStage>(stage);
				const bool used=std::any_of(info.prepared->passes.begin(),info.prepared->passes.end(),[&](const auto& p)
					{return p.enabled&&static_cast<std::uint32_t>(p.descriptor.stage)==stage;});
				if(!used)continue;
				const auto& source=info.stageSources[stage];
				// Bind only referenced pool indices. The adopter verifies real owner,
				// allocation metadata and index, without any GPU readiness inference.
				selection.images[stage].resize(3);
				for(const auto index:info.pingIndices[stage])
				{
					Require(index<3,"Custom view image pool index exceeds adopted pool capacity.");
					Require(info.imageLayouts[index]==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						"Custom ping image must enter the stage in caller-established sampled-read layout.");
					auto image=BindCustomPostProcessImageSetImage(info.images,index,source.scope,source.frameId,
						source.targetGeneration,info.imageLayouts[index],reason);if(!image)return {};
					selection.images[stage][index]=std::move(*image);
				}
				if(!SelectCustomPostProcessViewStage(*info.prepared,static_cast<CustomPostProcessStage>(stage),source,
					selection.images[stage],info.pingIndices[stage],view->stages[stage],reason))return {};
			}
			CustomPostProcessChainCreateInfo chain;chain.physicalDevice=info.physicalDevice;chain.device=info.device;
			chain.prepared=info.prepared;chain.fullscreenVertex=info.fullscreenVertex;chain.stageSources=info.stageSources;
			chain.selectOutput=SelectOutput;chain.context=&selection;chain.samplingFilter=info.samplingFilter;
			view->chain=PrepareCustomPostProcessChain(chain,reason);if(!view->chain)return {};
			reason.clear();return view;
		}
		catch(const std::exception& e){Fail(reason,e.what());return {};}
		catch(...){Fail(reason,"Custom view native preparation failed.");return {};}
	}
	std::optional<CustomViewStagePlan> ReadCustomPostProcessViewStage(const CustomPostProcessView& view,CustomPostProcessStage stage)
	{
		const auto index=static_cast<std::uint32_t>(stage);if(index>=3)return {};return view.stages[index];
	}
	std::shared_ptr<const CustomPostProcessViewFrame> PrepareCustomPostProcessViewFrame(
		std::shared_ptr<const CustomPostProcessView> view,const CustomViewFrameRequest& request,std::string& reason) noexcept
	{
		try
		{
			Require(bool(view),"Custom frame requires its retained native view.");
			auto frame=std::make_shared<CustomPostProcessViewFrame>();frame->view=std::move(view);
			if(!frame->view->chain){reason.clear();return frame;}
			CustomPostProcessChainFrameRequest chain;chain.frameId=request.frameId;chain.parameterRevision=request.authoringRevision;
			chain.scope=request.scope;chain.stageSources=request.stageSources;chain.parameters=request.parameters;
			frame->chainFrame=PrepareCustomPostProcessChainFrame(frame->view->chain,chain,reason);if(!frame->chainFrame)return {};
			reason.clear();return frame;
		}
		catch(const std::exception& e){Fail(reason,e.what());return {};}
		catch(...){Fail(reason,"Custom view immutable frame preparation failed.");return {};}
	}
	CustomChainRecordResult RecordCustomPostProcessViewStage(VkCommandBuffer command,const CustomPostProcessViewFrame& frame,
		CustomPostProcessStage stage,CustomChainStageRecordedUse& output,CustomChainRecordObserver observer,void* context) noexcept
	{
		if(!frame.view||static_cast<std::uint32_t>(stage)>=3)return CustomChainRecordResult::Rejected;
		if(!frame.chainFrame)return CustomChainRecordResult::Skipped;
		return RecordCustomPostProcessChainStage(command,*frame.chainFrame,stage,output,observer,context);
	}
}
