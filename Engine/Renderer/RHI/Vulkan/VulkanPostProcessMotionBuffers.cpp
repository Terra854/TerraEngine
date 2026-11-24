// Per-recording immutable host uploads; no global animation descriptors borrowed.
#include "VulkanPostProcessMotionBuffers.h"
#include "Renderer/RenderTypes.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace TerraEngine::Renderer::Vulkan
{
	namespace
	{
		struct PoseRow
		{
			std::uint32_t paletteOffset=0,paletteCount=0,morphOffset=0,morphCount=0;
			std::uint32_t jointCount=0,validMarker=1,reserved0=0,reserved1=0;
		};
		using Palette=TerraEngine::Animation::GpuAnimationPaletteRecord;
		static_assert(sizeof(PoseRow)==32 && offsetof(PoseRow,jointCount)==16
			&& offsetof(PoseRow,validMarker)==20 && sizeof(Palette)==48
			&& offsetof(Palette,row1)==16 && offsetof(Palette,row2)==32);
		static_assert(sizeof(PostProcessMotionViewData)==192 && sizeof(PostProcessMotionDrawData)==160
			&& sizeof(PostProcessMotionCameraOnlyData)==64
			&& offsetof(PostProcessMotionViewData,state)==176
			&& offsetof(PostProcessMotionDrawData,deformationSelectors)==128
			&& offsetof(PostProcessMotionDrawData,deformationState)==144);
		bool Finite(const glm::mat4& matrix)
		{
			for(int column=0;column<4;++column) for(int row=0;row<4;++row)
				if(!std::isfinite(matrix[column][row])) return false;
			return true;
		}
		bool Finite(const glm::vec4& value)
		{
			return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z)&&std::isfinite(value.w);
		}
		std::uint64_t Token(std::uint32_t low,std::uint32_t high)
		{
			return std::uint64_t(low)|(std::uint64_t(high)<<32u);
		}
		void Require(bool condition,const char* message)
		{
			if(!condition) throw std::runtime_error(message);
		}
		// One source byte owner can be reused by many exact pose records. Only
		// byte storage is deduplicated: each draw still validates its own proof.
		std::uint32_t PackPose(const PostProcessMotionCpuPoseOwner& pose,
			const PostProcessHistoryState& history,std::vector<PoseRow>& poses,
			std::vector<Palette>& palettes,std::vector<float>& morphs,
			std::unordered_map<const CpuAnimationDeformationData*,std::uint32_t>& rows,
			VkDeviceSize maxStorage)
		{
			Require(pose.data && pose.data->IsComplete() && pose.frameId==history.frameId
				&& pose.rendererGeneration==history.scope.lifecycle.rendererSessionGeneration
				&& pose.poseGeneration && pose.payloadGeneration && pose.skeletonFingerprint
				&& pose.authorityGeneration && pose.stableInstanceId && pose.recordIndex!=0xffffffffu,
				"Motion CPU pose lacks exact retained recording provenance.");
			if(const auto found=rows.find(pose.data.get());found!=rows.end()) return found->second;
			const auto& data=*pose.data;
			Require(data.jointCount<=data.paletteMatrices.size(),"Motion CPU palette omits a declared joint.");
			for(const auto& matrix:data.paletteMatrices)
				for(const auto* row:{&matrix.row0,&matrix.row1,&matrix.row2})
					for(float value:*row) Require(std::isfinite(value),"Motion CPU palette contains non-finite values.");
			for(float value:data.morphWeights) Require(std::isfinite(value),"Motion CPU morph weight is non-finite.");
			const auto fits=[maxStorage](std::size_t count,std::size_t extra,std::size_t stride)
			{
				return count<=std::numeric_limits<std::uint32_t>::max()
					&& extra<=std::numeric_limits<std::uint32_t>::max()-count
					&& count+extra<=maxStorage/stride;
			};
			Require(fits(poses.size(),1,sizeof(PoseRow))&&fits(palettes.size(),data.paletteMatrices.size(),sizeof(Palette))
				&&fits(morphs.size(),data.morphWeights.size(),sizeof(float)),"Motion private pose storage exceeds native limits.");
			const auto index=static_cast<std::uint32_t>(poses.size());
			poses.push_back({static_cast<std::uint32_t>(palettes.size()),static_cast<std::uint32_t>(data.paletteMatrices.size()),
				static_cast<std::uint32_t>(morphs.size()),static_cast<std::uint32_t>(data.morphWeights.size()),data.jointCount,1,0,0});
			palettes.insert(palettes.end(),data.paletteMatrices.begin(),data.paletteMatrices.end());
			morphs.insert(morphs.end(),data.morphWeights.begin(),data.morphWeights.end());
			rows.emplace(pose.data.get(),index); return index;
		}
		void Upload(VmaAllocator allocator,PostProcessMotionUploadBuffer& output,
			const void* data,std::size_t bytes,VkDeviceSize maxStorage)
		{
			Require(data && bytes && bytes<=maxStorage,"Motion storage upload is empty or exceeds maxStorageBufferRange.");
			VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
			buffer.size=bytes; buffer.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; buffer.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
			VmaAllocationCreateInfo allocation{};
			allocation.usage=VMA_MEMORY_USAGE_AUTO;
			allocation.requiredFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
			allocation.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;
			VmaAllocationInfo info{};
			Require(vmaCreateBuffer(allocator,&buffer,&allocation,&output.buffer,&output.allocation,&info)==VK_SUCCESS,
				"Motion host-visible buffer allocation failed.");
			output.size=bytes;
			Require(info.pMappedData!=nullptr,"Motion host-visible allocation is not mapped.");
			std::memcpy(info.pMappedData,data,bytes);
			Require(vmaFlushAllocation(allocator,output.allocation,0,bytes)==VK_SUCCESS,"Motion upload flush failed.");
		}
	}
	PostProcessMotionBufferLayout::~PostProcessMotionBufferLayout()
	{
		if(layout && device) vkDestroyDescriptorSetLayout(device,layout,nullptr);
	}
	PostProcessMotionBuffers::~PostProcessMotionBuffers()
	{
		// Destroy descriptors before their referenced buffers. Caller guarantees
		// completion of accepted GPU borrowers, including unfenced submissions.
		if(descriptorPool && device) vkDestroyDescriptorPool(device,descriptorPool,nullptr);
		for(auto& buffer:buffers) if(buffer.buffer && allocator)
			vmaDestroyBuffer(allocator,buffer.buffer,buffer.allocation);
	}
	std::shared_ptr<PostProcessMotionBufferLayout> CreatePostProcessMotionBufferLayout(VkDevice device,std::string& reason) noexcept
	{
		try
		{
			Require(device!=VK_NULL_HANDLE,"Motion layout requires a live device.");
			auto owner=std::make_shared<PostProcessMotionBufferLayout>(); owner->device=device;
			std::array<VkDescriptorSetLayoutBinding,6> bindings{};
			for(std::uint32_t i=0;i<bindings.size();++i)
				bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr};
			bindings[0].stageFlags|=VK_SHADER_STAGE_FRAGMENT_BIT;
			VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			info.bindingCount=static_cast<std::uint32_t>(bindings.size()); info.pBindings=bindings.data();
			Require(vkCreateDescriptorSetLayout(device,&info,nullptr,&owner->layout)==VK_SUCCESS,"Motion descriptor layout allocation failed.");
			reason.clear(); return owner;
		}
		catch(const std::exception& error) { try { reason=error.what(); } catch(...) {} return {}; }
		catch(...) { try { reason="Motion descriptor layout preparation failed."; } catch(...) {} return {}; }
	}
	std::shared_ptr<PostProcessMotionBuffers> PreparePostProcessMotionBuffers(
		VkPhysicalDevice physicalDevice,VkDevice device,VmaAllocator allocator,
		std::shared_ptr<PostProcessMotionBufferLayout> layout,PreparedPostProcessMotion qualified,std::string& reason) noexcept
	{
		try
		{
			Require(physicalDevice && device && allocator && layout && layout->device==device && layout->layout,
				"Motion upload requires the matching device-owned set2 layout.");
			VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physicalDevice,&properties);
			const auto maximum=properties.limits.maxStorageBufferRange;
			Require(maximum>=sizeof(PostProcessMotionViewData)&&properties.limits.maxPerStageDescriptorStorageBuffers>=6
				&&properties.limits.maxDescriptorSetStorageBuffers>=6,"Motion storage descriptors exceed native limits.");
			const auto& view=qualified.shader;
			Require(qualified.current && qualified.current->publication && qualified.current->frameId && qualified.current->recordingId,
				"Motion upload lacks retained current history.");
			Require(Finite(view.currentRasterViewProjection)&&Finite(view.previousRasterViewProjection)&&Finite(view.jitterUV)
				&&view.extents.x&&view.extents.y&&view.extents.z&&view.extents.w
				&&!(view.state.x&~1u)&&!view.state.y&&!view.state.z&&!view.state.w
				&&Token(view.recordingTokens.x,view.recordingTokens.y)==qualified.current->recordingId,
				"Motion view ABI, finite values or recording token is invalid.");
			if(view.state.x&1u)
				Require(qualified.cpuHistoryCompatible&&qualified.previous&&qualified.previous->publication
					&&qualified.previous->scope==qualified.current->scope&&qualified.previous->signature==qualified.current->signature
					&&Token(view.recordingTokens.z,view.recordingTokens.w)==qualified.previous->recordingId,
					"Qualified motion view has no exact previous recording.");
			Require(qualified.draws.size()<=maximum/sizeof(PostProcessMotionDrawData),"Motion draw count exceeds storage limits.");
			std::vector<PostProcessMotionDrawData> draws;
			std::vector<PostProcessMotionCameraOnlyData> cameras;
			std::vector<PoseRow> poses;
			std::vector<Palette> palettes;
			std::vector<float> morphs;
			std::unordered_map<const CpuAnimationDeformationData*,std::uint32_t> rows;
			draws.reserve(qualified.draws.size()); cameras.reserve(qualified.draws.size());
			for(std::size_t index=0;index<qualified.draws.size();++index)
			{
				auto& draw=qualified.draws[index]; auto& shader=draw.shader;
				Require(draw.selectionIndex==index&&Finite(shader.currentModel)&&Finite(shader.previousModel)
					&&Finite(draw.cameraOnly.currentModelInPreviousReference)&&shader.deformationState.x<=1
					&&shader.deformationState.y<=1&&!(shader.deformationState.z&~7u)&&!shader.deformationState.w,
					"Motion draw order, matrices or qualification ABI is invalid.");
				Require(shader.deformationSelectors==glm::uvec4(0xffffffffu),"Motion input selectors must remain unassigned before private packing.");
				const auto flags=shader.deformationState.z;
				Require(!(flags&2u)||((view.state.x&1u)&&qualified.cpuHistoryCompatible&&qualified.previous&&draw.cpuPairValid
					&&draw.reason==PostProcessMotionReason::None),"Motion previous-owner qualification lacks an exact compatible pair.");
				Require(!(flags&4u)||((flags&1u)&&draw.cameraOnlyAvailable&&(view.state.x&1u)),
					"Motion camera-only qualification lacks current geometry or previous reference.");
				const auto pack=[&](bool previous)
				{
					const auto kind=previous?shader.deformationState.y:shader.deformationState.x;
					const auto bit=previous?2u:1u;
					const auto& pose=previous?draw.previousPose:draw.currentPose;
					Require(!(flags&bit)||kind==1u||!pose.data,"Qualified CPU deformation cannot silently use static bind pose.");
					if(kind==0) return;
					Require((flags&bit)&&pose.data,"Motion CPU kind requires qualified retained pose bytes.");
					const auto& history=previous?qualified.previous:qualified.current;
					Require(bool(history),"Motion CPU kind lacks its corresponding history owner.");
					const auto row=PackPose(pose,*history,poses,palettes,morphs,rows,maximum);
					if(previous) { shader.deformationSelectors.z=0; shader.deformationSelectors.w=row; }
					else { shader.deformationSelectors.x=0; shader.deformationSelectors.y=row; }
				};
				pack(false); pack(true); draws.push_back(shader); cameras.push_back(draw.cameraOnly);
			}
			// Valid descriptors are required even when a branch has no elements.
			// Dummy records remain invalid/unqualified; they never fabricate history.
			if(draws.empty()) draws.emplace_back(); if(cameras.empty()) cameras.emplace_back();
			if(poses.empty()) { poses.emplace_back(); poses.back().validMarker=0; }
			if(palettes.empty()) palettes.emplace_back(); if(morphs.empty()) morphs.push_back(0.0f);
			auto owner=std::make_shared<PostProcessMotionBuffers>();
			owner->device=device; owner->allocator=allocator; owner->layout=std::move(layout);
			owner->prepared=std::move(qualified);
			Upload(allocator,owner->buffers[0],&owner->prepared.shader,sizeof(PostProcessMotionViewData),maximum);
			Upload(allocator,owner->buffers[1],draws.data(),draws.size()*sizeof(draws[0]),maximum);
			Upload(allocator,owner->buffers[2],cameras.data(),cameras.size()*sizeof(cameras[0]),maximum);
			Upload(allocator,owner->buffers[3],poses.data(),poses.size()*sizeof(poses[0]),maximum);
			Upload(allocator,owner->buffers[4],palettes.data(),palettes.size()*sizeof(palettes[0]),maximum);
			Upload(allocator,owner->buffers[5],morphs.data(),morphs.size()*sizeof(morphs[0]),maximum);
			VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,6};
			VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			pool.maxSets=1; pool.poolSizeCount=1; pool.pPoolSizes=&poolSize;
			Require(vkCreateDescriptorPool(device,&pool,nullptr,&owner->descriptorPool)==VK_SUCCESS,"Motion descriptor pool allocation failed.");
			VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
			allocation.descriptorPool=owner->descriptorPool; allocation.descriptorSetCount=1; allocation.pSetLayouts=&owner->layout->layout;
			Require(vkAllocateDescriptorSets(device,&allocation,&owner->descriptorSet)==VK_SUCCESS,"Motion descriptor allocation failed.");
			std::array<VkDescriptorBufferInfo,6> infos{};
			std::array<VkWriteDescriptorSet,6> writes{};
			for(std::uint32_t i=0;i<writes.size();++i)
			{
				infos[i]={owner->buffers[i].buffer,0,owner->buffers[i].size};
				writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[i].dstSet=owner->descriptorSet;
				writes[i].dstBinding=i; writes[i].descriptorCount=1; writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				writes[i].pBufferInfo=&infos[i];
			}
			vkUpdateDescriptorSets(device,static_cast<std::uint32_t>(writes.size()),writes.data(),0,nullptr);
			reason.clear(); return owner;
		}
		catch(const std::exception& error) { try { reason=error.what(); } catch(...) {} return {}; }
		catch(...) { try { reason="Motion buffer preparation failed."; } catch(...) {} return {}; }
	}
}
