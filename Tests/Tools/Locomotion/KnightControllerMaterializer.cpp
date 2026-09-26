// Validation-only authoring helper for the downloaded CC0 navigation-demo knight.
// Compile against TerraEngine.lib; run with a NEW .teanimctrl output path.
// Uses the production codec so the demo does not depend on a private binary format.
#include "Animation/Assets/AnimationControllerAsset.h"
#include "Utilities/UUID.h"
#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
	using namespace TerraEngine;
	using namespace TerraEngine::Animation;
	if (argc != 2 || std::filesystem::exists(argv[1])) return 2;
	const auto guid = [](const char* text) { return AnimationGuid{*ParseUUID(text)}; };
	constexpr std::uint64_t fingerprint = 12045776358348429517ULL;
	AnimationControllerAsset controller;
	controller.assetId = guid("06002309000040008000000000000001");
	controller.skeletonAssetId = guid("8d2409c0d3230a7c3c3d00d09250cfe7");
	controller.compatibilityFingerprint = fingerprint;
	controller.parameters = {{601, "Speed", AnimationControllerParameterType::Float, 0.0F},
		{701, "GoIdle", AnimationControllerParameterType::Trigger, std::monostate{}},
		{702, "GoWalk", AnimationControllerParameterType::Trigger, std::monostate{}},
		{703, "GoRun", AnimationControllerParameterType::Trigger, std::monostate{}}};
	const char* clips[] = {"2412d596d61e6c5c79459742a112145e", "a543cfca9e3f4f4a8cf255cc397ae2d6", "2f489fafa00c1c3fb24fbf17ce22860c"};
	for (std::uint64_t i = 0; i < 3; ++i) {
		controller.nodes.push_back({10 + i, AnimationControllerNodeKind::Clip, 0, guid(clips[i]), 1, fingerprint});
		controller.states.push_back({101 + i, 10 + i, 0, i == 0, false, false});
	}
	// Every state can reach every other state; 12 fixed ticks give a 0.2 s blend.
	for (std::uint64_t from = 0; from < 3; ++from) {
		for (std::uint64_t to = 0; to < 3; ++to) {
			if (from == to) continue;
			AnimationControllerTransition transition;
			transition.stableId = 201 + from * 3 + to;
			transition.sourceStateId = 101 + from;
			transition.destinationStateId = 101 + to;
			transition.conditionParameterId = 701 + to;
			transition.durationTicks = 12;
			transition.interruptionPolicy = AnimationControllerInterruptionPolicy::Replace;
			controller.transitions.push_back(transition);
		}
	}
	controller.layers.push_back({301, 101, {}, 0, 0, false, 1.0F});
	std::vector<std::uint8_t> bytes;
	if (SerializeAnimationControllerAsset(controller, bytes) != AnimationStatus::Ok) return 3;
	AnimationControllerAsset roundTrip;
	if (DeserializeAnimationControllerAsset(bytes, fingerprint, roundTrip) != AnimationStatus::Ok) return 4;
	std::ofstream output(argv[1], std::ios::binary);
	output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	std::cout << "Validated three states and six transitions; bytes=" << bytes.size() << '\n';
	return output.good() ? 0 : 5;
}
