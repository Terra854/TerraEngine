// Maintained production shaders, native buffers/fences and independent oracles.
// CPU hierarchy qualification is separately named and never claims GPU proof.
#include "TestHarness.h"
#include <stdexcept>
#include <string_view>
#include <chrono>
#include <atomic>
#include "Renderer/Deformation/GpuDeformationPlan.h"
#include "Renderer/VirtualGeometry/Selection/VirtualGeometrySelection.h"
#include "Tests/Support/ReviewRemediation/VulkanReviewFixture.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
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
		using Review::VulkanReviewBuffer;
		using namespace Renderer;
		template<typename T> std::vector<std::byte> Bytes(const T& value) {
			const auto bytes = std::as_bytes(std::span(&value, 1U)); return {bytes.begin(), bytes.end()};
		}
		template<typename T> void Put(std::vector<std::byte>& bytes, std::size_t offset, const T& value) {
			if(offset + sizeof(value) > bytes.size()) bytes.resize(offset + sizeof(value));
			std::memcpy(bytes.data() + offset, &value, sizeof(value));
		}
		void Persist(const std::filesystem::path& path, std::span<const std::byte> bytes) {
			std::filesystem::create_directories(path.parent_path()); std::ofstream file(path, std::ios::binary);
			file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			if(!file) throw TestFailure("Cannot persist native deformation/traversal evidence.");
		}
		void Check(VkResult result) { if(result != VK_SUCCESS) throw TestFailure("Native review pipeline prerequisite failed: " + std::to_string(result)); }
		void Finish(VulkanReviewFixture& fixture, const std::filesystem::path& directory) {
			try { fixture.Finish(); } catch(...) { fixture.WriteEvidence(directory / "native-final.json"); throw; }
			fixture.WriteEvidence(directory / "native-final.json");
		}
		VulkanReviewFixture::Options Options() {
			return VulkanReviewFixture::NativeDeviceOptions();
		}
		void Watched(TestContext& context, const std::string& name, const std::function<void()>& body) {
			if(VulkanReviewFixture::IsWatchedChild(name)) { body(); return; }
			const auto child = VulkanReviewFixture::RunWatchedSelf(name);
			context.Expect(child.started && !child.timedOut && child.exitCode == 0, "Native child must finish ordinarily, with actual oracle success. Output=" + child.output);
		}
		struct Pipeline {
			std::vector<VkDescriptorSetLayout> sets;
			VkPipelineLayout layout = VK_NULL_HANDLE; VkShaderModule shader = VK_NULL_HANDLE;
			VkPipeline pipeline = VK_NULL_HANDLE; VkDescriptorPool pool = VK_NULL_HANDLE;
			VkDescriptorSet descriptor = VK_NULL_HANDLE;
		};
		std::shared_ptr<Pipeline> MakePipeline(VulkanReviewFixture& fixture, const std::filesystem::path& source,
			const std::filesystem::path& evidence, std::uint32_t setIndex, std::span<const VulkanReviewBuffer> buffers, bool push) {
			// The shared compiler supplies the safely quoted engine shader include
			// root; additional arguments are reserved for individual safe tokens.
			const auto words = VulkanReviewFixture::CompileShader(source, "cs_6_0", "main", evidence);
			fixture.RequireShaderCapabilities(words);
			auto result = std::make_shared<Pipeline>(); result->sets.resize(setIndex + 1U);
			fixture.RegisterCleanup([result](const auto& native) {
				const auto& vk = *native.functions;
				if(result->pipeline) vk.vkDestroyPipeline(native.device, result->pipeline, nullptr);
				if(result->pool) vk.vkDestroyDescriptorPool(native.device, result->pool, nullptr);
				if(result->layout) vk.vkDestroyPipelineLayout(native.device, result->layout, nullptr);
				for(const auto set : result->sets) if(set) vk.vkDestroyDescriptorSetLayout(native.device, set, nullptr);
				if(result->shader) vk.vkDestroyShaderModule(native.device, result->shader, nullptr);
			});
			const auto& native = fixture.GetNative(); const auto& vk = *native.functions;
			std::vector<VkDescriptorSetLayoutBinding> bindings(buffers.size());
			for(std::uint32_t index = 0U; index < bindings.size(); ++index) bindings[index] = {index, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
			for(std::uint32_t index = 0U; index <= setIndex; ++index) {
				VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
				if(index == setIndex) { info.bindingCount = static_cast<std::uint32_t>(bindings.size()); info.pBindings = bindings.data(); }
				Check(vk.vkCreateDescriptorSetLayout(native.device, &info, nullptr, &result->sets[index]));
			}
			VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0U, 16U};
			VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount = static_cast<std::uint32_t>(result->sets.size()); layout.pSetLayouts = result->sets.data();
			if(push) { layout.pushConstantRangeCount = 1U; layout.pPushConstantRanges = &range; }
			Check(vk.vkCreatePipelineLayout(native.device, &layout, nullptr, &result->layout));
			VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; module.codeSize = words.size() * 4U; module.pCode = words.data(); Check(vk.vkCreateShaderModule(native.device, &module, nullptr, &result->shader));
			VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pipeline.layout = result->layout;
			pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipeline.stage.module = result->shader; pipeline.stage.pName = "main";
			Check(vk.vkCreateComputePipelines(native.device, VK_NULL_HANDLE, 1U, &pipeline, nullptr, &result->pipeline));
			VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<std::uint32_t>(buffers.size())};
			VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets = 1U; pool.poolSizeCount = 1U; pool.pPoolSizes = &poolSize; Check(vk.vkCreateDescriptorPool(native.device, &pool, nullptr, &result->pool));
			VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocation.descriptorPool = result->pool; allocation.descriptorSetCount = 1U; allocation.pSetLayouts = &result->sets[setIndex]; Check(vk.vkAllocateDescriptorSets(native.device, &allocation, &result->descriptor));
			std::vector<VkDescriptorBufferInfo> infos(buffers.size()); std::vector<VkWriteDescriptorSet> writes(buffers.size());
			for(std::uint32_t index = 0U; index < buffers.size(); ++index) {
				infos[index] = {buffers[index].handle, 0U, buffers[index].bytes}; writes[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
				writes[index].dstSet = result->descriptor; writes[index].dstBinding = index; writes[index].descriptorCount = 1U;
				writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[index].pBufferInfo = &infos[index];
			}
			vk.vkUpdateDescriptorSets(native.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0U, nullptr);
			fixture.RequireNoValidationErrors(); return result;
		}
		void Dispatch(const VulkanReviewFixture::Native& native, VkCommandBuffer command, const Pipeline& pipeline,
			std::uint32_t set, const std::array<std::uint32_t, 4U>* push) {
			const auto& vk = *native.functions; vk.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
			vk.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout, set, 1U, &pipeline.descriptor, 0U, nullptr);
			if(push) vk.vkCmdPushConstants(command, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0U, 16U, push->data());
			vk.vkCmdDispatch(command, 1U, 1U, 1U);
		}
		void Barrier(const VulkanReviewFixture::Native& native, VkCommandBuffer command, VkPipelineStageFlags source,
			VkPipelineStageFlags target, VkAccessFlags sourceAccess, VkAccessFlags targetAccess) {
			VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask = sourceAccess; barrier.dstAccessMask = targetAccess;
			native.functions->vkCmdPipelineBarrier(command, source, target, 0U, 1U, &barrier, 0U, nullptr, 0U, nullptr);
		}
		enum class DeformMode { Valid, InvalidGeneration, NonfinitePalette };
		void QualifyDeformation(TestContext& context, const std::string& name, DeformMode mode) {
			Watched(context, name, [&] {
				VulkanReviewFixture fixture(name, Options()); const auto root = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
				const auto directory = FreshEvidenceDirectory(root / "out/validation/review-remediation/deformation-failures", mode == DeformMode::Valid ? "valid" : mode == DeformMode::InvalidGeneration ? "invalid-generation" : "nonfinite-palette");
				GpuDeformationGenerationRecord generation{1U, 2U, 3U, 4U, 5U, 6U, 7U, 0U};
				if(mode == DeformMode::InvalidGeneration) generation.stableInstanceId = 0U;
				GpuDeformationShaderWorkRecord work; work.vertexCount = work.jointCount = work.morphTargetCount = 1U;
				work.skinRecordByteOffset = 16U; work.morphDeltaByteOffset = 24U; work.flags = 1U;
				work.positionOffset = {1.0F, 2.0F, 3.0F, 0.0F}; work.positionScale = {2.0F, 3.0F, 4.0F, 0.0F}; work.reserved[0] = 77U;
				// Frozen packed ABI: all position bits zero decode exactly to -1.
				// Skin joint0 has full byte weight255. Full morph tuple is allocated.
				std::vector<std::byte> page(72U); const std::array<std::uint32_t, 2U> skin{0U, 255U}; Put(page, 16U, skin);
				const std::array<float, 4U> delta{2.0F, 4.0F, 6.0F, 0.0F}; Put(page, 24U, delta);
				std::array<std::array<float, 4U>, 3U> palette{{{1.0F, 0.0F, 0.0F, 10.0F}, {0.0F, 1.0F, 0.0F, 20.0F}, {0.0F, 0.0F, 1.0F, 30.0F}}};
				if(mode == DeformMode::NonfinitePalette) palette[0][0] = std::numeric_limits<float>::quiet_NaN();
				GpuDeformedVertexRecord stale; stale.currentPosition = {101.0F, 102.0F, 103.0F, 1.0F}; stale.normal = {0.0F, 0.0F, 1.0F, 0.0F}; stale.tangentAndSign = {1.0F, 0.0F, 0.0F, 1.0F};
				auto history = stale; history.currentPosition = {11.0F, 12.0F, 13.0F, 1.0F};
				GpuDynamicBoundsRecord priorBounds; priorBounds.identity = {77U, 0U, kAnimatedGeometryAbiVersion, 13U};
				const float weight = 0.25F; const std::uint32_t zero = 0U;
				std::array<std::vector<std::byte>, 11U> input{Bytes(generation), Bytes(work), page, Bytes(palette), Bytes(weight), Bytes(stale), Bytes(history), Bytes(priorBounds), Bytes(priorBounds), std::vector<std::byte>(64U), Bytes(zero)};
				std::array<VulkanReviewBuffer, 11U> buffers;
				for(std::size_t index = 0U; index < buffers.size(); ++index) {
					buffers[index] = fixture.CreateBuffer(std::max<std::size_t>(input[index].size(), 64U), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, input[index]);
					Persist(directory / ("input-binding-" + std::to_string(index) + ".bin"), input[index]);
				}
				const auto deform = MakePipeline(fixture, root / "Engine/Shaders/VirtualGeometry/AnimatedPageDeform.comp.hlsl", directory / "deform.spv", 2U, buffers, true);
				const auto bounds = MakePipeline(fixture, root / "Engine/Shaders/VirtualGeometry/AnimatedClusterBounds.comp.hlsl", directory / "bounds.spv", 2U, buffers, true);
				const auto fetchOutput = fixture.CreateBuffer(16U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, std::vector<std::byte>(16U));
				const std::array<VulkanReviewBuffer, 2U> fetchBuffers{buffers[5U], fetchOutput};
				const auto fetch = MakePipeline(fixture, root / "Engine/Shaders/Tests/ReviewRemediation/ActualAnimatedVertexFetch.comp.hlsl", directory / "actual-fetch.spv", 0U, fetchBuffers, false);
				const std::array<std::uint32_t, 4U> push{0U, 1U, kAnimatedGeometryAbiVersion, 77U}; Persist(directory / "input-push.bin", Bytes(push));
				const auto submission = fixture.Submit([&](VkCommandBuffer command, const auto& native) {
					Barrier(native, command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
					Dispatch(native, command, *deform, 2U, &push);
					Barrier(native, command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
					Dispatch(native, command, *bounds, 2U, &push);
					Barrier(native, command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
					Dispatch(native, command, *fetch, 0U, nullptr);
					Barrier(native, command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
				});
				context.Expect(submission.submitted && submission.completed && submission.result == VK_SUCCESS, "The actual three-stage deformation/bounds/fetch chain must complete a real fence.");
				const auto vertexBytes = fixture.Read(buffers[5U], 64U), boundsBytes = fixture.Read(buffers[8U], 64U), diagnostics = fixture.Read(buffers[10U], 4U), fetched = fixture.Read(fetchOutput, 16U);
				Persist(directory / "output-vertex.bin", vertexBytes); Persist(directory / "output-bounds.bin", boundsBytes); Persist(directory / "output-diagnostic.bin", diagnostics); Persist(directory / "output-fetch.bin", fetched);
				GpuDeformedVertexRecord vertex; GpuDynamicBoundsRecord reduced; std::uint32_t diagnostic; std::array<std::uint32_t, 4U> fetchResult;
				std::memcpy(&vertex, vertexBytes.data(), 64U); std::memcpy(&reduced, boundsBytes.data(), 64U); std::memcpy(&diagnostic, diagnostics.data(), 4U); std::memcpy(fetchResult.data(), fetched.data(), 16U);
				// Independent fixed affine oracle: (-1,-1,-1)+(2,4,6)*.25,
				// then identity skin plus translation(10,20,30). No CPU evaluator call.
				const std::array<float, 4U> expected{9.5F, 20.0F, 30.5F, 1.0F}; Persist(directory / "independent-current-position.bin", Bytes(expected));
				Finish(fixture, directory);
				if(mode == DeformMode::Valid) {
					context.Expect(vertex.currentPosition == expected && vertex.previousPosition == history.currentPosition
						&& reduced.minimum[0] == expected[0] && reduced.minimum[1] == expected[1] && reduced.minimum[2] == expected[2]
						&& (reduced.identity[3] & 13U) == 13U && fetchResult[0] == 1U && diagnostic == 0U,
						"Real morph-before-skin deformation, history, exact bounds and production vertex fetch must match the independent oracle.");
				} else {
					const auto expectedDiagnostic = mode == DeformMode::InvalidGeneration ? 1U : 8U;
					context.Expect((diagnostic & expectedDiagnostic) != 0U && !std::isfinite(vertex.currentPosition[0])
						&& (reduced.identity[3] & 13U) == 0U && fetchResult[0] == 0U,
						"Failed current deformation must overwrite finite stale bytes so actual bounds and graphics-fetch consumers reject this generation.");
				}
			});
		}
		void QualifySafeTraversal(TestContext& context, const std::string& name) {
			Watched(context, name, [&] {
				VulkanReviewFixture fixture(name, Options()); const auto root = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
				const auto directory = FreshEvidenceDirectory(root / "out/validation/review-remediation/virtual-geometry-traversal", "invalid-page");
				// One root prefix then a valid chain deeper than the stack capacity.
				// Pending depth remains one; this is safe before any cycle guard exists.
				std::array<std::array<std::uint32_t, 4U>, 41U> records{}; records[0] = {1U, 7U, 0U, 0U};
				for(std::uint32_t index = 1U; index < records.size(); ++index) records[index] = {0U, 7U, index + 1U, index + 1U < records.size() ? 1U : 0U};
				const std::array<std::uint32_t, 4U> push{1U, 4U, 7U, 1U};
				const auto input = fixture.CreateBuffer(sizeof(records), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Bytes(records));
				const auto output = fixture.CreateBuffer(48U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, std::vector<std::byte>(48U));
				const auto counters = fixture.CreateBuffer(8U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, std::vector<std::byte>(8U));
				const std::array<VulkanReviewBuffer, 3U> buffers{input, output, counters};
				const auto pipeline = MakePipeline(fixture, root / "Engine/Shaders/VirtualGeometry/VirtualGeometryTraversal.comp.hlsl", directory / "traversal.spv", 0U, buffers, true);
				std::array<std::array<std::uint32_t, 2U>, 2U> countResults{}; std::array<std::array<std::uint32_t, 3U>, 2U> requestResults{};
				for(std::size_t variant = 0U; variant < 2U; ++variant) {
					if(variant == 1U) records[0][1] = 6U; // Safe stale-generation fail-open.
					fixture.Upload(input, Bytes(records)); fixture.Upload(counters, std::vector<std::byte>(8U));
					Persist(directory / ("input-" + std::to_string(variant) + ".bin"), Bytes(records));
					const auto submission = fixture.Submit([&](VkCommandBuffer command, const auto& native) {
						Barrier(native, command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
						Dispatch(native, command, *pipeline, 0U, &push);
						Barrier(native, command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
					});
					context.Expect(submission.submitted && submission.completed && submission.result == VK_SUCCESS, "Safe traversal controls must complete their real submitted fences.");
					const auto countBytes = fixture.Read(counters, 8U), requestBytes = fixture.Read(output, 12U);
					Persist(directory / ("output-counters-" + std::to_string(variant) + ".bin"), countBytes); Persist(directory / ("output-request-" + std::to_string(variant) + ".bin"), requestBytes);
					std::memcpy(countResults[variant].data(), countBytes.data(), 8U); std::memcpy(requestResults[variant].data(), requestBytes.data(), 12U);
				}
				Persist(directory / "input-push.bin", Bytes(push)); Finish(fixture, directory);
				context.Expect(countResults[0][0] == 1U && countResults[0][1] == 0U && requestResults[0] == std::array<std::uint32_t, 3U>{0U, 40U, 0U}, "A valid deep chain must retain its one terminal leaf without spurious overflow.");
				context.Expect(countResults[1][0] == 1U && requestResults[1] == std::array<std::uint32_t, 3U>{0U, 1U, 2U}, "A stale root must emit actual conservative coverage identifying the original root.");
			});
		}
		// POST-GUARD ONLY: these bounded allocated packets must never execute
		// against the old shader, whose depth-one cycles do not terminate.
		void QualifyGuardedCycle(TestContext& context, const std::string& name, bool ancestor) {
			Watched(context, name, [&] {
				VulkanReviewFixture fixture(name, Options()); const auto root = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
				const auto directory = FreshEvidenceDirectory(root / "out/validation/review-remediation/virtual-geometry-hierarchy", ancestor ? "ancestor-cycle" : "self-cycle");
				std::vector<std::array<std::uint32_t, 4U>> records{{1U,7U,0U,0U}, {0U,7U,1U,1U}};
				if(ancestor) { records[1][2] = 2U; records.push_back({0U,7U,1U,1U}); }
				const std::array<std::uint32_t, 4U> push{1U,4U,7U,1U};
				const auto raw = std::as_bytes(std::span(records));
				const auto input = fixture.CreateBuffer(raw.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, raw);
				const auto output = fixture.CreateBuffer(48U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, std::vector<std::byte>(48U));
				const auto counters = fixture.CreateBuffer(8U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, std::vector<std::byte>(8U));
				const std::array<VulkanReviewBuffer,3U> buffers{input,output,counters};
				const auto pipeline = MakePipeline(fixture, root / "Engine/Shaders/VirtualGeometry/VirtualGeometryTraversal.comp.hlsl", directory / "traversal.spv", 0U, buffers, true);
				Persist(directory / "input.bin",raw); Persist(directory / "input-push.bin",Bytes(push));
				const auto submission = fixture.Submit([&](VkCommandBuffer command, const auto& native) {
					Barrier(native, command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
					Dispatch(native, command, *pipeline, 0U, &push);
					Barrier(native, command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
				});
				context.Expect(submission.submitted && submission.completed && submission.result == VK_SUCCESS, "Guarded cycle must complete its actual submitted fence.");
				const auto countBytes = fixture.Read(counters,8U), requestBytes = fixture.Read(output,12U);
				Persist(directory / "output-counters.bin",countBytes); Persist(directory / "output-request.bin",requestBytes);
				std::array<std::uint32_t,2U> count{}; std::array<std::uint32_t,3U> request{};
				std::memcpy(count.data(),countBytes.data(),8U); std::memcpy(request.data(),requestBytes.data(),12U);
				Finish(fixture,directory);
				context.Expect(count[0] == 1U && (count[1] & 2U) != 0U && request == std::array<std::uint32_t,3U>{0U,1U,2U},
					"Actual bounded traversal must diagnose exhaustion and emit exactly conservative original-root coverage.");
			});
		}

	}
	void RegisterDeformationTraversalFailureTests(TestRegistry& registry) {
		registry.Add("Renderer.Review.Traversal.CpuCycleQualificationIsConservative", [](TestContext& context) {
			const auto result = SelectVirtualGeometryOracle({{1U, 2U, false, 2.0F}, {2U, 1U, false, 2.0F}}, 1.0F, 8U);
			context.Expect(result.failOpen && !result.fallbackReason.empty(), "Existing CPU hierarchy cycle detection remains conservative; this does not execute the GPU traversal or file loader.");
		});
		if(!VulkanReviewFixture::IsNativeReviewEnabled()) return;
		registry.Add("Native.ReviewSimulation.Deformation.ValidActualMorphSkinBoundsAndFetch", [](TestContext& context) { QualifyDeformation(context, "Native.ReviewSimulation.Deformation.ValidActualMorphSkinBoundsAndFetch", DeformMode::Valid); });
		registry.Add("Native.ReviewSimulation.Deformation.InvalidGenerationRejectsStaleConsumers", [](TestContext& context) { QualifyDeformation(context, "Native.ReviewSimulation.Deformation.InvalidGenerationRejectsStaleConsumers", DeformMode::InvalidGeneration); });
		registry.Add("Native.ReviewSimulation.Deformation.NonfiniteOutputRejectsStaleConsumers", [](TestContext& context) { QualifyDeformation(context, "Native.ReviewSimulation.Deformation.NonfiniteOutputRejectsStaleConsumers", DeformMode::NonfinitePalette); });
		registry.Add("Native.ReviewSimulation.Traversal.ValidDeepAndStaleRootBounded", [](TestContext& context) { QualifySafeTraversal(context, "Native.ReviewSimulation.Traversal.ValidDeepAndStaleRootBounded"); });
		// These registrations require the compiled hierarchy-cycle guard.
		registry.Add("Native.ReviewSimulation.Traversal.SelfCycleFailsOpenBoundedly", [](TestContext& context) { QualifyGuardedCycle(context, "Native.ReviewSimulation.Traversal.SelfCycleFailsOpenBoundedly", false); });
		registry.Add("Native.ReviewSimulation.Traversal.AncestorCycleFailsOpenBoundedly", [](TestContext& context) { QualifyGuardedCycle(context, "Native.ReviewSimulation.Traversal.AncestorCycleFailsOpenBoundedly", true); });
	}
}
