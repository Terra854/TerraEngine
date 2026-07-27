// AnimationControllerAssetTests.cpp - Durable controller and mask asset contracts.

#include "TestHarness.h"

#include "Animation/Assets/AnimationControllerAsset.h"
#include "Animation/Assets/AnimationMaskAsset.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		AnimationGuid Guid(std::uint8_t seed) {
			AnimationGuid value;
			for(std::size_t index = 0U; index < value.value.size(); ++index) value.value[index] = static_cast<std::uint8_t>(seed + index);
			return value;
		}

		AnimationControllerAsset MakeController(bool reverse) {
			AnimationControllerAsset asset;
			asset.assetId = Guid(1U);
			asset.skeletonAssetId = Guid(20U);
			asset.compatibilityFingerprint = 0x1234U;
			asset.parameters = {{1U, "Grounded", AnimationControllerParameterType::Boolean, true},
				{2U, "Speed", AnimationControllerParameterType::Float, 1.0F},
				{3U, "Action", AnimationControllerParameterType::Trigger, std::monostate{}},
				{4U, "Count", AnimationControllerParameterType::Integer, std::int32_t{2}},
				{5U, "Offset", AnimationControllerParameterType::Transform, AnimationTransform{}},
				{6U, "Surface", AnimationControllerParameterType::Tag, std::string{"stone"}}};
			asset.graphVariables = {{11U, "MoveScale", AnimationControllerParameterType::Float, 1.0F}, {12U, "Move", AnimationControllerParameterType::Vector, AnimationFloat3{1.0F, 0.0F, 0.0F}}};
			asset.reusableBindings = {{20U, 2U, 11U}};
			asset.nodes = {{100U, AnimationControllerNodeKind::Clip, 0U, Guid(40U), 1U, 0x1234U},
				{101U, AnimationControllerNodeKind::BlendTree1D, 0U, Guid(41U), 1U, 0x1234U}};
			asset.nodes[1].outputCompatibilityFingerprint = 0x1234U;
			asset.nodes[1].blendSamples = {{700U, 100U, 0.0F, 0.0F}};
			asset.states = {{200U, 100U, 0U, true, false, false}, {201U, 101U, 0U, false, true, false}};
			asset.transitions = {{300U, 200U, 201U, 2U, 10, 0, 0.0F, 0U}, {301U, 201U, 200U, 3U, 10, 0, 0.0F, 1U}};
			asset.layers = {{400U, 200U, Guid(50U), 1U, 0x1234U, false, 1.0F}};
			asset.syncGroups = {{500U, "Locomotion", 200U, {{501U, "FootPlant", 1100U, 200U}}}};
			asset.subgraphDependencies = {{600U, Guid(60U), 1U, 0x1234U}};
			if(reverse) {
				std::reverse(asset.parameters.begin(), asset.parameters.end());
				std::reverse(asset.nodes.begin(), asset.nodes.end());
				std::reverse(asset.transitions.begin(), asset.transitions.end());
			}
			return asset;
		}

		AnimationMaskAsset MakeMask() {
			AnimationMaskAsset asset;
			asset.assetId = Guid(70U);
			asset.skeletonAssetId = Guid(20U);
			asset.compatibilityFingerprint = 0x1234U;
			asset.jointSelectors = {{10U, 0.5F, true}};
			asset.branchSelectors = {{20U, 1.0F, false}};
			asset.morphSelectors = {{30U, true}};
			asset.curveSelectors = {{40U, true}};
			asset.rootMotionWeight = 0.25F;
			asset.compiledJointWeights = {{10U, 0.5F}, {20U, 1.0F}, {21U, 1.0F}};
			asset.compiledMorphIds = {30U};
			asset.compiledCurveIds = {40U};
			return asset;
		}

		// Re-envelop a deliberately changed payload so deserialization reaches its
		// count guards instead of correctly stopping at the content-hash boundary.
		std::vector<std::uint8_t> ReplacePayloadCount(const std::vector<std::uint8_t>& source, std::size_t fixedHeaderBytes, std::size_t countIndex, std::uint32_t count) {
			AnimationAssetReader reader;
			if(reader.Parse(source) != AnimationStatus::Ok) return {};
			std::vector<std::uint8_t> payload(reader.FindChunk(kAnimationChunkPayload).begin(), reader.FindChunk(kAnimationChunkPayload).end());
			const std::size_t countOffset = fixedHeaderBytes + countIndex * sizeof(std::uint32_t);
			if(countOffset > payload.size() || payload.size() - countOffset < sizeof(count)) return {};
			std::memcpy(payload.data() + countOffset, &count, sizeof(count));
			AnimationAssetWriter writer;
			if(!writer.AddChunk(kAnimationChunkPayload, true, payload)) return {};
			AnimationAssetEnvelope envelope{};
			envelope.assetId = reader.Envelope().assetId;
			envelope.compatibilityFingerprint = reader.Envelope().compatibilityFingerprint;
			std::vector<std::uint8_t> rebuilt;
			return writer.Finish(envelope, rebuilt) == AnimationStatus::Ok ? rebuilt : std::vector<std::uint8_t>{};
		}
	}

	void RegisterAnimationControllerAssetTests(TestRegistry& registry) {
		registry.Add("Animation.ControllerAsset.TypedGraphDependenciesRoundTripDeterministically", [](TestContext& context) {
			std::vector<std::uint8_t> forward;
			std::vector<std::uint8_t> reverse;
			context.Expect(SerializeAnimationControllerAsset(MakeController(false), forward) == AnimationStatus::Ok
				&& SerializeAnimationControllerAsset(MakeController(true), reverse) == AnimationStatus::Ok
				&& forward == reverse,
				"Canonical controller serialization must ignore insertion order while retaining typed graph data and dependencies.");
			AnimationControllerAsset restored;
			context.Expect(DeserializeAnimationControllerAsset(forward, 0x1234U, restored) == AnimationStatus::Ok
				&& restored == MakeController(false),
				"Controller parameters, bindings, graph records, layers, sync groups, and subgraph dependencies must round-trip exactly.");
		});

		registry.Add("Animation.ControllerAsset.RejectsTypeLimitAndCompatibilityFailuresAtomically", [](TestContext& context) {
			AnimationControllerAsset invalid = MakeController(false);
			invalid.parameters[1U].defaultValue = true;
			context.Expect(ValidateAnimationControllerAsset(invalid) == AnimationStatus::InvalidArgument,
				"A declared parameter type must match its durable default value.");
			invalid = MakeController(false);
			invalid.nodes.resize(static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::ControllerNodes).hardMaximum) + 1U);
			context.Expect(ValidateAnimationControllerAsset(invalid) == AnimationStatus::HardLimitExceeded,
				"Controller node limits must reject before graph evaluation or allocation can begin.");
			invalid = MakeController(false);
			invalid.reusableBindings.front().destinationVariableId = 12U;
			context.Expect(ValidateAnimationControllerAsset(invalid) == AnimationStatus::InvalidArgument,
				"Reusable bindings must reject implicit conversion between durable graph variable types.");
			std::vector<std::uint8_t> bytes;
			SerializeAnimationControllerAsset(MakeController(false), bytes);
			AnimationControllerAsset unchanged = MakeController(false);
			unchanged.assetId = Guid(90U);
			context.Expect(DeserializeAnimationControllerAsset(bytes, 0x9999U, unchanged) == AnimationStatus::IncompatibleAsset
				&& unchanged.assetId == Guid(90U),
				"Compatibility mismatch must leave the caller's last-known-good controller untouched.");
			const std::size_t controllerHeaderBytes = sizeof(std::uint32_t) + sizeof(std::uint16_t) * 2U + sizeof(AnimationGuid) + sizeof(AnimationCompatibilityFingerprint);
			const std::vector<std::uint8_t> malicious = ReplacePayloadCount(bytes, controllerHeaderBytes, 2U, static_cast<std::uint32_t>(GetAnimationRuntimeLimit(AnimationLimit::ControllerParameters).hardMaximum + 1U));
			context.Expect(!malicious.empty()
				&& DeserializeAnimationControllerAsset(malicious, 0x1234U, unchanged) == AnimationStatus::HardLimitExceeded
				&& unchanged.assetId == Guid(90U),
				"An oversized auxiliary binding count must reject before allocation and preserve caller output.");
		});

		registry.Add("Animation.ControllerAsset.PersistsIncompleteGraphsAndInternalNodes", [](TestContext& context) {
			AnimationControllerAsset authored = MakeController(false);
			authored.states.front().entry = false;
			authored.nodes.back() = {101U, AnimationControllerNodeKind::ProceduralPose, 0U, {}, 0U, 0U};
			authored.layers.front().maskAssetId = {};
			authored.layers.front().maskGeneration = 0U;
			authored.layers.front().maskFingerprint = 0U;
			std::vector<std::uint8_t> bytes;
			AnimationControllerAsset restored;
			context.Expect(SerializeAnimationControllerAsset(authored, bytes) == AnimationStatus::Ok
				&& DeserializeAnimationControllerAsset(bytes, 0x1234U, restored) == AnimationStatus::Ok
				&& restored == authored,
				"Incomplete authored entry state, internal nodes, and unmasked layers must persist for later compiler diagnostics.");
		});

		registry.Add("Animation.MaskAsset.SelectorsCompileAndRoundTripDeterministically", [](TestContext& context) {
			const AnimationMaskAsset mask = MakeMask();
			context.Expect(ValidateAnimationMaskAsset(mask) == AnimationStatus::Ok,
				"Joint branches, morphs, curves, root motion, and dense weights must form one compatible bounded mask.");
			std::vector<std::uint8_t> bytes;
			AnimationMaskAsset restored;
			context.Expect(SerializeAnimationMaskAsset(mask, bytes) == AnimationStatus::Ok
				&& DeserializeAnimationMaskAsset(bytes, 0x1234U, restored) == AnimationStatus::Ok
				&& restored == mask,
				"Mask selectors and compiled dense tables must serialize without depending on mutable editor state.");
			const std::size_t maskHeaderBytes = sizeof(std::uint32_t) + sizeof(std::uint16_t) * 2U + sizeof(AnimationGuid) + sizeof(AnimationCompatibilityFingerprint) + sizeof(float);
			const std::vector<std::uint8_t> malicious = ReplacePayloadCount(bytes, maskHeaderBytes, 5U, static_cast<std::uint32_t>(GetAnimationRuntimeLimit(AnimationLimit::MeshMorphTargets).hardMaximum + 1U));
			AnimationMaskAsset unchanged = MakeMask();
			unchanged.assetId = Guid(91U);
			context.Expect(!malicious.empty()
				&& DeserializeAnimationMaskAsset(malicious, 0x1234U, unchanged) == AnimationStatus::HardLimitExceeded
				&& unchanged.assetId == Guid(91U),
				"Oversized dense morph tables must reject before resize and leave mask output atomic.");
		});
	}
}
