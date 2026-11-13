#include "TerrainSurfaceCoordinateInputs.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include "Tests/Editor/TestSha256.h"
#include "File Formats/TerraEngineShaderFile.h"
#include "Renderer/Terrain/TerrainMaterialRuntimeRegistry.h"
#include <nlohmann/json.hpp>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <cstddef>
#include <utility>

namespace TerraEngine::Tests::Review::TerrainCoordinates {
	Texel MaskTexelOracle(glm::dvec2 uv, Extent extent) {
		if(!extent.width || !extent.height || !std::isfinite(uv.x) || !std::isfinite(uv.y))
			throw std::runtime_error("Mask oracle requires finite UV and a real nonempty texture.");
		const auto axis = [](double value, std::uint32_t size) {
			return static_cast<std::uint32_t>(std::min<double>(std::floor(std::clamp(value, 0.0, 1.0) * size), size - 1U));
		};
		return {axis(uv.x, extent.width), axis(uv.y, extent.height)};
	}

	glm::dvec2 AbsoluteTerrainUvOracle(const SurfaceCase& value) {
		// Evaluate the authored address in double precision directly, independently
		// of the GPU's bounded phase/local split. Camera and screen values do not
		// enter this definition of the material's physical coordinate system.
		const double x = (static_cast<double>(value.area.x) * 256.0 + value.pageOrigin.x + value.pageLocal.x) * value.spacingMetres;
		const double z = (static_cast<double>(value.area.z) * 256.0 + value.pageOrigin.y + value.pageLocal.y) * value.spacingMetres;
		const double c = std::cos(value.mapping.rotationRadians), s = std::sin(value.mapping.rotationRadians);
		return {(c * x + s * z + value.mapping.offsetXMetres) / value.mapping.tileSizeXMetres,
			(-s * x + c * z + value.mapping.offsetZMetres) / value.mapping.tileSizeZMetres};
	}
	std::array<float, 4> PatternOracle(const SurfaceCase& value, glm::dvec2 pageLocal) {
		auto sample = value; sample.pageLocal = pageLocal; const auto uv = AbsoluteTerrainUvOracle(sample);
		// Native inputs use an upward normal: triplanar's Y projection samples
		// (authored X, -authored Z). A nearest/repeat 32x32 texture encodes texels.
		const auto address = [](double coordinate) { const double wrapped = coordinate - std::floor(coordinate); return std::min(31U, static_cast<std::uint32_t>(std::floor(wrapped * 32.0))); };
		return {static_cast<float>(address(uv.x)) / 31.0F, static_cast<float>(address(-uv.y)) / 31.0F, 0.25F, 1.0F};
	}
	std::vector<SurfaceCase> SurfaceCases() {
		Terrain::TerrainLayerMapping mapping; mapping.tileSizeXMetres = 7.0; mapping.tileSizeZMetres = 5.0;
		mapping.offsetXMetres = 0.37; mapping.offsetZMetres = 1.17; mapping.rotationRadians = 0.41;
		mapping.mode = Terrain::TerrainLayerMappingMode::Triplanar;
		return {
			{"origin-control", {0, 0}, 0.5, {0.0, 0.0}, {}, mapping},
			{"page-origin-left", {-7, 11}, 0.5, {37.0, 29.0}, {}, mapping},
			{"page-origin-right", {-7, 11}, 0.5, {39.0, 29.0}, {}, mapping},
			{"large-signed-page", {1'000'000, -1'000'001}, 0.5, {37.0, 29.0}, {}, mapping},
			{"negative-spacing-control", {-1, -2}, 2.0, {19.0, 53.0}, {}, mapping},
			{"negative-area-edge-left", {-1, 0}, 0.5, {253.0, 29.0}, {}, mapping},
			{"negative-area-edge-right", {0, 0}, 0.5, {0.0, 29.0}, {}, mapping},
			{"positive-area-edge-left", {0, 0}, 0.5, {253.0, 29.0}, {}, mapping},
			{"positive-area-edge-right", {1, 0}, 0.5, {0.0, 29.0}, {}, mapping}
		};
	}
	std::vector<MaskCase> MaskCases() {
		return {
			{"independent-hole-dimensions", {0.5, 0.5}, {17, 9}, {64, 32}, true, false},
			{"independent-fallback-dimensions", {0.4375, 0.3125}, {17, 9}, {64, 32}, false, true},
			{"healthy-control", {0.125, 0.625}, {17, 9}, {64, 32}, false, false},
			{"lower-uv-clamp", {-0.25, -0.5}, {17, 9}, {64, 32}, true, false},
			{"upper-uv-clamp", {1.25, 1.5}, {17, 9}, {64, 32}, true, false}
		};
	}
}

namespace TerraEngine::Tests::Review::TerrainCoordinates {
	namespace {
		using Json = nlohmann::json;
		constexpr std::array<std::string, 9> kRelocation{"-fvk-b-shift", "100", "0", "-fvk-u-shift", "200", "0", "-fvk-s-shift", "300", "0"};
		void Check(VkResult result, const char* operation) { if(result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result)); }
		std::vector<std::byte> ReadFile(const std::filesystem::path& path) {
			std::ifstream file(path, std::ios::binary | std::ios::ate); const auto size = file.tellg();
			if(!file || size <= 0 || size > 64 * 1024 * 1024) throw std::runtime_error("Missing/bounded shader artifact: " + path.string());
			std::vector<std::byte> bytes(static_cast<std::size_t>(size)); file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), size);
			if(!file) throw std::runtime_error("Truncated shader artifact."); return bytes;
		}
		std::string Hash(std::span<const std::byte> bytes) { return EditorTests::ComputeTestSha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())); }
		std::vector<std::uint32_t> PackageWords(const std::filesystem::path& path) {
			const auto bytes = ReadFile(path); ShaderHeader header{};
			if(bytes.size() < sizeof(header)) throw std::runtime_error("Truncated original TSPF header."); std::memcpy(&header, bytes.data(), sizeof(header));
			if(std::memcmp(header.magic, "TSPF", 4) || header.compiledBinaryOffset > bytes.size() || sizeof(CompiledStageBinary) > bytes.size() - header.compiledBinaryOffset) throw std::runtime_error("Invalid original TSPF envelope.");
			CompiledStageBinary binary{}; std::memcpy(&binary, bytes.data() + header.compiledBinaryOffset, sizeof(binary));
			if(binary.bytecodeOffset > bytes.size() || binary.bytecodeSize > bytes.size() - binary.bytecodeOffset || binary.bytecodeSize % 4U) throw std::runtime_error("Invalid original SPIR-V range.");
			std::vector<std::uint32_t> words(binary.bytecodeSize / 4U); std::memcpy(words.data(), bytes.data() + binary.bytecodeOffset, binary.bytecodeSize); return words;
		}
		struct Reflection {
			std::map<std::uint32_t, std::string> names;
			std::map<std::uint32_t, std::uint32_t> bindings, sets, arrayStrides;
			std::map<std::uint32_t, std::vector<std::uint32_t>> types, offsets;
			std::map<std::uint32_t, std::uint32_t> variableTypes;
			std::vector<std::uint32_t> normalized;
			Json document;
		};
		Reflection Reflect(const std::vector<std::uint32_t>& words) {
			if(words.size() < 5 || words[0] != 0x07230203U) throw std::runtime_error("Invalid SPIR-V module.");
			Reflection r; r.normalized = words;
			for(std::size_t i = 5; i < words.size();) {
				const auto count = words[i] >> 16U, opcode = words[i] & 0xffffU;
				if(!count || count > words.size() - i) throw std::runtime_error("Malformed SPIR-V instruction.");
				if(opcode == 5U && count >= 3U) {
					const char* text = reinterpret_cast<const char*>(&words[i + 2]); const auto capacity = (count - 2U) * 4U;
					const void* terminator = std::memchr(text, 0, capacity); if(!terminator) throw std::runtime_error("Unterminated SPIR-V name.");
					r.names[words[i + 1]] = std::string(text, static_cast<const char*>(terminator));
				}
				if(opcode == 71U && count >= 4U) {
					const auto id = words[i + 1], decoration = words[i + 2];
					if(decoration == 33U) { r.bindings[id] = words[i + 3]; r.normalized[i + 3] = 0; }
					if(decoration == 34U) r.sets[id] = words[i + 3];
					if(decoration == 6U) r.arrayStrides[id] = words[i + 3];
				}
				if(opcode == 72U && count >= 5U && words[i + 3] == 35U) {
					auto& offsets = r.offsets[words[i + 1]]; offsets.resize(std::max<std::size_t>(offsets.size(), words[i + 2] + 1U)); offsets[words[i + 2]] = words[i + 4];
				}
				if(opcode >= 19U && opcode <= 39U && count >= 2U) r.types[words[i + 1]] = {words.begin() + i + 2, words.begin() + i + count};
				if(opcode == 59U && count >= 4U) r.variableTypes[words[i + 2]] = words[i + 1];
				i += count;
			}
			r.document = Json{{"bindings", Json::array()}, {"memberOffsets", Json::array()}, {"arrayStrides", Json::array()}};
			for(const auto& [id, binding] : r.bindings) r.document["bindings"].push_back({{"name", r.names[id]}, {"set", r.sets[id]}, {"binding", binding}});
			for(const auto& [id, offsets] : r.offsets) r.document["memberOffsets"].push_back({{"name", r.names[id]}, {"offsets", offsets}});
			for(const auto& [id, stride] : r.arrayStrides) r.document["arrayStrides"].push_back({{"id", id}, {"stride", stride}});
			return r;
		}
		void RequireMappingAbi(Reflection& r, const char* mappingName) {
			std::uint32_t variable = 0;
			for(const auto& [id, name] : r.names) if(name == mappingName && r.variableTypes.contains(id)) variable = id;
			if(!variable || r.bindings[variable] != 100U || r.sets[variable] != 0U) throw std::runtime_error("Relocated mapping uniform binding not observed.");
			const auto pointer = r.types.at(r.variableTypes.at(variable));
			if(pointer.size() != 2) throw std::runtime_error("Mapping pointer shape differs.");
			const auto structure = pointer[1]; const auto members = r.types.at(structure);
			if(r.offsets.at(structure) != std::vector<std::uint32_t>{0, 16, 528, 532} || members.size() != 4) throw std::runtime_error("Mapping cbuffer offsets differ from the 544-byte host payload.");
			if(r.offsets.at(members[0]) != std::vector<std::uint32_t>{0, 8, 12} || r.arrayStrides.at(members[1]) != 32U) throw std::runtime_error("Area/layer mapping layout differs from the host.");
			const auto layerType = r.types.at(members[1]).at(0);
			if(r.offsets.at(layerType) != std::vector<std::uint32_t>{0, 8, 16, 24, 28}) throw std::runtime_error("Layer cbuffer offsets differ from host bytes.");
			std::uint32_t layers = 0; for(const auto& [id, name] : r.names) if(name == "terrainGpuLayers" && r.variableTypes.contains(id)) layers = id;
			const auto block = r.types.at(r.types.at(r.variableTypes.at(layers)).at(1)); const auto array = block.at(0);
			if(r.arrayStrides.at(array) != 96U || r.offsets.at(r.types.at(array).at(0)) != std::vector<std::uint32_t>{0,4,8,12,16,32,48,52,56,60,64,72,80,84,88,92}) throw std::runtime_error("Structured terrain layer ABI differs from the 96-byte host payload.");
		}
		std::vector<std::uint32_t> CompileCoordinateShader(const ShaderPaths& paths, const std::string& name, const char* profile, const char* mapping) {
			const auto source = paths.sourceRoot / "Engine/Shaders/Terrain" / (name + ".hlsl");
			const auto package = paths.originalPackages / (name + ".tspf"); const auto installedSource = paths.originalPackages / (name + ".hlsl");
			const auto originalBytes = ReadFile(package), sourceBytes = ReadFile(source), installedBytes = ReadFile(installedSource);
			if(Hash(sourceBytes) != Hash(installedBytes)) throw std::runtime_error("Original installed source differs from current coordinate shader; re-install before qualification.");
			const auto sharedSource = ReadFile(paths.sourceRoot / "Engine/Shaders/Terrain/TerrainMaterial.hlsli"), sharedInstalled = ReadFile(paths.originalPackages / "TerrainMaterial.hlsli");
			if(Hash(sharedSource) != Hash(sharedInstalled)) throw std::runtime_error("Installed shared terrain mapping source differs from the current shader include.");
			const auto original = PackageWords(package);
			const auto relocated = VulkanReviewFixture::CompileShader(source, profile, "main", paths.evidenceRoot / (name + ".relocated.spv"), kRelocation);
			auto before = Reflect(original), after = Reflect(relocated); RequireMappingAbi(after, mapping);
			const bool onlyBindingsChanged = before.normalized == after.normalized;
			Json receipt{{"scope", "ValidationOnlyTerrainCoordinateInstructions"}, {"installedTerrainPipelineAcceptance", "unverified-no-dedicated-consumer"},
				{"originalPackageSha256", Hash(originalBytes)}, {"currentSourceSha256", Hash(sourceBytes)}, {"installedSourceSha256", Hash(installedBytes)},
				{"sharedMappingSourceSha256", Hash(sharedSource)}, {"installedSharedMappingSourceSha256", Hash(sharedInstalled)},
				{"originalReflection", before.document}, {"relocatedReflection", after.document}, {"additionalFlags", kRelocation}, {"onlyBindingDecorationsChanged", onlyBindingsChanged}, {"hostMappingBytes", 544}, {"hostLayerStride", 96}};
			std::filesystem::create_directories(paths.evidenceRoot); std::ofstream(paths.evidenceRoot / (name + ".coordinate-abi.json")) << receipt.dump(2);
			if(!onlyBindingsChanged) throw std::runtime_error("Validation recompile changed more than binding decorations; coordinate-instruction proof is unavailable.");
			return relocated;
		}
		struct alignas(16) AreaConstants { std::array<float, 2> origin{}; float spacing = 1.0F; std::uint32_t extent = 256U; };
		struct LayerConstants { std::array<float, 2> tile{7,5}, phase{}, rotation{1,0}; float sharpness = 4; std::uint32_t mode = 1; };
		struct alignas(16) MappingConstants { AreaConstants area; std::array<LayerConstants, 16> layers{}; std::uint32_t count = 1; std::array<std::uint32_t, 3> padding{}; };
		static_assert(sizeof(AreaConstants) == 16 && sizeof(LayerConstants) == 32 && sizeof(MappingConstants) == 544);
		static_assert(offsetof(AreaConstants, origin) == 0 && offsetof(AreaConstants, spacing) == 8 && offsetof(AreaConstants, extent) == 12);
		static_assert(offsetof(LayerConstants, tile) == 0 && offsetof(LayerConstants, phase) == 8 && offsetof(LayerConstants, rotation) == 16 && offsetof(LayerConstants, sharpness) == 24 && offsetof(LayerConstants, mode) == 28);
		static_assert(offsetof(MappingConstants, area) == 0 && offsetof(MappingConstants, layers) == 16 && offsetof(MappingConstants, count) == 528 && offsetof(MappingConstants, padding) == 532);
		using LayerBinding = Renderer::Terrain::TerrainGpuLayerBinding;
		static_assert(sizeof(LayerBinding) == 96);
		struct Handles {
			VkDescriptorSetLayout setLayout{}; VkDescriptorPool pool{}; VkDescriptorSet set{}; VkPipelineLayout layout{};
			VkPipeline pipeline{}; std::array<VkShaderModule, 2> modules{}; VkSampler sampler{}; VkRenderPass renderPass{}; VkFramebuffer framebuffer{};
		};
		std::shared_ptr<Handles> OwnHandles(VulkanReviewFixture& fixture) {
			auto handles = std::make_shared<Handles>();
			fixture.RegisterCleanup([handles](const VulkanReviewFixture::Native& native) {
				const auto& f = *native.functions; const auto d = native.device;
				if(handles->pipeline) f.vkDestroyPipeline(d, handles->pipeline, nullptr);
				if(handles->framebuffer) f.vkDestroyFramebuffer(d, handles->framebuffer, nullptr);
				if(handles->renderPass) f.vkDestroyRenderPass(d, handles->renderPass, nullptr);
				if(handles->layout) f.vkDestroyPipelineLayout(d, handles->layout, nullptr);
				if(handles->pool) f.vkDestroyDescriptorPool(d, handles->pool, nullptr);
				if(handles->setLayout) f.vkDestroyDescriptorSetLayout(d, handles->setLayout, nullptr);
				if(handles->sampler) f.vkDestroySampler(d, handles->sampler, nullptr);
				for(auto module : handles->modules) if(module) f.vkDestroyShaderModule(d, module, nullptr);
				*handles = Handles{};
			}); return handles;
		}
		VulkanReviewFixture::Options Options(bool compute) {
			VulkanReviewFixture::Options options; options.requiredDeviceName = "NVIDIA GeForce RTX 4080 Laptop GPU"; options.requiredVendor = 0x10de;
			options.requireValidation = true; options.requireSampledImageArrayDynamicIndexing = true; options.requireSampledImageArrayNonUniformIndexing = true;
			options.requireShaderDemoteToHelperInvocation = !compute;
			options.requireComputeDerivativeGroupQuads = compute; return options;
		}
		VkShaderModule Module(const VulkanReviewFixture::Native& native, const std::vector<std::uint32_t>& words) {
			VkShaderModuleCreateInfo create{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; create.codeSize = words.size() * 4U; create.pCode = words.data(); VkShaderModule module{};
			Check(native.functions->vkCreateShaderModule(native.device, &create, nullptr, &module), "create actual coordinate shader"); return module;
		}
		void Setup(VulkanReviewFixture& fixture, Handles& handles, bool compute) {
			const auto& native = fixture.GetNative(); const auto& f = *native.functions;
			const VkShaderStageFlags stage = compute ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
			std::vector<VkDescriptorSetLayoutBinding> bindings;
			for(std::uint32_t i = 0; i < (compute ? 2U : 4U); ++i) bindings.push_back({i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stage, nullptr});
			bindings.push_back({8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64, stage, nullptr}); bindings.push_back({72, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stage, nullptr});
			bindings.push_back({100, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stage, nullptr}); bindings.push_back({308, VK_DESCRIPTOR_TYPE_SAMPLER, 64, stage, nullptr});
			if(compute) for(std::uint32_t i = 0; i < 5; ++i) bindings.push_back({200U + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, stage, nullptr});
			VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; setInfo.bindingCount = static_cast<std::uint32_t>(bindings.size()); setInfo.pBindings = bindings.data();
			Check(f.vkCreateDescriptorSetLayout(native.device, &setInfo, nullptr, &handles.setLayout), "create relocated descriptor layout");
			const std::array<VkDescriptorPoolSize, 5> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 68}, {VK_DESCRIPTOR_TYPE_SAMPLER, 64}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}}};
			VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets = 1; pool.poolSizeCount = sizes.size(); pool.pPoolSizes = sizes.data();
			Check(f.vkCreateDescriptorPool(native.device, &pool, nullptr, &handles.pool), "create coordinate descriptor pool");
			VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool = handles.pool; allocate.descriptorSetCount = 1; allocate.pSetLayouts = &handles.setLayout;
			Check(f.vkAllocateDescriptorSets(native.device, &allocate, &handles.set), "allocate coordinate descriptors");
			VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST; sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT; sampler.maxLod = 0;
			Check(f.vkCreateSampler(native.device, &sampler, nullptr, &handles.sampler), "create explicit nearest-repeat sampler");
			VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 16}; VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount = 1; layout.pSetLayouts = &handles.setLayout;
			if(!compute) { layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push; }
			Check(f.vkCreatePipelineLayout(native.device, &layout, nullptr, &handles.layout), "create coordinate pipeline layout");
		}
		template<class T> std::span<const std::byte> Bytes(const T& value) { return std::as_bytes(std::span{&value, 1}); }
		template<class T> std::span<const std::byte> Bytes(const std::vector<T>& value) { return std::as_bytes(std::span{value}); }
		void ImageDescriptor(const VulkanReviewFixture::Native& native, Handles& handles, std::uint32_t binding, VkDescriptorType type, std::span<const VkDescriptorImageInfo> images) {
			VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet = handles.set; write.dstBinding = binding; write.descriptorType = type; write.descriptorCount = static_cast<std::uint32_t>(images.size()); write.pImageInfo = images.data(); native.functions->vkUpdateDescriptorSets(native.device, 1, &write, 0, nullptr);
		}
		void BufferDescriptor(const VulkanReviewFixture::Native& native, Handles& handles, std::uint32_t binding, VkDescriptorType type, const VulkanReviewBuffer& buffer) {
			VkDescriptorBufferInfo info{buffer.handle, 0, buffer.bytes}; VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet = handles.set; write.dstBinding = binding; write.descriptorType = type; write.descriptorCount = 1; write.pBufferInfo = &info; native.functions->vkUpdateDescriptorSets(native.device, 1, &write, 0, nullptr);
		}
		VulkanReviewImage Image(VulkanReviewFixture& fixture, std::uint32_t width, std::uint32_t height, VkFormat format, std::span<const std::byte> contents) {
			auto image = fixture.CreateImage(width, height, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT); fixture.UploadImage(image, contents); return image;
		}
		void CommonDescriptors(VulkanReviewFixture& fixture, Handles& handles, const VulkanReviewImage& weights, const VulkanReviewImage& ids, const VulkanReviewImage& texture, const VulkanReviewBuffer& uniform, const VulkanReviewBuffer& layers) {
			const auto& native = fixture.GetNative();
			for(const auto& [binding, image] : {std::pair{0U, weights}, std::pair{1U, ids}}) { const VkDescriptorImageInfo info{VK_NULL_HANDLE, image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; ImageDescriptor(native, handles, binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, {&info, 1}); }
			std::array<VkDescriptorImageInfo, 64> textures{}, samplers{};
			for(std::size_t i = 0; i < textures.size(); ++i) { textures[i] = {VK_NULL_HANDLE, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; samplers[i].sampler = handles.sampler; }
			ImageDescriptor(native, handles, 8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, textures); ImageDescriptor(native, handles, 308, VK_DESCRIPTOR_TYPE_SAMPLER, samplers);
			BufferDescriptor(native, handles, 72, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, layers); BufferDescriptor(native, handles, 100, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uniform);
		}
		std::vector<std::array<float, 4>> DecodeColor(const std::vector<std::byte>& bytes) {
			if(bytes.size() % 16U) throw std::runtime_error("Fenced color readback size mismatch."); std::vector<std::array<float, 4>> result(bytes.size() / 16U); std::memcpy(result.data(), bytes.data(), bytes.size()); return result;
		}
		std::string PersistRawReadback(const ShaderPaths& paths, const std::string& caseName, std::span<const std::byte> bytes) {
			// Preserve the complete observed bytes, including unexpected pixels. This
			// artifact is written directly from fenced readback, never from an oracle.
			const auto path = paths.evidenceRoot / (caseName + ".actual-gpu-readback.rgba32f.bin");
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			if(!output) throw std::runtime_error("Cannot create actual GPU readback artifact.");
			output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); output.close();
			if(!output) throw std::runtime_error("Cannot completely persist actual GPU readback bytes.");
			return path.filename().generic_string();
		}
	}
}

namespace TerraEngine::Tests::Review::TerrainCoordinates {
	std::vector<NativePixels> RunSurfaceCoordinateInstructions(std::span<const SurfaceCase> cases, const ShaderPaths& paths) {
		std::filesystem::create_directories(paths.evidenceRoot);
		const auto words = CompileCoordinateShader(paths, "TerrainSurface.comp", "cs_6_8", "TerrainSurfaceMapping");
		VulkanReviewFixture fixture("Native.ReviewSimulation.TerrainCoordinateInstruction.TriplanarPageOrigin", Options(true));
		const auto& native = fixture.GetNative(); const auto& f = *native.functions; auto handles = OwnHandles(fixture); Setup(fixture, *handles, true);
		fixture.RequireShaderCapabilities(words);
		handles->modules[0] = Module(native, words);
		VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pipeline.layout = handles->layout;
		pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipeline.stage.module = handles->modules[0]; pipeline.stage.pName = "main";
		Check(f.vkCreateComputePipelines(native.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &handles->pipeline), "create actual surface-coordinate compute pipeline");
		std::vector<std::array<float, 4>> weightValues(16, {1, 0, 0, 0}), idValues(16, {1.0F / 65535.0F, 0, 0, 0}), pattern(32U * 32U);
		for(std::uint32_t y = 0; y < 32; ++y) for(std::uint32_t x = 0; x < 32; ++x) pattern[y * 32U + x] = {x / 31.0F, y / 31.0F, 0.25F, 1};
		const auto weights = Image(fixture, 4, 4, VK_FORMAT_R32G32B32A32_SFLOAT, Bytes(weightValues));
		const auto ids = Image(fixture, 4, 4, VK_FORMAT_R32G32B32A32_SFLOAT, Bytes(idValues));
		const auto texture = Image(fixture, 32, 32, VK_FORMAT_R32G32B32A32_SFLOAT, Bytes(pattern));
		MappingConstants constants; LayerBinding layer;
		const auto uniform = fixture.CreateBuffer(sizeof(constants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, Bytes(constants));
		const auto layers = fixture.CreateBuffer(sizeof(layer), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Bytes(layer));
		CommonDescriptors(fixture, *handles, weights, ids, texture, uniform, layers);
		std::array<VulkanReviewImage, 5> outputs;
		for(std::size_t i = 0; i < outputs.size(); ++i) {
			outputs[i] = fixture.CreateImage(4, 4, i == 4 ? VK_FORMAT_R32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
			const VkDescriptorImageInfo info{VK_NULL_HANDLE, outputs[i].view, VK_IMAGE_LAYOUT_GENERAL}; ImageDescriptor(native, *handles, 200U + static_cast<std::uint32_t>(i), VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, {&info, 1});
		}
		Json receipt{{"scope", "ValidationOnlyTerrainCoordinateInstructions"}, {"installedPipelineAcceptance", "unverified"}, {"cases", Json::array()}};
		std::vector<NativePixels> results;
		for(const auto& value : cases) {
			Terrain::TerrainAreaMaterialPhase phase;
			if(!Terrain::PrepareTerrainAreaMaterialPhase(value.area, value.spacingMetres, value.mapping, phase).Succeeded()) throw std::runtime_error("Production CPU terrain phase rejected fixture inputs.");
			constants.area.origin = {static_cast<float>(value.pageOrigin.x), static_cast<float>(value.pageOrigin.y)}; constants.area.spacing = static_cast<float>(value.spacingMetres);
			constants.layers[0].tile = {static_cast<float>(value.mapping.tileSizeXMetres), static_cast<float>(value.mapping.tileSizeZMetres)};
			constants.layers[0].phase = {static_cast<float>(phase.phaseMetres.x), static_cast<float>(phase.phaseMetres.y)};
			constants.layers[0].rotation = {static_cast<float>(std::cos(value.mapping.rotationRadians)), static_cast<float>(-std::sin(value.mapping.rotationRadians))};
			layer.baseColorTextureIndex = 0; layer.stableLayerId = 1; layer.tileSizeMetres = constants.layers[0].tile; layer.inverseRotationCosSin = constants.layers[0].rotation;
			layer.mappingMode = Renderer::Terrain::TerrainGpuMappingMode::Triplanar;
			fixture.Upload(uniform, Bytes(constants)); fixture.Upload(layers, Bytes(layer));
			fixture.RequireNoValidationErrors();
			VulkanReviewBuffer readback;
			const auto completion = fixture.Submit([&](VkCommandBuffer command, const auto&) {
				std::array<VkImageMemoryBarrier, 5> barriers{};
				for(std::size_t i = 0; i < barriers.size(); ++i) {
					barriers[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barriers[i].image = outputs[i].handle; barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
					barriers[i].srcQueueFamilyIndex = barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}; barriers[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
				}
				f.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, barriers.size(), barriers.data());
				f.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, handles->pipeline); f.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, handles->layout, 0, 1, &handles->set, 0, nullptr);
				// The 4x4 logical domain is aligned to complete 2x2 derivative quads;
				// every other quad in this 8x8 group returns before accessing an image.
				f.vkCmdDispatch(command, 1, 1, 1); readback = fixture.CopyImageToReadback(command, outputs[0], 16U * 16U);
			});
			if(!completion.completed || !completion.submitted || completion.result != VK_SUCCESS) throw std::runtime_error("Surface instruction submission did not reach actual fence completion.");
			const auto bytes = fixture.Read(readback); const auto rawArtifact = PersistRawReadback(paths, value.name, bytes); auto color = DecodeColor(bytes);
			receipt["cases"].push_back({{"name", value.name}, {"area", {value.area.x, value.area.z}}, {"pageOrigin", {value.pageOrigin.x, value.pageOrigin.y}}, {"phaseMetres", {phase.phaseMetres.x, phase.phaseMetres.y}},
				{"hostConstantsSha256", Hash(Bytes(constants))}, {"readbackSha256", Hash(bytes)}, {"rawReadbackArtifact", rawArtifact}, {"rawReadbackBytes", bytes.size()}, {"rawReadbackProvenance", "direct-fenced-GPU-readback"}, {"pixels", color}, {"submitSerial", completion.submitSerial}, {"completionSerial", completion.completionSerial}, {"fullDerivativeQuads", true}});
			results.push_back({value.name, 4, 4, std::move(color), completion});
		}
		std::ofstream(paths.evidenceRoot / "surface-coordinate-results.json") << receipt.dump(2);
		// Teardown is part of native acceptance. Finish releases completed domain
		// objects and reports cleanup-only validation before the caller's oracle.
		try { fixture.Finish(); }
		catch(...) { try { fixture.WriteEvidence(paths.evidenceRoot / "surface-native-host.json"); } catch(...) {} throw; }
		fixture.WriteEvidence(paths.evidenceRoot / "surface-native-host.json"); return results;
	}
}


namespace TerraEngine::Tests::Review::TerrainCoordinates {
	std::vector<NativePixels> RunVisibilityCoordinateInstructions(std::span<const MaskCase> cases, const ShaderPaths& paths) {
		std::filesystem::create_directories(paths.evidenceRoot);
		const auto fragment = CompileCoordinateShader(paths, "TerrainVisibility.frag", "ps_6_8", "TerrainVisibilityMapping");
		const auto vertex = VulkanReviewFixture::CompileShader(paths.sourceRoot / "Engine/Shaders/Tests/ReviewRemediation/TerrainVisibilityCoordinates.vert.hlsl", "vs_6_8", "main", paths.evidenceRoot / "TerrainVisibilityCoordinates.vert.spv");
		VulkanReviewFixture fixture("Native.ReviewSimulation.TerrainCoordinateInstruction.MaskAddressing", Options(false));
		const auto& native = fixture.GetNative(); const auto& f = *native.functions; auto handles = OwnHandles(fixture); Setup(fixture, *handles, false);
		fixture.RequireShaderCapabilities(vertex); fixture.RequireShaderCapabilities(fragment);
		handles->modules[0] = Module(native, vertex); handles->modules[1] = Module(native, fragment);
		const auto target = fixture.CreateImage(12, 8, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
		VkAttachmentDescription attachment{}; attachment.format = target.format; attachment.samples = VK_SAMPLE_COUNT_1_BIT; attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}; VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &reference;
		VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; pass.attachmentCount = 1; pass.pAttachments = &attachment; pass.subpassCount = 1; pass.pSubpasses = &subpass;
		Check(f.vkCreateRenderPass(native.device, &pass, nullptr, &handles->renderPass), "create coordinate-only color render pass");
		VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; framebuffer.renderPass = handles->renderPass; framebuffer.attachmentCount = 1; framebuffer.pAttachments = &target.view; framebuffer.width = 12; framebuffer.height = 8; framebuffer.layers = 1;
		Check(f.vkCreateFramebuffer(native.device, &framebuffer, nullptr, &handles->framebuffer), "create coordinate framebuffer");
		std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
		for(std::size_t i = 0; i < stages.size(); ++i) { stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; stages[i].stage = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT; stages[i].module = handles->modules[i]; stages[i].pName = "main"; }
		VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewportState.viewportCount = viewportState.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1;
		VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineColorBlendAttachmentState blendAttachment{}; blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = 1; blend.pAttachments = &blendAttachment;
		const std::array<VkDynamicState, 2> dynamics{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR}; VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = dynamics.size(); dynamic.pDynamicStates = dynamics.data();
		VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; pipeline.stageCount = stages.size(); pipeline.pStages = stages.data(); pipeline.pVertexInputState = &input; pipeline.pInputAssemblyState = &assembly; pipeline.pViewportState = &viewportState; pipeline.pRasterizationState = &raster; pipeline.pMultisampleState = &samples; pipeline.pColorBlendState = &blend; pipeline.pDynamicState = &dynamic; pipeline.layout = handles->layout; pipeline.renderPass = handles->renderPass;
		Check(f.vkCreateGraphicsPipelines(native.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &handles->pipeline), "create actual visibility-fragment coordinate pipeline");
		std::vector<std::array<float, 4>> weightValues(64, {1,0,0,0}), idValues(64, {1.0F / 65535.0F,0,0,0}), textureValues(1, {1,1,1,1});
		const auto weights = Image(fixture, 8, 8, VK_FORMAT_R32G32B32A32_SFLOAT, Bytes(weightValues));
		const auto ids = Image(fixture, 8, 8, VK_FORMAT_R32G32B32A32_SFLOAT, Bytes(idValues)); const auto texture = Image(fixture, 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, Bytes(textureValues));
		MappingConstants constants; constants.layers[0].tile = {4,4}; constants.layers[0].mode = 0;
		LayerBinding layer; layer.stableLayerId = 1; layer.baseColorFactor = {0.1F,0.4F,0.8F,1}; layer.mappingMode = Renderer::Terrain::TerrainGpuMappingMode::TerrainXZ;
		const auto uniform = fixture.CreateBuffer(sizeof(constants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, Bytes(constants)); const auto layers = fixture.CreateBuffer(sizeof(layer), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Bytes(layer));
		CommonDescriptors(fixture, *handles, weights, ids, texture, uniform, layers);
		std::vector<NativePixels> results;
		Json receipt{{"scope", "ValidationOnlyTerrainCoordinateInstructions"}, {"installedColorShadowPickAcceptance", "unverified-no-dedicated-consumer"}, {"screenSamples", {{1,1},{7,4}}}, {"cases", Json::array()}};
		for(const auto& value : cases) {
			// Old screen coordinates remain in bounds for every submitted fragment.
			// The red result is wrong authored addressing, never undefined OOB loads.
			if(value.holes.width < 12 || value.holes.height < 8 || value.fallback.width < 12 || value.fallback.height < 8) throw std::runtime_error("Pre-fix visibility fixture would permit an out-of-bounds screen-space fetch.");
			std::vector<std::uint32_t> holeValues(value.holes.width * value.holes.height), fallbackValues(value.fallback.width * value.fallback.height);
			const auto holeAddress = MaskTexelOracle(value.uv, value.holes), fallbackAddress = MaskTexelOracle(value.uv, value.fallback);
			if(value.hole) holeValues[holeAddress.y * value.holes.width + holeAddress.x] = 1;
			if(value.diagnosticFallback) fallbackValues[fallbackAddress.y * value.fallback.width + fallbackAddress.x] = 1;
			const auto holes = Image(fixture, value.holes.width, value.holes.height, VK_FORMAT_R32_UINT, Bytes(holeValues)); const auto fallback = Image(fixture, value.fallback.width, value.fallback.height, VK_FORMAT_R32_UINT, Bytes(fallbackValues));
			for(const auto& [binding, image] : {std::pair{2U, fallback}, std::pair{3U, holes}}) { const VkDescriptorImageInfo info{VK_NULL_HANDLE,image.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; ImageDescriptor(native, *handles, binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, {&info,1}); }
			fixture.RequireNoValidationErrors();
			VulkanReviewBuffer readback;
			const auto completion = fixture.Submit([&](VkCommandBuffer command, const auto&) {
				VkClearValue clear{}; clear.color.float32[0] = 0.25F; clear.color.float32[2] = 0.25F; clear.color.float32[3] = 1;
				VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; begin.renderPass = handles->renderPass; begin.framebuffer = handles->framebuffer; begin.renderArea.extent = {12,8}; begin.clearValueCount = 1; begin.pClearValues = &clear;
				f.vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE); f.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,handles->pipeline);
				f.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,handles->layout,0,1,&handles->set,0,nullptr);
				const std::array<float,4> push{static_cast<float>(value.uv.x),static_cast<float>(value.uv.y),0,0}; f.vkCmdPushConstants(command,handles->layout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),push.data());
				for(const auto origin : {std::array<float,2>{0,0},std::array<float,2>{6,3}}) {
					const VkViewport viewport{origin[0],origin[1],4,4,0,1}; const VkRect2D scissor{{0,0},{12,8}}; f.vkCmdSetViewport(command,0,1,&viewport); f.vkCmdSetScissor(command,0,1,&scissor); f.vkCmdDraw(command,3,1,0,0);
				}
				f.vkCmdEndRenderPass(command); readback = fixture.CopyImageToReadback(command,target,12U*8U*16U,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
			});
			if(!completion.completed || !completion.submitted || completion.result != VK_SUCCESS) throw std::runtime_error("Visibility instruction submission did not reach actual fence completion.");
			const auto bytes = fixture.Read(readback); const auto rawArtifact = PersistRawReadback(paths, value.name, bytes); const auto pixels = DecodeColor(bytes); std::vector<std::array<float,4>> samplesOut{pixels.at(1U+12U),pixels.at(7U+4U*12U)};
			receipt["cases"].push_back({{"name",value.name},{"uv",{value.uv.x,value.uv.y}},{"holeExtent",{value.holes.width,value.holes.height}},{"fallbackExtent",{value.fallback.width,value.fallback.height}},
				{"oracleHoleTexel",{holeAddress.x,holeAddress.y}},{"oracleFallbackTexel",{fallbackAddress.x,fallbackAddress.y}},{"sampledPixels",samplesOut},{"readbackSha256",Hash(bytes)},{"rawReadbackArtifact",rawArtifact},{"rawReadbackBytes",bytes.size()},{"rawReadbackProvenance","direct-fenced-GPU-readback"},{"submitSerial",completion.submitSerial},{"completionSerial",completion.completionSerial},{"oldScreenFetchesInBounds",true}});
			results.push_back({value.name,12,8,std::move(samplesOut),completion});
		}
		std::ofstream(paths.evidenceRoot / "visibility-coordinate-results.json") << receipt.dump(2);
		try { fixture.Finish(); }
		catch(...) { try { fixture.WriteEvidence(paths.evidenceRoot / "visibility-native-host.json"); } catch(...) {} throw; }
		fixture.WriteEvidence(paths.evidenceRoot / "visibility-native-host.json"); return results;
	}
}
