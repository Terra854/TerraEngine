// AnimationAssetAuthoringDecode.h - Checked decoding when an editor owns no external fingerprint yet.
//
// Runtime consumers normally know the compatibility fingerprint expected by a
// scene/controller binding and must pass that value to the family decoder.
// Authoring windows instead open the document before such a binding exists.
// They may use the envelope's checked fingerprint, while the family decoder
// still reparses the bytes and verifies the matching payload fingerprint.

#pragma once

#include "Engine/File Formats/TerraEngineAnimationFile.h"

#include <cstdint>
#include <span>

namespace TerraEngine::Editor::Animation {
	template<typename AssetType>
	[[nodiscard]] TerraEngine::Animation::AnimationStatus DeserializeAnimationAssetForAuthoring(
		std::span<const std::uint8_t> input,
		AssetType& output,
		TerraEngine::Animation::AnimationStatus (*decoder)(
			std::span<const std::uint8_t>,
			TerraEngine::Animation::AnimationCompatibilityFingerprint,
			AssetType&)) {
		using namespace TerraEngine::Animation;
		AnimationAssetReader envelopeReader;
		const AnimationStatus envelopeStatus = envelopeReader.Parse(input);
		if(envelopeStatus != AnimationStatus::Ok) {
			return envelopeStatus;
		}
		return decoder(input, envelopeReader.Envelope().compatibilityFingerprint, output);
	}
}
