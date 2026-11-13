#include "TestHarness.h"
#include <stdexcept>
#include <string_view>
#include <chrono>
#include <atomic>
#include "Tests/Support/ReviewRemediation/VulkanReviewFixture.h"
#include "Renderer/RHI/Vulkan/VulkanVirtualGeometryExecutor.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>

namespace TerraEngine::Tests
{
	namespace
	{
		// Retain each invocation separately. Exclusive directory creation plus a
		// monotonic time/counter suffix prevents concurrent/repeated evidence writes
		// from replacing an earlier native receipt; collisions retry without deletion.
		std::filesystem::path FreshEvidenceDirectory(const std::filesystem::path& parent, std::string_view label) {
			static std::atomic_uint64_t next{0};
			std::filesystem::create_directories(parent);
			for (unsigned attempt = 0; attempt < 100; ++attempt) {
				const auto path = parent / (std::string(label) + "-"
					+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-"
					+ std::to_string(next.fetch_add(1u, std::memory_order_relaxed)));
				if (path.parent_path() != parent) throw std::runtime_error("Evidence label must remain within its owned parent.");
				if (std::filesystem::create_directory(path)) return path;
			}
			throw std::runtime_error("Could not reserve a fresh evidence directory.");
		}

		using Review::VulkanReviewFixture;
		void VkCheck(VkResult result) { if (result != VK_SUCCESS) throw std::runtime_error("Native shader object creation failed: " + std::to_string(result)); }
		template<class T> std::span<const std::byte> Bytes(const T& value) { return std::as_bytes(std::span(&value, 1)); }
		std::filesystem::path Artifact(const char* name)
		{
			return FreshEvidenceDirectory(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "out/validation/review-remediation/shader-view-contracts", name);
		}
		VulkanReviewFixture::Options NativeOptions()
		{
			return VulkanReviewFixture::NativeDeviceOptions();
		}
		void FinishEvidence(VulkanReviewFixture& fixture, const std::filesystem::path& output)
		{
			try { fixture.Finish(); }
			catch (...) { fixture.WriteEvidence(output); throw; }
			fixture.WriteEvidence(output);
		}
		bool EnterNative(TestContext& context, const std::string& name)
		{
			if (VulkanReviewFixture::IsWatchedChild(name)) return true;
			const auto child = VulkanReviewFixture::RunWatchedSelf(name);
			context.Expect(child.started && !child.timedOut && child.exitCode == 0,
				"Watched native case must complete with real output (exit " + std::to_string(child.exitCode) + "): " + child.output);
			return false;
		}
		VkShaderModule Module(VulkanReviewFixture& fixture, const std::vector<std::uint32_t>& words)
		{
			fixture.RequireShaderCapabilities(words);
			const auto& n = fixture.GetNative(); VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			info.codeSize = words.size() * 4; info.pCode = words.data(); VkShaderModule module{};
			VkCheck(n.functions->vkCreateShaderModule(n.device, &info, nullptr, &module));
			fixture.RegisterCleanup([module](const auto& native) { native.functions->vkDestroyShaderModule(native.device, module, nullptr); }); return module;
		}
		struct Program { VkPipelineLayout layout{}; VkPipeline pipeline{}; std::vector<VkDescriptorSet> sets; };
		Program Layout(VulkanReviewFixture& fixture,
			const std::vector<std::vector<VkDescriptorSetLayoutBinding>>& bindings, std::uint32_t pushBytes,
			VkShaderStageFlags pushStages = VK_SHADER_STAGE_COMPUTE_BIT)
		{
			const auto& n = fixture.GetNative(); Program result; std::vector<VkDescriptorSetLayout> layouts;
			std::map<VkDescriptorType, std::uint32_t> counts;
			for (const auto& set : bindings)
			{
				VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; info.bindingCount = static_cast<std::uint32_t>(set.size()); info.pBindings = set.data();
				VkDescriptorSetLayout layout{}; VkCheck(n.functions->vkCreateDescriptorSetLayout(n.device, &info, nullptr, &layout)); layouts.push_back(layout);
				fixture.RegisterCleanup([layout](const auto& native) { native.functions->vkDestroyDescriptorSetLayout(native.device, layout, nullptr); });
				for (const auto& binding : set) counts[binding.descriptorType] += binding.descriptorCount;
			}
			std::vector<VkDescriptorPoolSize> sizes; for (auto [type, count] : counts) sizes.push_back({type, count});
			VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; poolInfo.maxSets = static_cast<std::uint32_t>(layouts.size()); poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size()); poolInfo.pPoolSizes = sizes.data();
			VkDescriptorPool pool{}; VkCheck(n.functions->vkCreateDescriptorPool(n.device, &poolInfo, nullptr, &pool));
			fixture.RegisterCleanup([pool](const auto& native) { native.functions->vkDestroyDescriptorPool(native.device, pool, nullptr); });
			VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool = pool; allocate.descriptorSetCount = static_cast<std::uint32_t>(layouts.size()); allocate.pSetLayouts = layouts.data();
			result.sets.resize(layouts.size()); VkCheck(n.functions->vkAllocateDescriptorSets(n.device, &allocate, result.sets.data()));
			VkPushConstantRange range{pushStages, 0, pushBytes}; VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			pipelineLayout.setLayoutCount = static_cast<std::uint32_t>(layouts.size()); pipelineLayout.pSetLayouts = layouts.data(); pipelineLayout.pushConstantRangeCount = 1; pipelineLayout.pPushConstantRanges = &range;
			VkCheck(n.functions->vkCreatePipelineLayout(n.device, &pipelineLayout, nullptr, &result.layout));
			fixture.RegisterCleanup([layout = result.layout](const auto& native) { native.functions->vkDestroyPipelineLayout(native.device, layout, nullptr); });
			return result;
		}
		Program Compute(VulkanReviewFixture& fixture, const std::vector<std::uint32_t>& words,
			const std::vector<std::vector<VkDescriptorSetLayoutBinding>>& bindings, std::uint32_t pushBytes,
			VkShaderStageFlags pushStages = VK_SHADER_STAGE_COMPUTE_BIT)
		{
			const auto& n = fixture.GetNative(); auto result = Layout(fixture, bindings, pushBytes, pushStages);
			const auto module = Module(fixture, words);
			VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pipeline.layout = result.layout;
			pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipeline.stage.module = module; pipeline.stage.pName = "main";
			VkCheck(n.functions->vkCreateComputePipelines(n.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &result.pipeline));
			fixture.RegisterCleanup([pipeline = result.pipeline](const auto& native) { native.functions->vkDestroyPipeline(native.device, pipeline, nullptr); }); return result;
		}
		void BufferBinding(VulkanReviewFixture& fixture, VkDescriptorSet set, std::uint32_t binding, const Review::VulkanReviewBuffer& buffer)
		{
			VkDescriptorBufferInfo info{buffer.handle, 0, buffer.bytes}; VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			write.dstSet = set; write.dstBinding = binding; write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.descriptorCount = 1; write.pBufferInfo = &info;
			const auto& n = fixture.GetNative(); n.functions->vkUpdateDescriptorSets(n.device, 1, &write, 0, nullptr);
		}
		void ImageBinding(VulkanReviewFixture& fixture, VkDescriptorSet set, std::uint32_t binding, VkDescriptorType type, VkImageView view, VkImageLayout layout, VkSampler sampler = VK_NULL_HANDLE)
		{
			VkDescriptorImageInfo image{sampler, view, layout}; VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			write.dstSet = set; write.dstBinding = binding; write.descriptorType = type; write.descriptorCount = 1; write.pImageInfo = &image;
			const auto& n = fixture.GetNative(); n.functions->vkUpdateDescriptorSets(n.device, 1, &write, 0, nullptr);
		}
		void Vrs(TestContext& context, const std::string& name)
		{
			if (!EnterNative(context, name)) return;
			auto options = NativeOptions();
			VulkanReviewFixture fixture(name, options); const auto directory = Artifact("vrs");
			const auto words = VulkanReviewFixture::CompileShader(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/PostProcess/VRSGenerate.comp.hlsl", "cs_6_0", "main", directory / "production.spv", {}, true);
			const auto program = Compute(fixture, words, {{{14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {15, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}}, 12);
			nlohmann::json observations = nlohmann::json::array(); bool allMatch = true;
			for (int scenario = 0; scenario < 3; ++scenario)
			{
				// Half-float constants are exact: .25=0x3400, 4=0x4400. The odd
				// viewport also exercises a partial edge tile without a CPU mirror.
				const std::uint32_t width = 9, height = 9, tile = 8;
				std::vector<std::uint16_t> input(width * height * 4, scenario == 0 ? 0x3400u : 0x4400u);
				if (scenario == 2) for (std::size_t pixel = 0; pixel < width * height; ++pixel)
					for (int lane = 0; lane < 3; ++lane) input[pixel * 4 + lane] = (pixel % width) < 4 ? 0u : 0x4400u;
				const auto source = fixture.CreateImage(width, height, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT);
				fixture.UploadImage(source, std::as_bytes(std::span(input)));
				// Match the actual runtime R8 storage image and qualified shader format.
				const auto output = fixture.CreateImage(2, 2, VK_FORMAT_R8_UINT, VK_IMAGE_USAGE_STORAGE_BIT);
				// Uploading zeroes establishes GENERAL layout and a recognizable
				// non-coarse initial state; a missing dispatch cannot satisfy the test.
				const std::array<std::uint8_t, 4> zeros{}; fixture.UploadImage(output, Bytes(zeros), VK_IMAGE_LAYOUT_GENERAL);
				ImageBinding(fixture, program.sets[0], 14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, output.view, VK_IMAGE_LAYOUT_GENERAL);
				ImageBinding(fixture, program.sets[0], 15, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				fixture.RequireNoValidationErrors();
				Review::VulkanReviewBuffer readback;
				const auto proof = fixture.Submit([&](VkCommandBuffer command, const auto& native) {
					native.functions->vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, program.pipeline);
					native.functions->vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, program.layout, 0, 1, program.sets.data(), 0, nullptr);
					const std::array<std::uint32_t, 3> push{width, height, tile};
					native.functions->vkCmdPushConstants(command, program.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push.data());
					native.functions->vkCmdDispatch(command, 1, 1, 1); readback = fixture.CopyImageToReadback(command, output, 4);
				});
				context.Expect(proof.completed, "Native VRS classification needs a completed fence."); const auto bytes = fixture.Read(readback);
				std::array<std::uint8_t, 4> rates{}; std::memcpy(rates.data(), bytes.data(), bytes.size());
				std::ofstream raw(directory / ("rates-" + std::to_string(scenario) + ".bin"), std::ios::binary); raw.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
				const auto first = rates[0];
				allMatch &= first == (scenario == 2 ? 0u : 10u);
				if (scenario < 2) for (auto rate : rates) allMatch &= rate == 10u;
				observations.push_back({{"scenario", scenario}, {"rates", rates}});
			}
			std::ofstream(directory / "rates.json") << nlohmann::json{{"scope", "ProductionR8Classifier"}, {"productionAttachmentFormat", "R8_UINT"}, {"emittedStorageImageFormat", "R8ui"}, {"scenarios", observations}}.dump(2); FinishEvidence(fixture, directory / "native.json");
			context.Expect(allMatch, "Flat HDR4 and flat .25 tiles must both choose coarse0xA; varying tiles must stay fine.");
		}
		void Raster(TestContext& context, const std::string& name)
		{
			if (!EnterNative(context, name)) return;
			VulkanReviewFixture fixture(name, NativeOptions()); fixture.EnsureEngineDispatch(); const auto directory = Artifact("raster");
			const auto words = VulkanReviewFixture::CompileShader(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/VirtualGeometry/VirtualGeometryComputeRaster.comp.hlsl", "cs_6_0", "main", directory / "production.spv", {}, true);
			std::vector<VkDescriptorSetLayoutBinding> bindings; for (unsigned binding = 0; binding < 4; ++binding) bindings.push_back({binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
			const auto program = Compute(fixture, words, {{}, bindings}, 24, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
			constexpr unsigned width = 16, height = 16, count = width * height;
			// Wire layout consists of twenty float4/uint4 lanes, as authored by the
			// production RasterTriangle include. Keep integer lanes byte-exact.
			std::array<std::byte, 320> triangle{};
			const std::array<std::array<float, 4>, 3> clips{{{-0.35f, -0.35f, -0.1f, 0.5f}, {1.6f, -1.2f, 1.0f, 2.0f}, {-0.8f, 3.2f, 2.0f, 4.0f}}};
			std::memcpy(triangle.data(), clips.data(), sizeof(clips)); const std::array<std::uint32_t, 4> identity{7, 0, 1, 0x80000000u};
			std::memcpy(triangle.data() + 256, identity.data(), sizeof(identity));
			const auto input = fixture.CreateBuffer(triangle.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, triangle);
			const auto depth = fixture.CreateBuffer(count * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
			const auto visibility = fixture.CreateBuffer(count * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
			const auto resolved = fixture.CreateBuffer(count * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
			const auto bins = fixture.CreateBuffer(count * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
			const std::uint32_t zero = 0; const auto diagnostics = fixture.CreateBuffer(4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Bytes(zero));
			BufferBinding(fixture, program.sets[1], 0, input); BufferBinding(fixture, program.sets[1], 1, depth); BufferBinding(fixture, program.sets[1], 2, visibility); BufferBinding(fixture, program.sets[1], 3, diagnostics);
			fixture.RequireNoValidationErrors();
			using namespace Renderer::Vulkan;
			VulkanVirtualGeometryVisibilityPlan plan; plan.ready = true; plan.boundedPrimitiveCount = 1; plan.pixelCount = count;
			plan.targetWidth = width; plan.targetHeight = height; plan.materialBinCount = 1; plan.frameId = plan.geometryGeneration = 1;
			VulkanVirtualGeometryVisibilityResources resources; resources.inputTriangleBuffer = input.handle; resources.depthBuffer = depth.handle;
			resources.visibilityRecordBuffer = visibility.handle; resources.resolvedVisibilityBuffer = resolved.handle; resources.shadingBinBuffer = bins.handle;
			resources.diagnosticBuffer = diagnostics.handle; resources.bindlessDescriptorSet = program.sets[0]; resources.computeRasterDescriptorSet = program.sets[1]; resources.resourcesOwnedByFrame = true;
			VulkanVirtualGeometryVisibilityPipelines pipelines; pipelines.layout = program.layout; pipelines.computeRaster = program.pipeline;
			VulkanVirtualGeometryExecutorRecordingResult recorded;
			// This explicitly stage-only fixture does not pretend that triangle
			// setup/resolve/composite pipelines have been instantiated or executed.
			const auto proof = fixture.Submit([&](VkCommandBuffer command, const auto&) { recorded = RecordVulkanVirtualGeometryVisibilityStage(command, VulkanVirtualGeometryVisibilityStage::ComputeRaster, plan, resources, pipelines); });
			context.Expect(proof.completed && recorded.recorded && recorded.dispatchCount == 3, "Actual production recorder must run all three raster phases.");
			const auto bytes = fixture.Read(visibility); std::vector<std::array<std::uint32_t, 4>> records(count); std::memcpy(records.data(), bytes.data(), bytes.size());
			{ std::ofstream raw(directory / "visibility.bin", std::ios::binary); raw.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }
			{ std::ofstream inputs(directory / "input.json"); inputs << nlohmann::json{{"clips", clips}, {"width", width}, {"height", height}, {"triangleIdentity", identity}}.dump(2); }
			nlohmann::json oracleRows = nlohmann::json::array();
			unsigned compared = 0; double worst = 0;
			// Independent analytic perspective interpolation on the ORIGINAL
			// projected triangle avoids duplicating homogeneous clipping code.
			std::array<std::array<double, 2>, 3> screen;
			for (unsigned i = 0; i < 3; ++i) screen[i] = {(double(clips[i][0]) / clips[i][3] * .5 + .5) * width, (double(clips[i][1]) / clips[i][3] * .5 + .5) * height};
			const auto cross = [](auto a, auto b, auto p) { return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]); };
			const double area = cross(screen[0], screen[1], screen[2]);
			for (unsigned pixel = 0; pixel < count; ++pixel)
			{
				if (records[pixel][0] != 7) continue;
				const std::array<double, 2> point{double(pixel % width) + .5, double(pixel / width) + .5};
				const std::array<double, 3> lambda{cross(screen[1], screen[2], point) / area, cross(screen[2], screen[0], point) / area, cross(screen[0], screen[1], point) / area};
				if (*std::min_element(lambda.begin(), lambda.end()) < .03) continue;
				std::array<double, 3> expected{}, actual{}; double expectedSum = 0, actualSum = 0;
				const double x = double(records[pixel][1] & 65535u) / 65535.0, y = double(records[pixel][1] >> 16u) / 65535.0;
				const std::array<double, 3> encoded{x, y, (std::max)(0.0, 1.0 - x - y)};
				for (unsigned i = 0; i < 3; ++i) { expected[i] = lambda[i] / clips[i][3]; actual[i] = encoded[i]; expectedSum += expected[i]; actualSum += actual[i]; }
				for (unsigned i = 0; i < 3; ++i) { expected[i] /= expectedSum; actual[i] /= actualSum; worst = (std::max)(worst, std::abs(expected[i] - actual[i])); }
				oracleRows.push_back({{"pixel", pixel}, {"expectedPerspectiveWeights", expected}, {"actualCompositeWeights", actual}});
				++compared;
			}
			std::ofstream(directory / "comparison.json") << nlohmann::json{{"pixelsCompared", compared}, {"worstPerspectiveWeightError", worst}, {"tolerance", .0005}, {"scope", "ComputeRaster direct perspective weights (V2 transient contract)"}}.dump(2);
			std::ofstream(directory / "oracle-rows.json") << oracleRows.dump(2);
			FinishEvidence(fixture, directory / "native.json");
			context.Expect(compared >= 4 && worst <= .0005, "Near-plane-clipped unequal-w raster weights must match the independent original-triangle perspective oracle.");
		}
		void SignedWReconstruction(TestContext& context, const std::string& name)
		{
			if (!EnterNative(context, name)) return;
			VulkanReviewFixture fixture(name, NativeOptions()); fixture.EnsureEngineDispatch(); const auto directory = Artifact("reconstruction");
			const auto root = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			const auto rasterWords = VulkanReviewFixture::CompileShader(root / "Engine/Shaders/VirtualGeometry/VirtualGeometryComputeRaster.comp.hlsl", "cs_6_0", "main", directory / "raster.spv", {}, true);
			const auto helperWords = VulkanReviewFixture::CompileShader(root / "Engine/Shaders/Tests/ReviewRemediation/VirtualGeometryReconstruction.comp.hlsl", "cs_6_0", "main", directory / "reconstruction.spv", {}, true);
			const auto resolveWords = VulkanReviewFixture::CompileShader(root / "Engine/Shaders/VirtualGeometry/VirtualGeometryVisibilityResolve.comp.hlsl", "cs_6_0", "main", directory / "resolve.spv", {}, true);
			std::vector<VkDescriptorSetLayoutBinding> bindings;
			for (unsigned index = 0; index < 4; ++index) bindings.push_back({index, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
			const auto raster = Compute(fixture, rasterWords, {{}, bindings}, 24, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
			const auto resolve = Compute(fixture, resolveWords, {{}, bindings}, 24, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
			const auto helper = Compute(fixture, helperWords, {bindings}, 4);
			constexpr unsigned width = 16, height = 16, count = width * height;
			using Clips = std::array<std::array<float, 4>, 3>;
			const std::array<Clips, 4> scenarios{{
				Clips{{{-.35f, -.35f, -.1f, .5f}, {1.6f, -1.2f, 1.f, 2.f}, {-.8f, 3.2f, 2.f, 4.f}}},
				Clips{{{-.2f, -.2f, -.2f, -.5f}, {1.6f, -1.2f, 1.f, 2.f}, {-.8f, 3.2f, 2.f, 4.f}}},
				Clips{{{-.35f, -.35f, -.1f, 1.e-7f}, {1.6f, -1.2f, 1.f, 2.f}, {-.8f, 3.2f, 2.f, 4.f}}},
				Clips{{{-.5f, -.5f, .5f, 1.f}, {0.f, 0.f, .5f, 1.f}, {.5f, .5f, .5f, 1.f}}}
			}};
			bool allMatch = true; nlohmann::json summaries = nlohmann::json::array();
			for (unsigned scenario = 0; scenario < scenarios.size(); ++scenario)
			{
				const auto& clips = scenarios[scenario]; std::array<std::byte, 320> triangle{};
				std::memcpy(triangle.data(), clips.data(), sizeof(clips));
				const std::array<std::uint32_t, 4> identity{0, 0, 1, 0x80000000u}; std::memcpy(triangle.data() + 256, identity.data(), sizeof(identity));
				const auto input = fixture.CreateBuffer(triangle.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, triangle);
				const auto depth = fixture.CreateBuffer(count * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
				const auto visibility = fixture.CreateBuffer(count * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
				const auto resolved = fixture.CreateBuffer(count * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
				const auto surface = fixture.CreateBuffer(count * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
				const auto valid = fixture.CreateBuffer(count * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
				const auto unused = fixture.CreateBuffer(count * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
				const std::uint32_t zero = 0; const auto diagnostics = fixture.CreateBuffer(4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Bytes(zero));
				BufferBinding(fixture, raster.sets[1], 0, input); BufferBinding(fixture, raster.sets[1], 1, depth); BufferBinding(fixture, raster.sets[1], 2, visibility); BufferBinding(fixture, raster.sets[1], 3, diagnostics);
				BufferBinding(fixture, resolve.sets[1], 0, visibility); BufferBinding(fixture, resolve.sets[1], 1, resolved); BufferBinding(fixture, resolve.sets[1], 2, diagnostics); BufferBinding(fixture, resolve.sets[1], 3, input);
				BufferBinding(fixture, helper.sets[0], 0, input); BufferBinding(fixture, helper.sets[0], 1, resolved); BufferBinding(fixture, helper.sets[0], 2, surface); BufferBinding(fixture, helper.sets[0], 3, valid);
				using namespace Renderer::Vulkan;
				VulkanVirtualGeometryVisibilityPlan plan; plan.ready = true; plan.boundedPrimitiveCount = 1; plan.pixelCount = count;
				plan.targetWidth = width; plan.targetHeight = height; plan.materialBinCount = 1; plan.frameId = plan.geometryGeneration = 1;
				VulkanVirtualGeometryVisibilityResources resources; resources.inputTriangleBuffer = input.handle; resources.depthBuffer = depth.handle;
				resources.visibilityRecordBuffer = visibility.handle; resources.resolvedVisibilityBuffer = resolved.handle; resources.shadingBinBuffer = unused.handle;
				resources.diagnosticBuffer = diagnostics.handle; resources.bindlessDescriptorSet = raster.sets[0]; resources.computeRasterDescriptorSet = raster.sets[1]; resources.resourcesOwnedByFrame = true;
				resources.visibilityResolveDescriptorSet = resolve.sets[1];
				VulkanVirtualGeometryVisibilityPipelines pipelines; pipelines.computeRaster = raster.pipeline; pipelines.visibilityResolve = resolve.pipeline; pipelines.layout = raster.layout;
				fixture.RequireNoValidationErrors();
				VulkanVirtualGeometryExecutorRecordingResult recorded, resolvedRecording;
				const auto proof = fixture.Submit([&](VkCommandBuffer command, const auto& native) {
					recorded = RecordVulkanVirtualGeometryVisibilityStage(command, VulkanVirtualGeometryVisibilityStage::ComputeRaster, plan, resources, pipelines);
					VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
					native.functions->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
					resolvedRecording = RecordVulkanVirtualGeometryVisibilityStage(command, VulkanVirtualGeometryVisibilityStage::VisibilityResolve, plan, resources, pipelines);
					native.functions->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
					native.functions->vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, helper.pipeline);
					native.functions->vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, helper.layout, 0, 1, helper.sets.data(), 0, nullptr);
					native.functions->vkCmdPushConstants(command, helper.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &count);
					native.functions->vkCmdDispatch(command, (count + 63) / 64, 1, 1);
				});
				context.Expect(proof.completed && recorded.recorded && recorded.dispatchCount == 3 && resolvedRecording.recorded && resolvedRecording.dispatchCount == 1,
					"Signed-W proof requires actual production raster phases, actual resolve, and completed reconstruction dispatch.");
				const auto visibilityBytes = fixture.Read(visibility), resolvedBytes = fixture.Read(resolved), surfaceBytes = fixture.Read(surface), validityBytes = fixture.Read(valid), depthBytes = fixture.Read(depth);
				std::vector<std::array<std::uint32_t, 4>> records(count); std::memcpy(records.data(), visibilityBytes.data(), visibilityBytes.size());
				std::vector<std::array<float, 4>> surfaces(count); std::memcpy(surfaces.data(), surfaceBytes.data(), surfaceBytes.size());
				std::vector<std::uint32_t> validity(count), depths(count); std::memcpy(validity.data(), validityBytes.data(), validityBytes.size()); std::memcpy(depths.data(), depthBytes.data(), depthBytes.size());
				const auto prefix = std::to_string(scenario); nlohmann::json rows = nlohmann::json::array(); unsigned covered = 0, expectedInterior = 0, missed = 0, invalid = 0; double worstWeight = 0, worstDepth = 0, worstRasterDepth = 0;
				for (unsigned pixel = 0; pixel < count; ++pixel)
				{
					// Independent homogeneous nullspace solve uses original clip values;
					// no clipping implementation or original-W division is duplicated.
					const double nx = (double(pixel % width) + .5) * 2 / width - 1, ny = (double(pixel / width) + .5) * 2 / height - 1;
					std::array<double, 3> ax{}, ay{}, expected{};
					for (unsigned i = 0; i < 3; ++i) { ax[i] = clips[i][0] - nx * clips[i][3]; ay[i] = clips[i][1] - ny * clips[i][3]; }
					expected = {ax[1]*ay[2]-ax[2]*ay[1], ax[2]*ay[0]-ax[0]*ay[2], ax[0]*ay[1]-ax[1]*ay[0]};
					const double sum = expected[0]+expected[1]+expected[2];
					if (std::abs(sum) < 1.e-15) { allMatch &= records[pixel][0] == UINT32_MAX; continue; }
					double pointW = 0, pointZ = 0;
					for (unsigned i = 0; i < 3; ++i) { expected[i] /= sum; pointW += expected[i] * clips[i][3]; pointZ += expected[i] * clips[i][2]; }
					const bool interior = *std::min_element(expected.begin(), expected.end()) > .02 && pointW > 1.e-6 && pointZ > .001 && pointZ < pointW-.001;
					if (interior) { ++expectedInterior; if (records[pixel][0] != 0) ++missed; }
					if (records[pixel][0] != 0) continue;
					++covered; if (validity[pixel] != 1 || !std::all_of(surfaces[pixel].begin(), surfaces[pixel].end(), [](float value) { return std::isfinite(value); })) { ++invalid; continue; }
					if (pointW <= 0) { ++invalid; continue; }
					const double expectedDepth = pointZ / pointW, rasterDepth = double(depths[pixel]) / double(UINT32_MAX);
					for (unsigned i = 0; i < 3; ++i) worstWeight = (std::max)(worstWeight, std::abs(expected[i] - surfaces[pixel][i]));
					worstDepth = (std::max)(worstDepth, std::abs(expectedDepth - surfaces[pixel][3])); worstRasterDepth = (std::max)(worstRasterDepth, std::abs(expectedDepth-rasterDepth));
					rows.push_back({{"pixel", pixel}, {"expectedWeights", expected}, {"gpuWeightsDepth", surfaces[pixel]}, {"expectedDepth", expectedDepth}, {"rasterDepth", rasterDepth}});
				}
				const bool matched = scenario == 3 ? covered == 0 : covered >= 4 && expectedInterior >= 4 && !missed && !invalid && worstWeight <= .0005 && worstDepth <= .0005 && worstRasterDepth <= .0005;
				allMatch &= matched; summaries.push_back({{"scenario",scenario}, {"covered",covered}, {"expectedInterior",expectedInterior}, {"missedInterior",missed}, {"invalidReconstruction",invalid}, {"worstWeightError",worstWeight}, {"worstHelperDepthError",worstDepth}, {"worstRasterDepthError",worstRasterDepth}, {"matched",matched}});
				std::ofstream(directory/(prefix+"-input.json")) << nlohmann::json{{"clips",clips},{"width",width},{"height",height}}.dump(2);
				std::ofstream(directory/(prefix+"-oracle.json")) << rows.dump(2);
				for (const auto& [label, bytes] : std::array<std::pair<const char*, std::span<const std::byte>>, 5>{{{"visibility",visibilityBytes},{"resolved",resolvedBytes},{"surface",surfaceBytes},{"validity",validityBytes},{"depth",depthBytes}}})
				{ std::ofstream file(directory/(prefix+"-"+label+".bin"), std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }
			}
			// Synthetic protocol negatives execute the real resolve shader. They
			// qualify same-generation version rejection and malformed payloads,
			// independently of the measured geometry scenarios above.
			constexpr std::uint32_t token = 0x56320000u, protocolCount = 4;
			const std::array<std::array<std::uint32_t, 4>, protocolCount> protocol{{
				{0, 0x40004000u, 0x80000000u, 1u ^ token},
				{0, 0x40004000u, 0x80000000u, 1u},
				{0, UINT32_MAX, 0x80000000u, 1u ^ token},
				{0, 0x40004000u, 0x80000000u, 2u ^ token}}};
			std::array<std::byte, 320> triangle{}; const std::array<std::uint32_t, 4> identity{0, 0, 1, 0}; std::memcpy(triangle.data()+256, identity.data(), sizeof(identity));
			const auto source = fixture.CreateBuffer(triangle.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, triangle);
			const auto raw = fixture.CreateBuffer(sizeof(protocol), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Bytes(protocol));
			const auto output = fixture.CreateBuffer(sizeof(protocol), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
			const std::uint32_t zero = 0; const auto diagnostics = fixture.CreateBuffer(4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Bytes(zero));
			BufferBinding(fixture, resolve.sets[1], 0, raw); BufferBinding(fixture, resolve.sets[1], 1, output); BufferBinding(fixture, resolve.sets[1], 2, diagnostics); BufferBinding(fixture, resolve.sets[1], 3, source);
			using namespace Renderer::Vulkan;
			VulkanVirtualGeometryVisibilityPlan protocolPlan; protocolPlan.ready = true; protocolPlan.pixelCount = protocolCount; protocolPlan.boundedPrimitiveCount = 1; protocolPlan.geometryGeneration = protocolPlan.frameId = 1;
			VulkanVirtualGeometryVisibilityResources protocolResources; protocolResources.bindlessDescriptorSet = resolve.sets[0]; protocolResources.visibilityResolveDescriptorSet = resolve.sets[1];
			VulkanVirtualGeometryVisibilityPipelines protocolPipelines; protocolPipelines.layout = resolve.layout; protocolPipelines.visibilityResolve = resolve.pipeline;
			fixture.RequireNoValidationErrors(); VulkanVirtualGeometryExecutorRecordingResult protocolRecorded;
			const auto protocolProof = fixture.Submit([&](VkCommandBuffer command, const auto&) { protocolRecorded = RecordVulkanVirtualGeometryVisibilityStage(command, VulkanVirtualGeometryVisibilityStage::VisibilityResolve, protocolPlan, protocolResources, protocolPipelines); });
			context.Expect(protocolProof.completed && protocolRecorded.recorded, "Actual resolve protocol rejection requires completed native work.");
			const auto protocolBytes = fixture.Read(output), diagnosticBytes = fixture.Read(diagnostics);
			std::array<std::array<std::uint32_t, 4>, protocolCount> protocolOutput{}; std::memcpy(protocolOutput.data(), protocolBytes.data(), protocolBytes.size());
			std::uint32_t diagnostic = 0; std::memcpy(&diagnostic, diagnosticBytes.data(), sizeof(diagnostic));
			const bool protocolMatches = protocolOutput[0][0] == 0 && (protocolOutput[0][3] & 0xffff0000u) == token
				&& protocolOutput[1][0] == UINT32_MAX && protocolOutput[2][0] == UINT32_MAX && protocolOutput[3][0] == UINT32_MAX
				&& (diagnostic & 1u) && (diagnostic & 64u);
			allMatch &= protocolMatches;
			std::ofstream(directory/"protocol.json") << nlohmann::json{{"scope","SyntheticProtocolNegatives_ActualResolveExecution"},{"input",protocol},{"output",protocolOutput},{"diagnostics",diagnostic},{"matched",protocolMatches}}.dump(2);
			std::ofstream(directory/"comparison.json") << nlohmann::json{{"scope","ActualRasterResolveAndSharedProductionReconstruction_NotFullMaterialCompositeDraw"},{"tolerance",.0005},{"scenarios",summaries},{"protocolMatched",protocolMatches}}.dump(2);
			FinishEvidence(fixture, directory/"native.json");
			context.Expect(allMatch, "Positive, negative, nearzero original W and degenerate controls must retain coverage and bounded GPU source-weight/depth accuracy.");
		}
		void Ui(TestContext& context, const std::string& name)
		{
			if (!EnterNative(context, name)) return;
			auto options = NativeOptions(); options.requireRuntimeDescriptorArrays = options.requireSampledImageArrayDynamicIndexing = true;
			VulkanReviewFixture fixture(name, options); const auto& n = fixture.GetNative(); const auto directory = Artifact("ui");
			const auto source = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/UI/UI_Static.hlsl";
			const auto vertex = VulkanReviewFixture::CompileShader(source, "vs_6_0", "VSMain", directory / "production.vert.spv", {}, true);
			const auto fragment = VulkanReviewFixture::CompileShader(source, "ps_6_0", "PSMain", directory / "production.frag.spv", {}, true);
			// Inspect actual emitted SPIR-V, not the authored binding strings.
			// OpTypeImage(Sampled=1) and OpTypeSampler may share one COMBINED
			// descriptor under the Vulkan Shader Resource Interface contract.
			std::map<unsigned, unsigned> sets, bindings, pointees, opcodes;
			std::vector<std::pair<unsigned, unsigned>> variables;
			for (std::size_t word = 5; word < fragment.size();)
			{
				const auto size = fragment[word] >> 16u, opcode = fragment[word] & 65535u;
				context.Expect(size && word + size <= fragment.size(), "Actual SPIR-V instructions must be bounded.");
				const auto* operands = fragment.data() + word + 1;
				if (opcode == 71 && size >= 4) { if (operands[1] == 33) bindings[operands[0]] = operands[2]; if (operands[1] == 34) sets[operands[0]] = operands[2]; }
				if (opcode == 32) pointees[operands[0]] = operands[2];
				if (opcode == 28 || opcode == 29) pointees[operands[0]] = operands[1];
				if (opcode == 25 || opcode == 26 || opcode == 27) opcodes[operands[0]] = opcode;
				if (opcode == 59) variables.emplace_back(operands[1], operands[0]);
				word += size;
			}
			bool image = false, sampler = false;
			for (auto [id, type] : variables)
			{
				if (!bindings.contains(id)) continue;
				context.Expect(bindings[id] == 0 && sets[id] == 0, "Current UI resources must match actual global combined binding0.");
				while (pointees.contains(type)) type = pointees[type];
				image |= opcodes[type] == 25; sampler |= opcodes[type] == 26;
			}
			context.Expect(image && sampler, "The qualification must exercise both emitted image and sampler variables.");
			const auto program = Layout(fixture, {{{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}}}, 16, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
			const auto texture = fixture.CreateImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
			const std::array<std::uint8_t, 4> known{51, 102, 153, 255}; fixture.UploadImage(texture, Bytes(known));
			VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST;
			samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			VkSampler nativeSampler{}; VkCheck(n.functions->vkCreateSampler(n.device, &samplerInfo, nullptr, &nativeSampler));
			fixture.RegisterCleanup([nativeSampler](const auto& native) { native.functions->vkDestroySampler(native.device, nativeSampler, nullptr); });
			ImageBinding(fixture, program.sets[0], 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, nativeSampler);
			const auto target = fixture.CreateImage(4, 4, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
			VkAttachmentDescription attachment{}; attachment.format = target.format; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
			attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
			VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
			VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color;
			VkRenderPassCreateInfo renderInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; renderInfo.attachmentCount = 1; renderInfo.pAttachments = &attachment; renderInfo.subpassCount = 1; renderInfo.pSubpasses = &subpass;
			VkRenderPass renderPass{}; VkCheck(n.functions->vkCreateRenderPass(n.device, &renderInfo, nullptr, &renderPass));
			fixture.RegisterCleanup([renderPass](const auto& native) { native.functions->vkDestroyRenderPass(native.device, renderPass, nullptr); });
			VkFramebufferCreateInfo frameInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; frameInfo.renderPass = renderPass; frameInfo.attachmentCount = 1; frameInfo.pAttachments = &target.view; frameInfo.width = frameInfo.height = 4; frameInfo.layers = 1;
			VkFramebuffer framebuffer{}; VkCheck(n.functions->vkCreateFramebuffer(n.device, &frameInfo, nullptr, &framebuffer));
			fixture.RegisterCleanup([framebuffer](const auto& native) { native.functions->vkDestroyFramebuffer(native.device, framebuffer, nullptr); });
			const auto vs = Module(fixture, vertex), ps = Module(fixture, fragment);
			std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
			stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "VSMain";
			stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = ps; stages[1].pName = "PSMain";
			const VkVertexInputBindingDescription vertexBinding{0, 32, VK_VERTEX_INPUT_RATE_VERTEX};
			const std::array<VkVertexInputAttributeDescription, 3> attributes{{{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 8}, {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16}}};
			VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO}; vertexInput.vertexBindingDescriptionCount = 1; vertexInput.pVertexBindingDescriptions = &vertexBinding; vertexInput.vertexAttributeDescriptionCount = 3; vertexInput.pVertexAttributeDescriptions = attributes.data();
			VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			VkViewport viewport{0, 0, 4, 4, 0, 1}; VkRect2D scissor{{0, 0}, {4, 4}};
			VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewportState.viewportCount = viewportState.scissorCount = 1; viewportState.pViewports = &viewport; viewportState.pScissors = &scissor;
			VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.lineWidth = 1;
			VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
			VkPipelineColorBlendAttachmentState blendAttachment{}; blendAttachment.colorWriteMask = 15;
			VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = 1; blend.pAttachments = &blendAttachment;
			VkGraphicsPipelineCreateInfo graphics{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; graphics.stageCount = 2; graphics.pStages = stages.data(); graphics.pVertexInputState = &vertexInput; graphics.pInputAssemblyState = &assembly; graphics.pViewportState = &viewportState; graphics.pRasterizationState = &raster; graphics.pMultisampleState = &samples; graphics.pColorBlendState = &blend; graphics.layout = program.layout; graphics.renderPass = renderPass;
			VkPipeline pipeline{}; VkCheck(n.functions->vkCreateGraphicsPipelines(n.device, VK_NULL_HANDLE, 1, &graphics, nullptr, &pipeline));
			fixture.RegisterCleanup([pipeline](const auto& native) { native.functions->vkDestroyPipeline(native.device, pipeline, nullptr); });
			const std::array<std::array<float, 8>, 3> vertices{{{0, 0, .5f, .5f, 1, 1, 1, 1}, {8, 0, .5f, .5f, 1, 1, 1, 1}, {0, 8, .5f, .5f, 1, 1, 1, 1}}};
			const auto vertexBuffer = fixture.CreateBuffer(sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, Bytes(vertices));
			Review::VulkanReviewBuffer readback;
			const auto proof = fixture.Submit([&](VkCommandBuffer command, const auto& native) {
				VkClearValue clear{}; VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; begin.renderPass = renderPass; begin.framebuffer = framebuffer; begin.renderArea = scissor; begin.clearValueCount = 1; begin.pClearValues = &clear;
				native.functions->vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE); native.functions->vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
				native.functions->vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, program.layout, 0, 1, program.sets.data(), 0, nullptr);
				const VkDeviceSize offset = 0; native.functions->vkCmdBindVertexBuffers(command, 0, 1, &vertexBuffer.handle, &offset);
				struct Push { float x, y; std::uint32_t texture, use; } push{4, 4, 0, 1};
				native.functions->vkCmdPushConstants(command, program.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
				native.functions->vkCmdDraw(command, 3, 1, 0, 0); native.functions->vkCmdEndRenderPass(command);
				readback = fixture.CopyImageToReadback(command, target, 4 * 4 * 4);
			});
			context.Expect(proof.completed, "Current UI shader sampling needs real fenced graphics completion."); const auto pixels = fixture.Read(readback);
			std::ofstream output(directory / "sample.rgba", std::ios::binary); output.write(reinterpret_cast<const char*>(pixels.data()), pixels.size()); output.close();
			FinishEvidence(fixture, directory / "native.json");
			for (std::size_t index = 0; index < pixels.size(); ++index)
				context.Expect(std::to_integer<unsigned>(pixels[index]) == known[index % 4], "Actual combined descriptor must sample the known texture into every UI pixel.");
		}
	}
	void RegisterShaderViewContractTests(TestRegistry& registry)
	{
		if (!VulkanReviewFixture::IsNativeReviewEnabled()) return;
		registry.Add("Native.ReviewSimulation.ShaderView.HdrVrsClassifier", [](TestContext& context) { Vrs(context, "Native.ReviewSimulation.ShaderView.HdrVrsClassifier"); });
		registry.Add("Native.ReviewSimulation.ShaderView.ClippedPerspectiveRaster", [](TestContext& context) { Raster(context, "Native.ReviewSimulation.ShaderView.ClippedPerspectiveRaster"); });
		registry.Add("Native.ReviewSimulation.ShaderView.SignedWReconstructionAndDepth", [](TestContext& context) { SignedWReconstruction(context, "Native.ReviewSimulation.ShaderView.SignedWReconstructionAndDepth"); });
		registry.Add("Native.ReviewSimulation.ShaderView.CurrentUiCombinedDescriptor", [](TestContext& context) { Ui(context, "Native.ReviewSimulation.ShaderView.CurrentUiCombinedDescriptor"); });
	}
}
