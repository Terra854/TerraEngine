// AnimationTypes.cpp - Human-readable labels for stable animation statuses.

#include "Animation/AnimationTypes.h"

namespace TerraEngine::Animation {
	std::string_view ToString(AnimationStatus status) noexcept {
		switch(status) {
		case AnimationStatus::Ok: return "Ok";
		case AnimationStatus::Queued: return "Queued";
		case AnimationStatus::NotReady: return "NotReady";
		case AnimationStatus::BufferTooSmall: return "BufferTooSmall";
		case AnimationStatus::InvalidArgument: return "InvalidArgument";
		case AnimationStatus::InvalidType: return "InvalidType";
		case AnimationStatus::MissingAsset: return "MissingAsset";
		case AnimationStatus::IncompatibleAsset: return "IncompatibleAsset";
		case AnimationStatus::StaleHandle: return "StaleHandle";
		case AnimationStatus::Cancelled: return "Cancelled";
		case AnimationStatus::Unsupported: return "Unsupported";
		case AnimationStatus::DeterminismViolation: return "DeterminismViolation";
		case AnimationStatus::RendererUnavailable: return "RendererUnavailable";
		case AnimationStatus::Rejected: return "Rejected";
		case AnimationStatus::InternalError: return "InternalError";
		case AnimationStatus::UnsupportedMajorVersion: return "UnsupportedMajorVersion";
		case AnimationStatus::InvalidEnvelope: return "InvalidEnvelope";
		case AnimationStatus::TruncatedInput: return "TruncatedInput";
		case AnimationStatus::RangeOverflow: return "RangeOverflow";
		case AnimationStatus::RangeOutOfBounds: return "RangeOutOfBounds";
		case AnimationStatus::HardLimitExceeded: return "HardLimitExceeded";
		case AnimationStatus::UnknownRequiredChunk: return "UnknownRequiredChunk";
		case AnimationStatus::InvalidChunk: return "InvalidChunk";
		case AnimationStatus::ContentHashMismatch: return "ContentHashMismatch";
		}
		return "Unknown";
	}
}
