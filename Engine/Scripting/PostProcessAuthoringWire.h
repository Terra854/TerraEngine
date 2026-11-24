// Pure copied authoring protocol: no resource lookup, scene mutation or publication.
#pragma once
#include "Renderer/GraphicsSettings/GraphicsSettingsTypes.h"
#include "Renderer/Graph/PostProcessCustomPass.h"
#include <cstddef>
#include <memory>
#include <string_view>

struct NativeGraphicsSettingsTargetPacket;
struct NativeGraphicsSettingsReceiptPacket;
enum class NativeGraphicsSettingsStatus : std::uint32_t;
namespace TerraEngine::Resource { struct PostProcessEffectAsset; }
namespace TerraEngine::Scripting::PostProcessAuthoringWire
{
	namespace Settings = Renderer::GraphicsSettings;
	inline constexpr std::uint32_t kAbiVersion = 1;
	inline constexpr std::size_t kMaximumBytes = 1024u * 1024u;
	enum class Operation { SetCameraProfile, PatchCustomEffects, ResetCustomEffects };
	struct EffectPatch
	{
		UUID assetId = EMPTY_UUID;
		std::optional<bool> enabled;
		std::vector<Renderer::CustomPostProcessParameterValue> parameters;
		std::vector<std::string> resetParameters;
	};
	struct Request
	{
		Operation operation = Operation::SetCameraProfile;
		Settings::GraphicsTarget target;
		Settings::GraphicsLifecycleStamp lifecycle;
		Settings::GraphicsRevision baseRevision = 0;
		// Null is an explicit authored profile clear only for SetCameraProfile.
		std::optional<UUID> profileAssetId;
		std::vector<EffectPatch> effects;
		std::vector<UUID> resetEffectAssetIds;
		bool clearAll = false;
	};
	struct Diagnostic
	{
		NativeGraphicsSettingsStatus status = static_cast<NativeGraphicsSettingsStatus>(0);
		Settings::GraphicsRequestReason reason = Settings::GraphicsRequestReason::None;
		std::string detail;
	};
	struct DescriptorState
	{
		std::shared_ptr<const Resource::PostProcessEffectAsset> effect;
		bool available = false;
		Settings::GraphicsRequestReason reason = Settings::GraphicsRequestReason::None;
		std::string detail;
	};
	struct EffectState
	{
		UUID assetId = EMPTY_UUID;
		bool enabled = false;
		std::vector<Renderer::CustomPostProcessParameterValue> parameters;
	};
	struct Snapshot
	{
		Settings::GraphicsTarget target;
		Settings::GraphicsLifecycleStamp lifecycle;
		Settings::GraphicsRevision revision = 0;
		UUID authoredProfileAssetId = EMPTY_UUID;
		// Callers supply real protected scene visibility and accepted output proof
		// separately. The codec never derives either frame from the other.
		std::uint64_t sceneVisibleFrameId = 0, effectiveFrameId = 0;
		std::vector<DescriptorState> descriptors;
		std::vector<EffectState> requestedEffects, effectiveEffects;
		Settings::GraphicsRequestReason reason = Settings::GraphicsRequestReason::None;
		std::string detail;
	};
	struct Receipt
	{
		Settings::GraphicsRequestReceipt request;
		std::uint64_t sceneVisibleFrameId = 0, effectiveFrameId = 0;
	};
	// Decode is transactional: on any malformed member the previous DTO survives.
	// The protected admission owner must still revalidate current lifetimes,
	// revisions, resource readiness, parameter schemas and capabilities.
	bool DecodeTarget(const NativeGraphicsSettingsTargetPacket*, Settings::GraphicsTarget&, Diagnostic&);
	bool DecodeRequest(const NativeGraphicsSettingsTargetPacket*, std::span<const std::byte>, Request&, Diagnostic&);
	bool SerializeSnapshot(const Snapshot&, std::string& inOutJson, Diagnostic&);
	bool SerializeReceipt(const Receipt&, std::string& inOutJson, Diagnostic&);
	NativeGraphicsSettingsStatus CopyJson(std::string_view, char*, std::uint32_t capacity, std::uint32_t* required);
	// Validate before admission. WriteReceipt copies admission facts only; it does
	// not claim that a scene update or native rendered output has completed.
	NativeGraphicsSettingsStatus ValidateReceiptOutput(const NativeGraphicsSettingsReceiptPacket*);
	NativeGraphicsSettingsStatus WriteReceipt(const Settings::GraphicsRequestReceipt&, NativeGraphicsSettingsReceiptPacket*);
}
