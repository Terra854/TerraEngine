// Controller ValidationCreateControllerFixture.cpp - Produce the checked Controller Validation controller assets.
//
// This small fixture producer intentionally calls the same serializers and graph
// compiler as the editor.  It keeps the binary test inputs reproducible without
// inventing a text-only stand-in for .teanimctrl or .teanimmask files.

#include "Animation/Assets/AnimationControllerAsset.h"
#include "Animation/Assets/AnimationMaskAsset.h"
#include "Animation/Graph/AnimationGraphCompiler.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {
	using namespace TerraEngine::Animation;

	AnimationGuid FixtureGuid(std::uint8_t seed) {
		AnimationGuid result;
		for(std::size_t index = 0U; index < result.value.size(); ++index)
			result.value[index] = static_cast<std::uint8_t>(seed + index);
		return result;
	}

	bool WriteCheckedBytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
		std::filesystem::create_directories(path.parent_path());
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if(!output) return false;
		output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		return static_cast<bool>(output);
	}

	AnimationControllerAsset MakeController(const AnimationGuid& skeletonId, const AnimationGuid& maskId) {
		AnimationControllerAsset controller;
		controller.assetId = FixtureGuid(0x31U);
		controller.skeletonAssetId = skeletonId;
		controller.compatibilityFingerprint = 0x4354524C00000001ULL;
		controller.parameters = {
			{100U, "Speed", AnimationControllerParameterType::Float, 0.0F},
			{101U, "PlayAction", AnimationControllerParameterType::Trigger, std::monostate{}},
			{102U, "Direction", AnimationControllerParameterType::Vector, AnimationFloat3{0.0F, 0.0F, 1.0F}},
			{103U, "Grounded", AnimationControllerParameterType::Boolean, true},
			{104U, "Surface", AnimationControllerParameterType::Tag, std::string{"training-floor"}},
			{105U, "AimTarget", AnimationControllerParameterType::Transform, AnimationTransform{}},
			{106U, "ActionCount", AnimationControllerParameterType::Integer, std::int32_t{0}}};
		controller.graphVariables = {
			{200U, "LocomotionSpeed", AnimationControllerParameterType::Float, 0.0F},
			{201U, "LocomotionDirection", AnimationControllerParameterType::Vector, AnimationFloat3{0.0F, 0.0F, 1.0F}}};
		controller.reusableBindings = {{210U, 100U, 200U}, {211U, 102U, 201U}};

		// Procedural leaves keep this self-contained fixture independently loadable;
		// the graph still exercises the production blend/state/layer serialization.
		controller.nodes = {
			{300U, AnimationControllerNodeKind::ProceduralPose},
			{301U, AnimationControllerNodeKind::ProceduralPose},
			{302U, AnimationControllerNodeKind::BlendTree1D},
			{303U, AnimationControllerNodeKind::ProceduralPose},
			{304U, AnimationControllerNodeKind::ProceduralPose},
			{305U, AnimationControllerNodeKind::BlendTree2D},
			{306U, AnimationControllerNodeKind::BlendTree1D}};
		controller.nodes[2].blendSamples = {{320U, 300U, 0.0F, 0.0F}, {321U, 301U, 1.0F, 0.0F}};
		controller.nodes[5].blendSamples = {{330U, 300U, -1.0F, 0.0F}, {331U, 301U, 1.0F, 0.0F}, {332U, 303U, 0.0F, 1.0F}};
		controller.nodes[6].blendSamples = {{340U, 302U, 0.0F, 0.0F}, {341U, 305U, 1.0F, 0.0F}};
		controller.states = {{400U, 306U, 0U, true, false, false}, {401U, 303U, 0U, false, false, false}};
		controller.transitions = {
			{410U, 400U, 401U, 101U, 12, 0, 0.0F, 20U},
			{411U, 401U, 400U, 101U, 12, 0, 0.0F, 20U}};
		controller.layers = {
			{500U, 400U, {}, 0U, 0U, false, 1.0F},
			{501U, 401U, maskId, 1U, controller.compatibilityFingerprint, true, 0.65F}};
		controller.syncGroups = {{600U, "Locomotion", 400U, {{610U, "FootPlant", 700U, 400U}, {611U, "FootPlant", 700U, 401U}}}};
		return controller;
	}

	AnimationMaskAsset MakeMask(const AnimationGuid& skeletonId, AnimationCompatibilityFingerprint fingerprint) {
		AnimationMaskAsset mask;
		mask.assetId = FixtureGuid(0x51U);
		mask.skeletonAssetId = skeletonId;
		mask.compatibilityFingerprint = fingerprint;
		mask.jointSelectors = {{1U, 1.0F, true}};
		mask.branchSelectors = {{10U, 0.75F, true}};
		mask.morphSelectors = {{20U, true}};
		mask.curveSelectors = {{30U, true}};
		mask.rootMotionWeight = 0.0F;
		mask.compiledJointWeights = {{1U, 1.0F}, {10U, 0.75F}};
		mask.compiledMorphIds = {20U};
		mask.compiledCurveIds = {30U};
		return mask;
	}
}

int main(int argumentCount, char** arguments) {
	if(argumentCount != 2) {
		std::cerr << "Usage: Controller ValidationCreateControllerFixture <fixture-root>\n";
		return 2;
	}
	const std::filesystem::path fixtureRoot = arguments[1];
	const AnimationGuid skeletonId = FixtureGuid(0x11U);
	const AnimationMaskAsset mask = MakeMask(skeletonId, 0x4354524C00000001ULL);
	const AnimationControllerAsset controller = MakeController(skeletonId, mask.assetId);
	EvaluationProgram program;
	if(CompileAnimationControllerGraph(controller, {}, program).status != AnimationStatus::Ok) {
		std::cerr << "Controller Validation controller graph did not compile.\n";
		return 3;
	}
	std::vector<std::uint8_t> maskBytes;
	std::vector<std::uint8_t> controllerBytes;
	if(SerializeAnimationMaskAsset(mask, maskBytes) != AnimationStatus::Ok
		|| SerializeAnimationControllerAsset(controller, controllerBytes) != AnimationStatus::Ok) {
		std::cerr << "Controller Validation fixture serialization failed.\n";
		return 4;
	}
	AnimationMaskAsset decodedMask;
	AnimationControllerAsset decodedController;
	if(DeserializeAnimationMaskAsset(maskBytes, mask.compatibilityFingerprint, decodedMask) != AnimationStatus::Ok
		|| DeserializeAnimationControllerAsset(controllerBytes, controller.compatibilityFingerprint, decodedController) != AnimationStatus::Ok
		|| decodedMask != mask || decodedController != controller) {
		std::cerr << "Controller Validation fixture round-trip verification failed.\n";
		return 5;
	}
	if(!WriteCheckedBytes(fixtureRoot / "Assets/Masks/ControllerUpperBody.teanimmask", maskBytes)
		|| !WriteCheckedBytes(fixtureRoot / "Assets/Controllers/ControllerLocomotionAction.teanimctrl", controllerBytes)) {
		std::cerr << "Controller Validation fixture output write failed.\n";
		return 6;
	}
	return 0;
}
