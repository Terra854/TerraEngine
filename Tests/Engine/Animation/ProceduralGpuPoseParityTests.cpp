// Production procedural evaluation against independently composed model poses.
// Native shader cases use the shared Vulkan fixture separately; CPU results do
// not qualify execution of AnimationBlend or AnimationSample on a device.
#include "TestHarness.h"
#include "Animation/Graph/ProceduralNodeEvaluator.h"
#include "Animation/Gpu/GpuAnimationPlan.h"
#include "Animation/Codec/AnimationCodec.h"
#include "Tests/Support/ReviewRemediation/VulkanReviewFixture.h"

#include <glm/gtc/quaternion.hpp>
#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <vector>
#include <cmath>

namespace TerraEngine::Tests {
	namespace {
		using namespace Animation;
		glm::dquat Quaternion(const AnimationQuaternion& value) { return {value.w, value.x, value.y, value.z}; }
		AnimationTransform ComposeReference(const AnimationTransform& parent, const AnimationTransform& local) {
			// GLM's Hamilton rotation/composition is independent of ProceduralMath.
			const glm::dquat rotation = Quaternion(parent.rotation) * Quaternion(local.rotation);
			const glm::dvec3 translated = glm::dvec3(parent.translation.x, parent.translation.y, parent.translation.z)
				+ Quaternion(parent.rotation) * (glm::dvec3(parent.scale.x, parent.scale.y, parent.scale.z)
					* glm::dvec3(local.translation.x, local.translation.y, local.translation.z));
			AnimationTransform result;
			result.translation = {static_cast<float>(translated.x), static_cast<float>(translated.y), static_cast<float>(translated.z)};
			result.rotation = {static_cast<float>(rotation.x), static_cast<float>(rotation.y), static_cast<float>(rotation.z), static_cast<float>(rotation.w)};
			result.scale = {parent.scale.x * local.scale.x, parent.scale.y * local.scale.y, parent.scale.z * local.scale.z};
			return result;
		}
		void ExpectTransform(TestContext& context, const AnimationTransform& expected, const AnimationTransform& actual) {
			context.Expect(std::abs(expected.translation.x - actual.translation.x) < 0.0001F
				&& std::abs(expected.translation.y - actual.translation.y) < 0.0001F
				&& std::abs(expected.translation.z - actual.translation.z) < 0.0001F
				&& std::abs(expected.scale.x - actual.scale.x) < 0.0001F
				&& std::abs(expected.scale.y - actual.scale.y) < 0.0001F
				&& std::abs(expected.scale.z - actual.scale.z) < 0.0001F
				&& std::abs(glm::dot(Quaternion(expected.rotation), Quaternion(actual.rotation))) > 0.99999,
				"A model-space output must equal independent parent/target composition in every transform lane.");
		}
		void QualifyCopySpace(TestContext& context, ProceduralCoordinateSpace space) {
			constexpr float quarterTurn = 0.7071067811865475F;
			AnimationTransform parent;
			parent.translation = {10.0F, 2.0F, 3.0F}; parent.rotation = {0.0F, 0.0F, quarterTurn, quarterTurn};
			parent.scale = {2.0F, 3.0F, 4.0F};
			AnimationTransform childLocal; childLocal.translation = {2.0F, 1.0F, -1.0F};
			AnimationTransform target;
			target.translation = {7.0F, 8.0F, 9.0F}; target.rotation = {quarterTurn, 0.0F, 0.0F, quarterTurn};
			target.scale = {0.5F, 2.0F, 1.0F};
			GraphPoseContribution pose;
			pose.compatibilityFingerprint = 77U; pose.jointCount = 2U; pose.jointStableIds = {10U, 20U};
			// The real procedural stage supplies model-space joints, not local poses.
			pose.joints = {parent, ComposeReference(parent, childLocal)};
			ProceduralCopyTransformNodeInput copy;
			copy.jointIndex = 1U; copy.parentJointIndex = 0U; copy.targetTransform = target; copy.targetSpace = space; copy.goalId = 1U;
			ProceduralGraphNodeInput node; node.stableId = 1U; node.payload = copy;
			ProceduralGenerationStamp generation;
			generation.animator = {21U, 2U}; generation.componentGeneration = 3U; generation.skeletonGeneration = 4U;
			generation.programGeneration = 5U; generation.poseGeneration = 6U; generation.publicationGeneration = 7U;
			generation.queryGeneration = 8U; generation.tick = 60;
			ProceduralGraphEvaluationRequest request;
			request.generation = generation; request.expectedGeneration = generation; request.inputPose = &pose; request.nodes = std::span(&node, 1U);
			ProceduralGraphEvaluationResult output;
			context.Expect(EvaluateProceduralGraphNodes(request, output) == AnimationStatus::Ok && output.pose.joints.size() == 2U,
				"The real graph copy-transform node must accept the complete valid model-space fixture.");
			const bool parentRelative = space == ProceduralCoordinateSpace::Parent || space == ProceduralCoordinateSpace::Local;
			ExpectTransform(context, parentRelative ? ComposeReference(parent, target) : target, output.pose.joints[1U]);
			ExpectTransform(context, parent, output.pose.joints[0U]);
		}
		using Review::VulkanReviewFixture;
		template<typename Value> std::vector<std::byte> ObjectBytes(const Value& value) {
			const auto bytes = std::as_bytes(std::span(&value, 1U)); return {bytes.begin(), bytes.end()};
		}
		template<typename Value> void WriteBytes(std::vector<std::byte>& output, std::size_t offset, const Value& value) {
			if(offset + sizeof(Value) > output.size()) output.resize(offset + sizeof(Value));
			std::memcpy(output.data() + offset, &value, sizeof(Value));
		}
		void CheckVk(VkResult result, const char* operation) {
			if(result != VK_SUCCESS) throw TestFailure(std::string(operation) + " failed: " + std::to_string(result));
		}
		GpuAnimationPoseRecord PoseRecord(glm::dquat rotation, AnimationFloat3 translation = {}) {
			GpuAnimationPoseRecord pose;
			pose.translation = {translation.x, translation.y, translation.z, 0.0F};
			pose.rotation = {static_cast<float>(rotation.x), static_cast<float>(rotation.y), static_cast<float>(rotation.z), static_cast<float>(rotation.w)};
			pose.scale = {1.0F, 1.0F, 1.0F, 0.0F}; return pose;
		}
		struct AnimationStageReadback { GpuAnimationPoseRecord pose; GpuAnimationDiagnosticRecord diagnostic; };
		// Persist actual bytes before any expected oracle failure, so a red case
		// still identifies its complete bounded inputs and fenced output.
		void PersistBytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
			std::filesystem::create_directories(path.parent_path());
			std::ofstream file(path, std::ios::binary);
			file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			if(!file) throw TestFailure("Could not persist native animation bytes: " + path.string());
		}
		void FinishEvidence(VulkanReviewFixture& fixture, const std::filesystem::path& directory) {
			// Both dispatches are fenced and read back before releasing the shared
			// owner; teardown validation must participate in the test outcome.
			try { fixture.Finish(); }
			catch(...) { fixture.WriteEvidence(directory / "native-evidence-final.json"); throw; }
			fixture.WriteEvidence(directory / "native-evidence-final.json");
		}
		struct StageNativeOwners {
			std::array<VkDescriptorSetLayout, 2U> sets{};
			VkPipelineLayout layout = VK_NULL_HANDLE;
			VkDescriptorPool pool = VK_NULL_HANDLE;
			VkShaderModule shader = VK_NULL_HANDLE;
			VkPipeline pipeline = VK_NULL_HANDLE;
		};
		AnimationStageReadback DispatchProductionStage(TestContext& context, VulkanReviewFixture& fixture,
			GpuAnimationStage stage, const std::vector<std::byte>& program, const std::vector<std::byte>& clip,
			const std::vector<std::byte>& poses, bool holdLast, const std::filesystem::path& evidence) {
			const auto& native = fixture.GetNative(); const auto& vk = *native.functions;
			const auto shaderName = stage == GpuAnimationStage::Blend ? "AnimationBlend" : "AnimationSample";
			const auto words = VulkanReviewFixture::CompileShader(std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Engine/Shaders/Animation" / (std::string(shaderName) + ".comp.hlsl"), "cs_6_0", "main", evidence / (std::string(shaderName) + ".spv"));
			context.Expect(!words.empty(), "The actual maintained production stage must compile to nonempty SPIR-V.");
			fixture.RequireShaderCapabilities(words);
			auto owners = std::make_shared<StageNativeOwners>();
			fixture.RegisterCleanup([owners](const VulkanReviewFixture::Native& owner) {
				const auto& functions = *owner.functions;
				if(owners->pipeline) functions.vkDestroyPipeline(owner.device, owners->pipeline, nullptr);
				if(owners->pool) functions.vkDestroyDescriptorPool(owner.device, owners->pool, nullptr);
				if(owners->layout) functions.vkDestroyPipelineLayout(owner.device, owners->layout, nullptr);
				for(auto set : owners->sets) if(set) functions.vkDestroyDescriptorSetLayout(owner.device, set, nullptr);
				if(owners->shader) functions.vkDestroyShaderModule(owner.device, owners->shader, nullptr);
			});
			VkDescriptorSetLayoutCreateInfo empty{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			CheckVk(vk.vkCreateDescriptorSetLayout(native.device, &empty, nullptr, &owners->sets[0U]), "empty descriptor set layout");
			std::array<VkDescriptorSetLayoutBinding, 16U> bindings{};
			for(std::uint32_t index = 0U; index < bindings.size(); ++index) bindings[index] = {index, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
			VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; setInfo.bindingCount = 16U; setInfo.pBindings = bindings.data();
			CheckVk(vk.vkCreateDescriptorSetLayout(native.device, &setInfo, nullptr, &owners->sets[1U]), "animation descriptor set layout");
			VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(GpuAnimationPushConstants)};
			VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount = 2U; layout.pSetLayouts = owners->sets.data();
			layout.pushConstantRangeCount = 1U; layout.pPushConstantRanges = &range;
			CheckVk(vk.vkCreatePipelineLayout(native.device, &layout, nullptr, &owners->layout), "animation pipeline layout");
			VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; shader.codeSize = words.size() * sizeof(std::uint32_t); shader.pCode = words.data();
			CheckVk(vk.vkCreateShaderModule(native.device, &shader, nullptr, &owners->shader), "production animation shader module");
			VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pipeline.layout = owners->layout;
			pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			pipeline.stage.module = owners->shader; pipeline.stage.pName = "main";
			CheckVk(vk.vkCreateComputePipelines(native.device, VK_NULL_HANDLE, 1U, &pipeline, nullptr, &owners->pipeline), "production animation pipeline");
			GpuAnimationGenerationRecord generation{};
			generation.instanceSlot = 1U; generation.instanceGeneration = generation.componentGeneration = generation.workGeneration = 1U;
			generation.programGeneration = generation.controllerGeneration = generation.skeletonGeneration = generation.clipGeneration = 1U;
			generation.meshGeneration = generation.resourceGeneration = generation.rendererGeneration = generation.sceneGeneration = 1U;
			generation.frameIdLow = 1U;
			GpuAnimationWorkPacket packet{}; packet.stableInstanceId = (1ULL << 32U) | 1U; packet.jointCount = 1U;
			packet.stageMask = GpuAnimationStageMask(stage); packet.clipResourceIndex = packet.programResourceIndex = packet.skeletonResourceIndex = 0U;
			if(holdLast) packet.flags |= static_cast<std::uint32_t>(GpuAnimationWorkFlag::UseHoldLastFallback);
			GpuAnimationDispatchRecord dispatch{}; dispatch.stableInstanceId = packet.stableInstanceId;
			dispatch.stage = static_cast<std::uint32_t>(stage); dispatch.elementCount = 1U;
			const std::uint32_t sampleTick = 1U;
			std::array<Review::VulkanReviewBuffer, 16U> buffers{};
			const std::vector<std::byte> zero(64U);
			for(auto& buffer : buffers) buffer = fixture.CreateBuffer(64U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, zero);
			auto bufferFor = [&](std::size_t index, const std::vector<std::byte>& bytes) {
				buffers[index] = fixture.CreateBuffer(std::max<std::size_t>(bytes.size(), 64U), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, bytes);
			};
			bufferFor(0U, ObjectBytes(generation)); bufferFor(1U, ObjectBytes(packet)); bufferFor(2U, ObjectBytes(dispatch));
			bufferFor(3U, program); bufferFor(4U, clip); bufferFor(9U, poses); bufferFor(15U, ObjectBytes(sampleTick));
			VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16U};
			VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets = 1U; pool.poolSizeCount = 1U; pool.pPoolSizes = &poolSize;
			CheckVk(vk.vkCreateDescriptorPool(native.device, &pool, nullptr, &owners->pool), "animation descriptor pool");
			VkDescriptorSet set = VK_NULL_HANDLE;
			VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocation.descriptorPool = owners->pool;
			allocation.descriptorSetCount = 1U; allocation.pSetLayouts = &owners->sets[1U];
			CheckVk(vk.vkAllocateDescriptorSets(native.device, &allocation, &set), "animation descriptor set");
			std::array<VkDescriptorBufferInfo, 16U> descriptors{}; std::array<VkWriteDescriptorSet, 16U> writes{};
			for(std::uint32_t index = 0; index < 16U; ++index) {
				descriptors[index] = {buffers[index].handle, 0U, buffers[index].bytes}; writes[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
				writes[index].dstSet = set; writes[index].dstBinding = index; writes[index].descriptorCount = 1U;
				writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[index].pBufferInfo = &descriptors[index];
			}
			vk.vkUpdateDescriptorSets(native.device, 16U, writes.data(), 0U, nullptr);
			GpuAnimationPushConstants push{}; push.stage = static_cast<std::uint32_t>(stage); push.dispatchRecordCount = 1U; push.elementCount = 1U; push.planGeneration = 1U;
			const auto submission = fixture.Submit([&](VkCommandBuffer command, const VulkanReviewFixture::Native& owner) {
				const auto& functions = *owner.functions;
				VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; before.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
				functions.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 1U, &before, 0U, nullptr, 0U, nullptr);
				functions.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, owners->pipeline);
				functions.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, owners->layout, 1U, 1U, &set, 0U, nullptr);
				functions.vkCmdPushConstants(command, owners->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(push), &push);
				functions.vkCmdDispatch(command, 1U, 1U, 1U);
				VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
				functions.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0U, 1U, &after, 0U, nullptr, 0U, nullptr);
			});
			context.Expect(submission.submitted && submission.completed && submission.result == VK_SUCCESS,
				"The actual production shader dispatch must cross a successful submitted fence before readback.");
			const auto poseBytes = fixture.Read(buffers[9U], sizeof(GpuAnimationPoseRecord));
			const auto diagnosticBytes = fixture.Read(buffers[14U], sizeof(GpuAnimationDiagnosticRecord));
			AnimationStageReadback result{}; std::memcpy(&result.pose, poseBytes.data(), sizeof(result.pose));
			std::memcpy(&result.diagnostic, diagnosticBytes.data(), sizeof(result.diagnostic));
			PersistBytes(evidence / "input-generation.bin", ObjectBytes(generation));
			PersistBytes(evidence / "input-work.bin", ObjectBytes(packet));
			PersistBytes(evidence / "input-dispatch.bin", ObjectBytes(dispatch));
			PersistBytes(evidence / "input-push.bin", ObjectBytes(push));
			PersistBytes(evidence / "input-sample-tick.bin", ObjectBytes(sampleTick));
			PersistBytes(evidence / "input-program.bin", program); PersistBytes(evidence / "input-clip.bin", clip);
			PersistBytes(evidence / "input-poses.bin", poses); PersistBytes(evidence / "output-pose.bin", poseBytes);
			PersistBytes(evidence / "output-diagnostic.bin", diagnosticBytes);
			fixture.RequireNoValidationErrors(); fixture.WriteEvidence(evidence / "native-evidence.json"); return result;
		}
		void WatchedNative(TestContext& context, const std::string& name, const std::function<void()>& body) {
			if(!VulkanReviewFixture::IsWatchedChild(name)) {
				const auto child = VulkanReviewFixture::RunWatchedSelf(name);
				context.Expect(child.started && !child.timedOut && child.exitCode == 0,
					"Native shader child must finish without timeout. Exit=" + std::to_string(child.exitCode) + " Output=" + child.output);
				return;
			}
			body();
		}
		VulkanReviewFixture::Options NativeOptions() {
			return VulkanReviewFixture::NativeDeviceOptions();
		}

	}
	void RegisterProceduralGpuPoseParityTests(TestRegistry& registry) {
		registry.Add("Animation.Review.Procedural.CopyModelAndWorldTargetsStayModelSpace", [](TestContext& context) {
			QualifyCopySpace(context, Animation::ProceduralCoordinateSpace::Model);
			QualifyCopySpace(context, Animation::ProceduralCoordinateSpace::World);
		});
		registry.Add("Animation.Review.Procedural.CopyParentAndLocalTargetsComposeToModelSpace", [](TestContext& context) {
			QualifyCopySpace(context, Animation::ProceduralCoordinateSpace::Parent);
			QualifyCopySpace(context, Animation::ProceduralCoordinateSpace::Local);
		});
		// Legacy unfiltered unit runs must never initialize a device or launch children.
		if(VulkanReviewFixture::IsNativeReviewEnabled()) {

			registry.Add("Native.ReviewSimulation.Animation.AdditiveQuaternionMatchesIndependentHamiltonOracle", [](TestContext& context) {
				const std::string name = "Native.ReviewSimulation.Animation.AdditiveQuaternionMatchesIndependentHamiltonOracle";
				WatchedNative(context, name, [&] {
					VulkanReviewFixture fixture(name, NativeOptions());
					const auto directory = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "out/validation/review-remediation/native-animation"
						/ ("additive-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
					context.Expect(std::filesystem::create_directories(directory), "Reserve a fresh native quaternion evidence root.");
					const glm::dquat base = glm::angleAxis(0.8, glm::dvec3(1.0, 0.0, 0.0));
					const glm::dquat reference = glm::angleAxis(0.45, glm::dvec3(0.0, 0.0, 1.0));
					const glm::dquat source = glm::angleAxis(1.2, glm::dvec3(0.0, 1.0, 0.0)) * reference;
					constexpr float weight = 0.35F;
					std::array<AnimationStageReadback, 2U> outputs;
					for(std::size_t sign = 0U; sign < 2U; ++sign) {
						std::vector<std::byte> poses(144U); WriteBytes(poses, 0U, PoseRecord(base));
						WriteBytes(poses, 48U, PoseRecord(sign == 0U ? source : -source)); WriteBytes(poses, 96U, PoseRecord(reference));
						std::vector<std::byte> program(80U); const std::array<std::uint32_t, 5U> layer{1U, 48U, 96U, std::bit_cast<std::uint32_t>(weight), 1U};
						WriteBytes(program, 16U, layer);
						outputs[sign] = DispatchProductionStage(context, fixture, GpuAnimationStage::Blend, program, std::vector<std::byte>(64U), poses, false, directory / std::to_string(sign));
					}
					// The delta is reference-local because it is applied on the right.
					glm::dquat delta = glm::normalize(glm::inverse(reference) * source); if(delta.w < 0.0) delta = -delta;
					const glm::dquat weighted = glm::normalize(glm::dquat(1.0, 0.0, 0.0, 0.0) * (1.0 - weight) + delta * static_cast<double>(weight));
					const glm::dquat expected = glm::normalize(base * weighted);
					PersistBytes(directory / "independent-hamilton-oracle-pose.bin", ObjectBytes(PoseRecord(expected)));
					for(const auto& output : outputs) {
						const auto& q = output.pose.rotation; const glm::dquat actual(q[3U], q[0U], q[1U], q[2U]);
						context.Expect(output.diagnostic.flags == 0U && std::abs(glm::dot(expected, glm::normalize(actual))) > 0.99999,
							"Real production blend readback must implement Hamilton composition and the same shortest rotation for q and -q.");
					}
					// This endpoint oracle uses the authored source directly, independently
					// of any copied delta formula: reference * delta must reconstruct it.
					std::vector<std::byte> endpointPoses(144U);
					WriteBytes(endpointPoses, 0U, PoseRecord(reference)); WriteBytes(endpointPoses, 48U, PoseRecord(source)); WriteBytes(endpointPoses, 96U, PoseRecord(reference));
					std::vector<std::byte> endpointProgram(80U);
					const std::array<std::uint32_t, 5U> endpointLayer{1U, 48U, 96U, std::bit_cast<std::uint32_t>(1.0F), 1U};
					WriteBytes(endpointProgram, 16U, endpointLayer);
					const auto endpoint = DispatchProductionStage(context, fixture, GpuAnimationStage::Blend, endpointProgram, std::vector<std::byte>(64U), endpointPoses, false, directory / "full-reference");
					const auto& q = endpoint.pose.rotation;
					context.Expect(endpoint.diagnostic.flags == 0U && std::abs(glm::dot(source, glm::normalize(glm::dquat(q[3], q[0], q[1], q[2])))) > 0.99999,
						"Full noncommuting reference delta must reconstruct the source on the actual GPU.");
					FinishEvidence(fixture, directory);
				});
			});
			registry.Add("Native.ReviewSimulation.Animation.LateSampleDecodeFailureRestoresWholeFallback", [](TestContext& context) {
				const std::string name = "Native.ReviewSimulation.Animation.LateSampleDecodeFailureRestoresWholeFallback";
				WatchedNative(context, name, [&] {
					VulkanReviewFixture fixture(name, NativeOptions());
					const auto directory = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "out/validation/review-remediation/sol-3/native-animation/fallback";
					AnimationCodecTrackHeaderV1 header; header.encoding = AnimationCodecValueEncoding::Vector3Unorm16;
					header.flags = kAnimationCodecConstantTrack; header.byteSize = sizeof(header); header.keyCount = 1U; header.segmentEndTick = 60;
					header.constantVector = {7.0F, 8.0F, 9.0F};
					auto clip = ObjectBytes(header);
					// Allocate the whole second header at a valid aligned address. Only
					// its magic is invalid, so rejection occurs inside the bounded decoder
					// before any key/count-dependent fetch; no OOB locator is submitted.
					auto rejectedHeader = header; rejectedHeader.magic = 0U;
					WriteBytes(clip, sizeof(header), rejectedHeader);
					std::vector<std::byte> program(80U);
					const std::array<std::uint32_t, 4U> locators{0U, static_cast<std::uint32_t>(sizeof(header)), 0U, 0U}; WriteBytes(program, 64U, locators);
					const auto hold = PoseRecord(glm::angleAxis(0.3, glm::dvec3(1.0, 0.0, 0.0)), {3.0F, 4.0F, 5.0F});
					const auto bindOutput = DispatchProductionStage(context, fixture, GpuAnimationStage::Sample, program, clip, ObjectBytes(hold), false, directory / "bind");
					const auto holdOutput = DispatchProductionStage(context, fixture, GpuAnimationStage::Sample, program, clip, ObjectBytes(hold), true, directory / "hold");
					const auto bind = PoseRecord(glm::dquat(1.0, 0.0, 0.0, 0.0));
					PersistBytes(directory / "independent-bind-oracle-pose.bin", ObjectBytes(bind));
					PersistBytes(directory / "independent-hold-oracle-pose.bin", ObjectBytes(hold));
					FinishEvidence(fixture, directory);
					context.Expect(std::memcmp(&holdOutput.pose, &hold, sizeof(hold)) == 0 && holdOutput.diagnostic.flags == 0x201U,
						"A later channel failure must preserve the exact hold-last pose and report invalid-record plus hold-last, without falsely claiming bind fallback.");
					context.Expect(std::memcmp(&bindOutput.pose, &bind, sizeof(bind)) == 0 && (bindOutput.diagnostic.flags & 0x101U) == 0x101U,
						"A later rotation decode failure must discard the earlier successful translation and restore the entire declared bind fallback.");
				});
			});

		}
	}
}
