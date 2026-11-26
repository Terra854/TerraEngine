// Native custom-pass resources are private candidates until their caller publishes.
#include "VulkanPostProcessCustom.h"
#include "Resource/PostProcessEffectAsset.h"
#include "File Formats/TerraEngineShaderFile.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace TerraEngine::Renderer::Vulkan
{
	struct CustomPostProcessPass
	{
		VkDevice device=VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptors=VK_NULL_HANDLE;
		VkDescriptorPool pool=VK_NULL_HANDLE;
		VkDescriptorSet set=VK_NULL_HANDLE;
		VkSampler sampler=VK_NULL_HANDLE;
		VkRenderPass renderPass=VK_NULL_HANDLE;
		VkFramebuffer framebuffer=VK_NULL_HANDLE;
		VkPipelineLayout layout=VK_NULL_HANDLE;
		VkPipeline pipeline=VK_NULL_HANDLE;
		std::shared_ptr<const PreparedPostProcessCustomPass> prepared;
		std::vector<CustomPostProcessBinding> bindings;
		CustomPostProcessImage output;
		~CustomPostProcessPass()
		{
			// Caller provides real GPU retirement. Borrowers die before retained
			// image owners, and a partially prepared candidate is safe to destroy.
			if(!device) return;
			if(pipeline) vkDestroyPipeline(device,pipeline,nullptr);
			if(framebuffer) vkDestroyFramebuffer(device,framebuffer,nullptr);
			if(renderPass) vkDestroyRenderPass(device,renderPass,nullptr);
			if(layout) vkDestroyPipelineLayout(device,layout,nullptr);
			if(pool) vkDestroyDescriptorPool(device,pool,nullptr);
			if(descriptors) vkDestroyDescriptorSetLayout(device,descriptors,nullptr);
			if(sampler) vkDestroySampler(device,sampler,nullptr);
		}
	};
	namespace
	{
		void Require(bool valid,const char* detail) { if(!valid) throw std::runtime_error(detail); }
		std::uint32_t Size(CustomPostProcessParameterKind kind) noexcept
		{
			constexpr std::uint32_t sizes[]{4,4,4,4,8,12,16};
			const auto index=static_cast<std::uint32_t>(kind); return index<std::size(sizes)?sizes[index]:0;
		}
		bool Finite(const CustomPostProcessValue& value)
		{
			return std::visit([](const auto& v) {
				if constexpr(std::is_same_v<std::decay_t<decltype(v)>,float>) return std::isfinite(v);
				else if constexpr(requires { v.begin(); }) return std::all_of(v.begin(),v.end(),[](float x){return std::isfinite(x);});
				else return true;
			},value);
		}
		bool LessEqual(const CustomPostProcessValue& left,const CustomPostProcessValue& right)
		{
			if(left.index()!=right.index()) return false;
			return std::visit([&](const auto& a) {
				const auto& b=std::get<std::decay_t<decltype(a)>>(right);
				if constexpr(requires { a.begin(); }) { for(std::size_t i=0;i<a.size();++i) if(a[i]>b[i]) return false; return true; }
				else return a<=b;
			},left);
		}
		void ValidateParameters(const PostProcessCustomPassDescriptor& schema)
		{
			Require(schema.parameterBytes<=128&&schema.parameterBytes%4==0&&schema.parameters.size()<=32
				&&(schema.parameters.empty()==(schema.parameterBytes==0)),"Custom parameter block has invalid bounded extent.");
			for(std::size_t i=0;i<schema.parameters.size();++i)
			{
				const auto& p=schema.parameters[i]; const auto bytes=Size(p.layout.kind);
				Require(!p.layout.name.empty()&&bytes&&p.layout.offsetBytes<=schema.parameterBytes
					&&bytes<=schema.parameterBytes-p.layout.offsetBytes&&p.layout.offsetBytes%(bytes>=12?16:bytes)==0,
					"Custom parameter has invalid name, physical extent or alignment.");
				Require(p.defaultValue.index()==static_cast<std::size_t>(p.layout.kind)&&Finite(p.defaultValue)
					&&(!p.minimum||(p.minimum->index()==p.defaultValue.index()&&Finite(*p.minimum)))
					&&(!p.maximum||(p.maximum->index()==p.defaultValue.index()&&Finite(*p.maximum)))
					&&(!p.minimum||!p.maximum||LessEqual(*p.minimum,*p.maximum))
					&&(!p.minimum||LessEqual(*p.minimum,p.defaultValue))&&(!p.maximum||LessEqual(p.defaultValue,*p.maximum))
					&&(p.layout.kind!=CustomPostProcessParameterKind::Boolean||(!p.minimum&&!p.maximum)),
					"Custom parameter types, defaults or bounds are invalid.");
				for(std::size_t j=0;j<i;++j)
				{
					const auto& q=schema.parameters[j];
					Require(p.layout.name!=q.layout.name&&!(p.layout.offsetBytes<q.layout.offsetBytes+Size(q.layout.kind)
						&&q.layout.offsetBytes<p.layout.offsetBytes+bytes),"Custom parameter names or physical byte ranges overlap.");
				}
			}
		}
		template<class T> T Read(const std::byte* bytes) noexcept { T v; std::memcpy(&v,bytes,sizeof(T)); return v; }
		bool ValidPacked(const PostProcessCustomPassDescriptor& schema,std::span<const std::byte> bytes) noexcept
		{
			if(bytes.size()!=schema.parameterBytes) return false;
			for(const auto& p:schema.parameters)
			{
				const auto* data=bytes.data()+p.layout.offsetBytes;
				if(p.layout.kind==CustomPostProcessParameterKind::Boolean) { if(Read<std::uint32_t>(data)>1) return false; continue; }
				if(p.layout.kind==CustomPostProcessParameterKind::Signed)
				{
					const auto v=Read<std::int32_t>(data);
					if((p.minimum&&v<std::get<std::int32_t>(*p.minimum))||(p.maximum&&v>std::get<std::int32_t>(*p.maximum))) return false;
				}
				else if(p.layout.kind==CustomPostProcessParameterKind::Unsigned)
				{
					const auto v=Read<std::uint32_t>(data);
					if((p.minimum&&v<std::get<std::uint32_t>(*p.minimum))||(p.maximum&&v>std::get<std::uint32_t>(*p.maximum))) return false;
				}
				else
				{
					const auto bound=[](const CustomPostProcessValue& value,std::size_t lane) noexcept {
						return std::visit([lane](const auto& v)->float {
							if constexpr(requires { v.begin(); }) return v[lane]; else return static_cast<float>(v);
						},value);
					};
					for(std::size_t lane=0;lane<Size(p.layout.kind)/4;++lane)
					{
						const auto v=Read<float>(data+lane*4);
						if(!std::isfinite(v)||(p.minimum&&v<bound(*p.minimum,lane))||(p.maximum&&v>bound(*p.maximum,lane))) return false;
					}
				}
			}
			return true;
		}
		bool FloatColor(VkFormat format) noexcept
		{
			return format==VK_FORMAT_R16G16B16A16_SFLOAT||format==VK_FORMAT_R32G32B32A32_SFLOAT;
		}
		bool LinearColor(VkFormat format) noexcept
		{
			return FloatColor(format)||format==VK_FORMAT_R8G8B8A8_UNORM||format==VK_FORMAT_B8G8R8A8_UNORM;
		}
		bool LinearAsset(VkFormat format) noexcept
		{
			if(LinearColor(format)) return true;
			switch(format)
			{
			case VK_FORMAT_R8_UNORM:case VK_FORMAT_R8G8_UNORM:case VK_FORMAT_R16_UNORM:case VK_FORMAT_R16G16_UNORM:
			case VK_FORMAT_R16_SFLOAT:case VK_FORMAT_R16G16_SFLOAT:case VK_FORMAT_R32_SFLOAT:case VK_FORMAT_R32G32_SFLOAT:
			case VK_FORMAT_BC1_RGB_UNORM_BLOCK:case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:case VK_FORMAT_BC2_UNORM_BLOCK:
			case VK_FORMAT_BC3_UNORM_BLOCK:case VK_FORMAT_BC4_UNORM_BLOCK:case VK_FORMAT_BC5_UNORM_BLOCK:
			case VK_FORMAT_BC6H_UFLOAT_BLOCK:case VK_FORMAT_BC6H_SFLOAT_BLOCK:case VK_FORMAT_BC7_UNORM_BLOCK:return true;
			default:return false;
			}
		}
		bool Depth(VkFormat format) noexcept { return format==VK_FORMAT_D32_SFLOAT||format==VK_FORMAT_D16_UNORM; }
		std::string_view ShaderString(const Shader& shader,std::uint32_t offset)
		{
			if(offset>=shader.stringTable.size()) return {};const auto value=shader.stringTable.substr(offset);
			const auto end=value.find('\0');return end==std::string_view::npos?std::string_view{}:value.substr(0,end);
		}
		// Bounded entry check on the supplied actual module, not a filename or
		// metadata claim. Full validation remains the shader compiler/device lane.
		bool Entry(std::span<const std::uint32_t> words,std::uint32_t model,std::string_view name)
		{
			if(words.size()<5||words[0]!=0x07230203u) return false;
			bool found=false;
			for(std::size_t i=5;i<words.size();)
			{
				const auto count=words[i]>>16,op=words[i]&0xffffu;if(!count||count>words.size()-i) return false;
				if(op==15)
				{
					if(count<4) return false;
					const auto* text=reinterpret_cast<const char*>(&words[i+3]);const auto limit=(count-3)*4;
					const auto* end=static_cast<const char*>(std::memchr(text,0,limit));if(!end) return false;
					if(words[i+1]==model&&std::string_view(text,static_cast<std::size_t>(end-text))==name) { if(found) return false; found=true; }
				}
				// The bundled vertex-free stage must not sneak in descriptor or
				// push-constant obligations absent from the custom fragment schema.
				if(model==0&&((op==71&&count>=3&&(words[i+2]==33||words[i+2]==34))
					||(op==59&&count>=4&&words[i+3]==9))) return false;
				i+=count;
			}
			return found;
		}
		struct Modules
		{
			VkDevice device;VkShaderModule vertex=VK_NULL_HANDLE,fragment=VK_NULL_HANDLE;
			~Modules(){if(vertex)vkDestroyShaderModule(device,vertex,nullptr);if(fragment)vkDestroyShaderModule(device,fragment,nullptr);}
		};
	}
	bool PackCustomPostProcessParameters(const PostProcessCustomPassDescriptor& schema,
		std::span<const CustomPostProcessParameterValue> values,std::vector<std::byte>& output,std::string& reason) noexcept
	{
		try
		{
			ValidateParameters(schema);Require(values.size()==schema.parameters.size(),"Custom parameter values must be complete.");
			std::vector<std::byte> candidate(schema.parameterBytes,std::byte{0});
			for(const auto& p:schema.parameters)
			{
				const CustomPostProcessParameterValue* selected=nullptr;
				for(const auto& value:values) if(value.name==p.layout.name) { Require(!selected,"Custom parameter name is duplicated.");selected=&value; }
				Require(selected&&selected->value.index()==static_cast<std::size_t>(p.layout.kind)&&Finite(selected->value)
					&&(!p.minimum||LessEqual(*p.minimum,selected->value))&&(!p.maximum||LessEqual(selected->value,*p.maximum)),
					"Custom parameter value is missing, wrong-type, non-finite or outside its declared range.");
				std::visit([&](const auto& value) {
					using T=std::decay_t<decltype(value)>;
					if constexpr(std::is_same_v<T,bool>) { const std::uint32_t physical=value?1:0;std::memcpy(candidate.data()+p.layout.offsetBytes,&physical,4); }
					else std::memcpy(candidate.data()+p.layout.offsetBytes,&value,sizeof(value));
				},selected->value);
			}
			output=std::move(candidate);reason.clear();return true;
		}
		catch(const std::exception& error){try{reason=error.what();}catch(...){}return false;}
		catch(...){try{reason="Custom parameter packing failed.";}catch(...){}return false;}
	}

	std::shared_ptr<CustomPostProcessPass> PrepareCustomPostProcessPass(const CustomPostProcessCreateInfo& info,std::string& reason) noexcept
	{
		// Disabled registrations preserve the existing route without device calls.
		if(info.prepared&&!info.prepared->enabled) { reason.clear();return {}; }
		try
		{
			Require(info.physicalDevice&&info.device&&info.prepared&&info.prepared->shader,"Custom pass needs an immutable prepared shader and live device.");
			const auto& d=info.prepared->descriptor;const auto& shader=*info.prepared->shader;
			Require(d.schemaVersion==1&&d.revision&&d.assetId!=TerraEngine::EMPTY_UUID&&d.shaderAssetId!=TerraEngine::EMPTY_UUID
				&&d.stage==info.stage&&d.domain==info.domain&&static_cast<std::uint32_t>(d.stage)<3
				&&(d.stage==CustomPostProcessStage::HdrScene?d.domain==CustomPostProcessDomain::LinearHdr:d.domain==CustomPostProcessDomain::DisplayLinear)
				&&d.outputAccess==CustomPostProcessOutputAccess::DistinctColorWrite&&d.inputs.size()<=32,
				"Custom pass stage, source domain or distinct-output contract does not match this insertion point.");
			ValidateParameters(d);
			std::vector<std::byte> defaultBytes;
			Require(PackCustomPostProcessParameters(d,info.prepared->values,defaultBytes,reason),"Custom prepared parameter values are invalid.");
			Require(shader.publicationGeneration&&shader.binary.format==static_cast<std::uint32_t>(ShaderBinaryFormat::SPIRV)
				&&shader.reflection.source.stage==static_cast<std::uint32_t>(ShaderStage::Fragment)
				&&shader.bytecode.size()==shader.binary.bytecodeSize&&shader.bytecode.size()%4==0
				&&ShaderString(shader,shader.binary.entryPointOffset)==d.entryPoint
				&&ShaderString(shader,shader.reflection.source.entryPointOffset)==d.entryPoint,
				"Custom pass requires the exact published SPIR-V fragment entry.");
			std::vector<std::uint32_t> fragment(shader.bytecode.size()/4);
			if(!fragment.empty()) std::memcpy(fragment.data(),shader.bytecode.data(),shader.bytecode.size());
			Require(Entry(fragment,4,d.entryPoint)&&Entry(info.fullscreenVertex,0,"main"),"Custom fragment or bundled fullscreen vertex module has an invalid entry.");
			Resource::PostProcessEffectParameterReflection reflection;
			Require(Resource::ReflectPostProcessEffectParameters(shader,reflection,reason)&&reflection.parameterBytes==d.parameterBytes
				&&reflection.parameters.size()==d.parameters.size(),"Custom parameter layout no longer matches its compiled SPIR-V.");
			for(const auto& p:d.parameters)
			{
				Require(std::count(reflection.parameters.begin(),reflection.parameters.end(),p.layout)==1,"Custom typed parameter differs from its exact compiled member.");
				// Vulkan physical push storage cannot be certified by inventing a uint
				// lowering for a logical Boolean reflected from the source module.
				Require(p.layout.kind!=CustomPostProcessParameterKind::Boolean,"Logical Boolean custom push storage has no qualified Vulkan physical lowering.");
			}
			Require(shader.outputs.size()==1&&shader.outputs[0].location==0&&shader.outputs[0].componentCount==4
				&&shader.outputs[0].componentType==static_cast<std::uint32_t>(IoComponentType::Float)
				&&shader.inputs.size()<=1&&(shader.inputs.empty()||(shader.inputs[0].location==0&&shader.inputs[0].componentCount==2
				&&shader.inputs[0].componentType==static_cast<std::uint32_t>(IoComponentType::Float))),"Custom fragment IO disagrees with the fullscreen contract.");
			const auto count=d.inputs.size()+(d.samplerBinding?1:0);
			Require(shader.descriptorBindings.size()==count&&info.bindings.size()==d.inputs.size()
				&&(count?(shader.descriptorSets.size()==1&&shader.descriptorSets[0].setIndex==0
				&&shader.descriptorSets[0].firstBinding==0&&shader.descriptorSets[0].bindingCount==count):shader.descriptorSets.empty()),
				"Custom sampled descriptor set does not exactly match the prepared input list.");
			const auto& output=info.output;
			Require(output.image&&output.view&&output.owner&&output.extent.width&&output.extent.height&&output.frameId&&output.targetGeneration
				&&output.scope.lifecycle.rendererSessionGeneration&&output.scope.lifecycle.backendResourceGeneration
				&&output.scope.target.view&&output.scope.target.view->lifetimeGeneration
				&&output.samples==VK_SAMPLE_COUNT_1_BIT&&output.viewType==VK_IMAGE_VIEW_TYPE_2D
				&&(output.usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)&&(output.usage&VK_IMAGE_USAGE_SAMPLED_BIT)
				&&(d.domain==CustomPostProcessDomain::LinearHdr?FloatColor(output.format):LinearColor(output.format)),
				"Custom output needs a retained same-view linear single-sample color allocation.");
			VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(info.physicalDevice,&properties);
			Require(output.extent.width<=properties.limits.maxFramebufferWidth&&output.extent.height<=properties.limits.maxFramebufferHeight
				&&d.parameterBytes<=properties.limits.maxPushConstantsSize,"Custom output or parameter block exceeds native limits.");
			VkFormatProperties outputFormat{};vkGetPhysicalDeviceFormatProperties(info.physicalDevice,output.format,&outputFormat);
			Require((outputFormat.optimalTilingFeatures&(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
				==(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT),"Custom output format lacks color/sampled support.");
			Require(info.samplingFilter==VK_FILTER_LINEAR||info.samplingFilter==VK_FILTER_NEAREST,"Custom sampler filter is unsupported.");
			auto pass=std::make_shared<CustomPostProcessPass>();pass->device=info.device;pass->prepared=info.prepared;
			pass->output=output;pass->bindings.assign(info.bindings.begin(),info.bindings.end());
			std::vector<VkDescriptorSetLayoutBinding> layouts;layouts.reserve(count);
			std::vector<VkDescriptorImageInfo> images;images.reserve(count);
			std::vector<std::uint32_t> imageBindings;imageBindings.reserve(count);
			std::vector<VkDescriptorType> types;types.reserve(count);
			std::uint32_t samplerCount=0,sampledCount=0;
			for(const auto& input:d.inputs)
			{
				Require(input.access==CustomPostProcessAccess::SampledRead&&static_cast<std::uint32_t>(input.kind)<=5
					&&(!d.samplerBinding||input.binding!=*d.samplerBinding),"Custom input access or binding is invalid.");
				Require(std::count_if(d.inputs.begin(),d.inputs.end(),[&](const auto& other){return other.binding==input.binding;})==1,
					"Custom input binding is duplicated.");
				const auto native=std::find_if(pass->bindings.begin(),pass->bindings.end(),[&](const auto& b){return b.binding==input.binding;});
				Require(native!=pass->bindings.end()&&std::count_if(pass->bindings.begin(),pass->bindings.end(),[&](const auto& b){return b.binding==input.binding;})==1
					&&native->kind==input.kind&&native->assetId==input.assetId,"Custom declared binding has no exact supplied semantic/asset view.");
				const auto& image=native->image;const bool asset=input.kind==CustomPostProcessInputKind::AssetTexture;
				Require(image.image&&image.view&&image.owner&&image.extent.width&&image.extent.height
					&&image.image!=output.image&&image.view!=output.view&&(image.usage&VK_IMAGE_USAGE_SAMPLED_BIT)
					&&image.samples==VK_SAMPLE_COUNT_1_BIT&&image.viewType==VK_IMAGE_VIEW_TYPE_2D,
					"Custom sampled view is missing, aliases output or has unsupported samples/type.");
				Require(asset?(input.assetId!=TerraEngine::EMPTY_UUID&&LinearAsset(image.format))
					:(input.assetId==TerraEngine::EMPTY_UUID&&image.scope==output.scope&&image.frameId==output.frameId
					&&image.targetGeneration==output.targetGeneration&&image.extent.width==output.extent.width&&image.extent.height==output.extent.height),
					"Custom intrinsic input does not belong to this exact current view/frame/extent, or asset is not linear color.");
				if(input.kind==CustomPostProcessInputKind::Depth)
					Require(Depth(image.format)&&image.depthEncoding==CustomPostProcessDepthEncoding::VulkanStandardZ
						&&image.layout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,"Custom depth needs typed standard sampled depth layout.");
				else
				{
					Require(image.layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,"Custom color/motion input is not explicitly shader-readable.");
					if(input.kind==CustomPostProcessInputKind::SourceColor)
						Require(d.domain==CustomPostProcessDomain::LinearHdr?FloatColor(image.format):LinearColor(image.format),"Custom source color has the wrong linear stage domain.");
					else if(input.kind==CustomPostProcessInputKind::Normals)
						Require(FloatColor(image.format)&&image.signedViewNormals,"Custom normal input lacks its signed view-space float4 producer.");
					else if(input.kind==CustomPostProcessInputKind::Motion)
						Require(FloatColor(image.format)&&image.motionEncoding==CustomPostProcessMotionEncoding::PreviousUvDeltaDepthValidity,
							"Custom motion input lacks its previous-UV delta/depth/validity float4 producer.");
					else if(input.kind==CustomPostProcessInputKind::Roughness)
						Require(image.perceptualRoughness&&(FloatColor(image.format)||image.format==VK_FORMAT_R16_SFLOAT||image.format==VK_FORMAT_R32_SFLOAT),"Custom roughness requires a perceptual float producer.");
				}
				VkFormatProperties format{};vkGetPhysicalDeviceFormatProperties(info.physicalDevice,image.format,&format);
				Require(format.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,"Custom input format lacks sampled support.");
				if((input.combinedSampler||d.samplerBinding)&&info.samplingFilter==VK_FILTER_LINEAR)
					Require(format.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,"Custom sampled input cannot use the declared linear filter.");
				const auto type=input.combinedSampler?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
				layouts.push_back({input.binding,type,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr});
				images.push_back({VK_NULL_HANDLE,image.view,image.layout});imageBindings.push_back(input.binding);types.push_back(type);
				++sampledCount;if(input.combinedSampler)++samplerCount;
			}
			if(d.samplerBinding) { layouts.push_back({*d.samplerBinding,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr});++samplerCount; }
			Require(sampledCount<=properties.limits.maxPerStageDescriptorSampledImages&&samplerCount<=properties.limits.maxPerStageDescriptorSamplers
				&&sampledCount<=properties.limits.maxDescriptorSetSampledImages&&samplerCount<=properties.limits.maxDescriptorSetSamplers,
				"Custom input descriptors exceed native stage/set limits.");
			for(const auto& binding:shader.descriptorBindings)
			{
				const auto expected=std::find_if(layouts.begin(),layouts.end(),[&](const auto& b){return b.binding==binding.binding;});
				Require(expected!=layouts.end()&&binding.arrayCount==1&&binding.visibility==StageToMask(ShaderStage::Fragment)
					&&std::count_if(shader.descriptorBindings.begin(),shader.descriptorBindings.end(),[&](const auto& b){return b.binding==binding.binding;})==1,
					"Custom reflected binding is absent, duplicated or has wrong visibility/count.");
				const auto reflectionType=expected->descriptorType==VK_DESCRIPTOR_TYPE_SAMPLER?DescriptorType::Sampler
					:expected->descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER?DescriptorType::CombinedImageSampler:DescriptorType::SampledImage;
				Require(binding.type==static_cast<std::uint32_t>(reflectionType),"Custom reflected descriptor type differs from supplied typed binding.");
			}
			if(samplerCount)
			{
				VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.magFilter=sampler.minFilter=info.samplingFilter;
				sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
				Require(vkCreateSampler(info.device,&sampler,nullptr,&pass->sampler)==VK_SUCCESS,"Custom sampler allocation failed.");
			}
			if(count)
			{
				VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};set.bindingCount=static_cast<std::uint32_t>(layouts.size());set.pBindings=layouts.data();
				Require(vkCreateDescriptorSetLayout(info.device,&set,nullptr,&pass->descriptors)==VK_SUCCESS,"Custom descriptor layout failed.");
				std::vector<VkDescriptorPoolSize> sizes;
				for(const auto& binding:layouts) {auto it=std::find_if(sizes.begin(),sizes.end(),[&](const auto& s){return s.type==binding.descriptorType;});if(it==sizes.end())sizes.push_back({binding.descriptorType,1});else++it->descriptorCount;}
				VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=1;pool.poolSizeCount=static_cast<std::uint32_t>(sizes.size());pool.pPoolSizes=sizes.data();
				Require(vkCreateDescriptorPool(info.device,&pool,nullptr,&pass->pool)==VK_SUCCESS,"Custom descriptor pool failed.");
				VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocation.descriptorPool=pass->pool;allocation.descriptorSetCount=1;allocation.pSetLayouts=&pass->descriptors;
				Require(vkAllocateDescriptorSets(info.device,&allocation,&pass->set)==VK_SUCCESS,"Custom descriptor allocation failed.");
				for(std::size_t i=0;i<d.inputs.size();++i) if(d.inputs[i].combinedSampler) images[i].sampler=pass->sampler;
				if(d.samplerBinding) {images.push_back({pass->sampler,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED});imageBindings.push_back(*d.samplerBinding);types.push_back(VK_DESCRIPTOR_TYPE_SAMPLER);}
				std::vector<VkWriteDescriptorSet> writes(count);
				for(std::size_t i=0;i<count;++i){auto& w=writes[i];w={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=pass->set;w.dstBinding=imageBindings[i];w.descriptorCount=1;w.descriptorType=types[i];w.pImageInfo=&images[i];}
				vkUpdateDescriptorSets(info.device,static_cast<std::uint32_t>(count),writes.data(),0,nullptr);
			}
			VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,d.parameterBytes};
			VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout.setLayoutCount=count?1:0;layout.pSetLayouts=count?&pass->descriptors:nullptr;
			layout.pushConstantRangeCount=d.parameterBytes?1:0;layout.pPushConstantRanges=d.parameterBytes?&push:nullptr;
			Require(vkCreatePipelineLayout(info.device,&layout,nullptr,&pass->layout)==VK_SUCCESS,"Custom pipeline layout failed.");
			VkAttachmentDescription attachment{};attachment.format=output.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
			attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
			attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachment.initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;attachment.finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass.colorAttachmentCount=1;subpass.pColorAttachments=&color;
			VkRenderPassCreateInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};render.attachmentCount=1;render.pAttachments=&attachment;render.subpassCount=1;render.pSubpasses=&subpass;
			Require(vkCreateRenderPass(info.device,&render,nullptr,&pass->renderPass)==VK_SUCCESS,"Custom color render pass failed.");
			VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};framebuffer.renderPass=pass->renderPass;framebuffer.attachmentCount=1;framebuffer.pAttachments=&output.view;framebuffer.width=output.extent.width;framebuffer.height=output.extent.height;framebuffer.layers=1;
			Require(vkCreateFramebuffer(info.device,&framebuffer,nullptr,&pass->framebuffer)==VK_SUCCESS,"Custom output framebuffer failed.");
			Modules modules{info.device};VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};module.codeSize=info.fullscreenVertex.size_bytes();module.pCode=info.fullscreenVertex.data();
			Require(vkCreateShaderModule(info.device,&module,nullptr,&modules.vertex)==VK_SUCCESS,"Custom fullscreen vertex module failed.");
			module.codeSize=fragment.size()*4;module.pCode=fragment.data();Require(vkCreateShaderModule(info.device,&module,nullptr,&modules.fragment)==VK_SUCCESS,"Custom fragment module failed.");
			std::array<VkPipelineShaderStageCreateInfo,2> stages{};for(auto& s:stages)s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stages[0].stage=VK_SHADER_STAGE_MESH_BIT_EXT;stages[0].module=modules.vertex;stages[0].pName="main";
			stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=modules.fragment;stages[1].pName=d.entryPoint.c_str();
			VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};viewport.viewportCount=viewport.scissorCount=1;
			VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1;
			VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
			VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
			VkPipelineColorBlendAttachmentState write{};write.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
			VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&write;
			const std::array<VkDynamicState,2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=2;dynamic.pDynamicStates=dynamicStates.data();
			VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pipeline.stageCount=2;pipeline.pStages=stages.data();pipeline.pViewportState=&viewport;pipeline.pRasterizationState=&raster;pipeline.pMultisampleState=&samples;pipeline.pDepthStencilState=&depth;pipeline.pColorBlendState=&blend;pipeline.pDynamicState=&dynamic;pipeline.layout=pass->layout;pipeline.renderPass=pass->renderPass;
			Require(vkCreateGraphicsPipelines(info.device,VK_NULL_HANDLE,1,&pipeline,nullptr,&pass->pipeline)==VK_SUCCESS,"Custom compatible fullscreen pipeline failed.");
			reason.clear();return pass;
		}
		catch(const std::exception& error){try{reason=error.what();}catch(...){}return {};}
		catch(...){try{reason="Custom native preparation failed.";}catch(...){}return {};}
	}
	std::optional<CustomPostProcessRecordedOutput> RecordCustomPostProcessPass(VkCommandBuffer command,
		const CustomPostProcessPass& pass,std::span<const std::byte> pushBytes) noexcept
	{
		if(!command||!pass.pipeline||!pass.prepared||!ValidPacked(pass.prepared->descriptor,pushBytes))return {};
		const auto& output=pass.output;
		// Full overwrite discards prior content but orders all earlier accesses.
		// Recording never mutates layout truth for a command that may be discarded.
		VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=output.image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};barrier.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&barrier);
		VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=pass.renderPass;begin.framebuffer=pass.framebuffer;begin.renderArea.extent=output.extent;
		vkCmdBeginRenderPass(command,&begin,VK_SUBPASS_CONTENTS_INLINE);
		VkViewport viewport{0,0,float(output.extent.width),float(output.extent.height),0,1};VkRect2D scissor{{0,0},output.extent};vkCmdSetViewport(command,0,1,&viewport);vkCmdSetScissor(command,0,1,&scissor);
		vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pass.pipeline);
		if(pass.set)vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pass.layout,0,1,&pass.set,0,nullptr);
		if(!pushBytes.empty())vkCmdPushConstants(command,pass.layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,static_cast<std::uint32_t>(pushBytes.size()),pushBytes.data());
		vkCmdDrawMeshTasksEXT(command, 1, 1, 1);vkCmdEndRenderPass(command);
		barrier.oldLayout=barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
		CustomPostProcessRecordedOutput recorded;recorded.command=command;recorded.stage=pass.prepared->descriptor.stage;recorded.domain=pass.prepared->descriptor.domain;
		recorded.image=output;recorded.image.layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;recorded.descriptorRevision=pass.prepared->descriptor.revision;recorded.shaderPublicationGeneration=pass.prepared->shader->publicationGeneration;
		return recorded;
	}
}
