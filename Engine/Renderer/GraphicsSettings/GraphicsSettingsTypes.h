// GraphicsSettingsTypes.h - Copied requests and immutable graphics publications.
//
// These native records belong to the renderer's versioned settings workflow.
// They deliberately contain no managed or Vulkan pointers. A producer copies a
// request, a worker prepares a candidate, and the protected render boundary is
// the only place that can publish that candidate as effective frame settings.
#pragma once

#include "Utilities/UUID.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

// Reuse the existing renderer enums without pulling the complete rendering
// implementation into every settings producer and persistence adapter.
enum class QualityPreset : std::uint32_t;
enum class RenderViewType : std::uint8_t;
enum class RenderOutputKind : std::uint8_t;

namespace TerraEngine::Renderer::GraphicsSettings
{
	struct PostProcessAuthoringFrame;
	using GraphicsRequestId = std::uint64_t;
	using GraphicsRevision = std::uint64_t;

	// IDs are serialized and exposed to managed callers. Keep assigned values
	// stable when adding settings; zero always denotes an invalid setting.
	enum class GraphicsSettingId : std::uint32_t
	{
		Invalid = 0,
		ShadowResolution = 1,
		ShadowFilteringSize = 2,
		MaxUploadedLights = 3,
		MaxDynamicLights = 4,
		MaxShadowLights = 5,
		MaxAreaLights = 6,
		EnableLTC = 7,
		EnableIBL = 8,
		IndirectStrength = 9,
		AmbientOcclusionStrength = 10,
		AmbientOcclusionQuality = 11,
		AnisotropicFiltering = 12,
		VariableRateShading = 13,
		ValidationEnabled = 14,
		DebugEnabled = 15,
		GlobalTaskShaderCullingEnabled = 16,
		VirtualGeometryLodErrorScale = 17,
		VirtualGeometryMaxLodDrop = 18,
		VirtualGeometryHiZOcclusionEnabled = 19,
		VirtualGeometryHiZSelfOcclusionGuardEnabled = 20,
		VirtualGeometryHiZNearPlaneGuardEnabled = 21,
		VirtualGeometryHiZThinGeometryGuardEnabled = 22,
		VirtualGeometryDebugVisualizationMode = 23,
		VirtualGeometryBudgetBytes = 24,
		VirtualGeometryPreloadBias = 25,
		VirtualGeometryLodTransitionHysteresisPixels = 26,
		VirtualGeometryLodTransitionStableFrames = 27,
		VirtualGeometryDeformationCurrentBudgetBytes = 28,
		VirtualGeometryDeformationHistoryBudgetBytes = 29,
		VirtualGeometryDeformationBoundsBudgetBytes = 30,
		TextureBudgetMegabytes = 31,
		ProfileResolutionScale = 32,
		SceneIndirectStrength = 33,
		SceneAmbientOcclusionStrength = 34,
		SceneAmbientOcclusionQuality = 35,
		SkyExposure = 36,
		AntiAliasingMode = 64,
		FixedExposureCompensation = 65,
		InternalResolutionScale = 66,
		ExposureMode = 67,
		AutoExposureMinEV = 68,
		AutoExposureMaxEV = 69,
		AutoExposureBrightenSpeed = 70,
		AutoExposureDarkenSpeed = 71,
		ToneMappingMode = 72,
		ColorGradingEnabled = 73,
		ColorTemperatureKelvin = 74,
		ColorTint = 75,
		ColorContrast = 76,
		ColorSaturation = 77,
		ColorLift = 78,
		ColorGain = 79,
		ColorCurve = 80,
		ColorCurveStrength = 81,
		ColorLut = 82,
		ColorLutStrength = 83,
		BloomEnabled = 84,
		BloomThreshold = 85,
		BloomIntensity = 86,
		BloomSpread = 87,
		BloomSoftKnee = 88,
		UpscalingMode = 89,
		UpscalingSharpness = 90,
		GeometryRasterMode = 91,
		GtaoEnabled = 92,
		GtaoIntensity = 93,
		GtaoQuality = 94,
		ScreenSpaceReflectionsEnabled = 95,
		ScreenSpaceReflectionsIntensity = 96,
		ScreenSpaceReflectionsQuality = 97,
		ScreenSpaceIndirectEnabled = 98,
		ScreenSpaceIndirectIntensity = 99,
		ScreenSpaceIndirectQuality = 100,
		DepthOfFieldEnabled = 104,
		DepthOfFieldFocusDistance = 105,
		DepthOfFieldFocusRange = 106,
		DepthOfFieldBlurStrength = 107,
		MotionBlurEnabled = 108,
		MotionBlurStrength = 109,
		MotionBlurCameraContribution = 110,
		MotionBlurObjectContribution = 111,
		LensDirtEnabled = 112,
		LensDirtIntensity = 113,
		LensDirtTexture = 114,
		LensFlareEnabled = 115,
		LensFlareIntensity = 116,
		LensFlareThreshold = 117,
		VignetteEnabled = 118,
		VignetteIntensity = 119,
		VignetteRadius = 120,
		VignetteSoftness = 121,
		FilmGrainEnabled = 122,
		FilmGrainIntensity = 123,
		FilmGrainResponse = 124,
		ChromaticAberrationEnabled = 125,
		ChromaticAberrationPixels = 126,
		LensDistortionEnabled = 127,
		LensDistortionK1 = 128,
		LensDistortionK2 = 129,
		VolumetricFogEnabled = 130, VolumetricFogDensity = 131,
		VolumetricFogHeightFalloff = 132, VolumetricFogBaseHeight = 133,
		VolumetricFogMaxDistance = 134, SunShaftsEnabled = 135,
		SunShaftsIntensity = 136, OutputDitheringEnabled = 137
	};

	// Stable wire values identify independent coverage, reconstruction and raster
	// choices. A temporal upscaler owns reconstruction while standalone AA is Off.
	enum class GraphicsAntiAliasingMode : std::uint32_t { Off = 0, FXAA = 1, SMAA = 2, CMAA2 = 3, MSAA = 4, TAA = 5 };
	enum class GraphicsUpscalingMode : std::uint32_t { Off = 0, Temporal = 1 };
	enum class GraphicsGeometryRasterMode : std::uint32_t { Automatic = 0, Indexed = 1 };

	// Native typed values retain full-width byte budgets. String and UUID values
	// are owned copies for authored effect parameters and asset references.
	using GraphicsValue = std::variant<bool, std::uint32_t, std::uint64_t,
		float, std::string, TerraEngine::UUID>;

	enum class GraphicsValueKind : std::uint32_t
	{
		Boolean = 0,
		Unsigned32 = 1,
		Unsigned64 = 2,
		Scalar = 3,
		String = 4,
		Asset = 5
	};

	enum class GraphicsScope : std::uint32_t
	{
		Game = 0,
		Camera = 1
	};

	// Values with an authored owner are surfaced by discovery. Their writes
	// continue through the existing scene, sky component, or profile APIs.
	enum class GraphicsAuthoringScope : std::uint32_t
	{
		None = 0,
		Scene = 1,
		SkyComponent = 2,
		BuildProfile = 3
	};

	enum class GraphicsActivation : std::uint32_t
	{
		Live = 0,
		Rebuild = 1,
		NextLaunch = 2,
		ProfileOnly = 3,
		DevelopmentOnly = 4
	};

	enum class GraphicsRequestOperation : std::uint32_t
	{
		Patch = 0,
		Reset = 1,
		Preset = 2,
		StageNextLaunch = 3
	};

	enum class GraphicsRequestState : std::uint32_t
	{
		Submitted = 0,
		Accepted = 1,
		PendingRebuild = 2,
		RestartRequired = 3,
		ScheduledRestart = 4,
		Applied = 5,
		Rejected = 6,
		Cancelled = 7
	};

	enum class GraphicsRequestReason : std::uint32_t
	{
		None = 0,
		InvalidSetting = 1,
		InvalidValue = 2,
		Unsupported = 3,
		InvalidScope = 4,
		StaleLifetime = 5,
		StaleRevision = 6,
		TargetUnavailable = 7,
		AmbiguousView = 8,
		PreparationFailed = 9,
		StorageFailed = 10,
		ConflictingRequest = 11,
		InterruptedStartup = 12,
		ExplicitCancellation = 13,
		RestartRequired = 14,
		RequestCapacityExceeded = 15
	};

	// This tuple is captured at request admission. A renderer resource rebuild
	// can advance independently of project, Play Mode, or renderer replacement.
	struct GraphicsLifecycleStamp
	{
		std::uint64_t projectGeneration = 0;
		std::uint64_t playSessionGeneration = 0;
		std::uint64_t rendererSessionGeneration = 0;
		std::uint64_t backendResourceGeneration = 0;
		bool operator==(const GraphicsLifecycleStamp&) const = default;
	};

	// Camera identity includes the native object lifetime and managed binding
	// generations. It must never be inferred from a recycled output or frame ID.
	struct GraphicsCameraTarget
	{
		TerraEngine::UUID cameraId = TerraEngine::EMPTY_UUID;
		std::uint64_t sceneGeneration = 0;
		std::uint64_t objectLifetimeGeneration = 0;
		std::uint64_t componentGeneration = 0;
		std::uint64_t bindingGeneration = 0;
		bool operator==(const GraphicsCameraTarget&) const = default;
	};

	// A stable published view lifetime is separate from per-frame visibility
	// generations. A caller can select one of several views of the same camera.
	struct GraphicsViewIdentity
	{
		RenderViewType viewType = static_cast<RenderViewType>(0);
		std::uint32_t outputId = 0;
		std::uint64_t lifetimeGeneration = 0;
		// Dedicated captures can share a camera ID with its visible viewport.
		// Keep output kind in the identity so their resources never alias.
		RenderOutputKind outputKind = static_cast<RenderOutputKind>(0);
		bool operator==(const GraphicsViewIdentity&) const = default;
	};

	struct GraphicsTarget
	{
		GraphicsScope scope = GraphicsScope::Game;
		std::optional<GraphicsCameraTarget> camera;
		std::optional<GraphicsViewIdentity> view;
		bool operator==(const GraphicsTarget&) const = default;
	};

	struct GraphicsSettingValue
	{
		GraphicsSettingId id = GraphicsSettingId::Invalid;
		GraphicsValue value = false;
		bool operator==(const GraphicsSettingValue&) const = default;
	};

	// Range and enum entries use the declared value kind. Absence of a bound
	// means that the catalog imposes no bound on that side of the value range.
	struct GraphicsSettingDescriptor
	{
		GraphicsSettingId id = GraphicsSettingId::Invalid;
		std::string name;
		std::string units;
		GraphicsValueKind valueKind = GraphicsValueKind::Boolean;
		GraphicsValue defaultValue = false;
		std::optional<GraphicsValue> minimum;
		std::optional<GraphicsValue> maximum;
		std::vector<GraphicsValue> options;
		bool gameScope = false;
		bool cameraScope = false;
		GraphicsAuthoringScope authoringScope = GraphicsAuthoringScope::None;
		GraphicsActivation activation = GraphicsActivation::Live;
		bool available = false;
		GraphicsRequestReason unavailableReason = GraphicsRequestReason::Unsupported;
		std::string detail;
	};

	// A reset uses the IDs in resetSettings and removes those explicit overrides.
	// A patch owns complete values. The controller rejects duplicate IDs and
	// validates the entire resulting candidate before anything becomes visible.
	struct GraphicsSettingsRequest
	{
		GraphicsRequestOperation operation = GraphicsRequestOperation::Patch;
		GraphicsTarget target{};
		GraphicsLifecycleStamp lifecycle{};
		GraphicsRevision baseRevision = 0;
		std::vector<GraphicsSettingValue> patches;
		std::vector<GraphicsSettingId> resetSettings;
		std::optional<QualityPreset> preset;
	};

	// A receipt identifies one ordered request. Zero request/revision/frame IDs
	// mean no corresponding admission, publication, or rendered frame exists yet.
	struct GraphicsRequestReceipt
	{
		GraphicsRequestId requestId = 0;
		std::uint64_t sequence = 0;
		GraphicsRequestState state = GraphicsRequestState::Submitted;
		GraphicsActivation activation = GraphicsActivation::Live;
		GraphicsRequestReason reason = GraphicsRequestReason::None;
		std::vector<GraphicsSettingId> affectedSettings;
		std::string detail;
		GraphicsRevision requestedRevision = 0;
		GraphicsRevision effectiveRevision = 0;
		std::uint64_t appliedFrameId = 0;
	};

	// Once published, consumers retain a const owning snapshot for the whole
	// frame. The producer may construct another snapshot while readers use it.
	struct GraphicsSettingsSnapshot
	{
		GraphicsTarget target{};
		GraphicsLifecycleStamp lifecycle{};
		GraphicsRevision revision = 0;
		std::uint64_t publishedFrameId = 0;
		std::vector<GraphicsSettingValue> values;
		std::optional<QualityPreset> preset;
		std::uint32_t internalWidth = 0;
		std::uint32_t internalHeight = 0;
		std::uint32_t outputWidth = 0;
		std::uint32_t outputHeight = 0;
		std::uint32_t sampleCount = 1;
		// Only a protected resource owner may certify that this publication's
		// post-process formats and native passes have been prepared.
		std::uint64_t postProcessResourceGeneration = 0;
		// Native preparation owns these insertion bits. Managed values cannot
		// set them through an ordinary graphics settings request.
		std::uint8_t customPostProcessStages = 0u;
	};

	// Preparation is based on one exact snapshot. The commit boundary compares
	// the base revision and lifecycle again before adopting the candidate. Backend
	// resources remain with their renderer owner and are addressed by generation.
	struct GraphicsPreparedCandidate
	{
		GraphicsRequestId requestId = 0;
		std::uint64_t sequence = 0;
		GraphicsRevision baseRevision = 0;
		GraphicsLifecycleStamp lifecycle{};
		GraphicsActivation activation = GraphicsActivation::Live;
		GraphicsSettingsSnapshot snapshot{};
		std::uint64_t preparedResourceGeneration = 0;
	};

	// A named preset is the exact field patch the renderer resolves for that
	// tier. Workers receive a copied catalog alongside the setting descriptors.
	struct GraphicsPresetDefinition
	{
		QualityPreset preset = static_cast<QualityPreset>(0);
		std::vector<GraphicsSettingValue> values;
	};

	// RenderSystem captures these catalog and target views at its protected
	// boundary. The caller keeps the backing arrays immutable and alive for the
	// complete Prepare/Publish call. Workers never resolve a live scene object.
	struct GraphicsPreparationContext
	{
		GraphicsLifecycleStamp lifecycle{};
		std::span<const GraphicsSettingDescriptor> descriptors;
		std::span<const GraphicsTarget> validTargets;
		std::uint64_t frameId = 0;
		std::span<const GraphicsPresetDefinition> presets{};
	};

	// Persistence adapters own one store per game. Its revision supports checked
	// replacements, so staging and claiming cannot silently overwrite each other.
	struct GraphicsStartupTransaction
	{
		GraphicsRequestId requestId = 0;
		GraphicsSettingsRequest request{};
		std::vector<GraphicsSettingValue> fieldPreconditions;
	};

	enum class GraphicsStartupClaimState : std::uint32_t
	{
		Pending = 0,
		Claimed = 1
	};

	struct GraphicsStartupRecord
	{
		GraphicsStartupTransaction transaction{};
		GraphicsStartupClaimState claimState = GraphicsStartupClaimState::Pending;
		std::uint64_t claimGeneration = 0;
	};

	struct GraphicsStartupStoreSnapshot
	{
		std::uint32_t schemaVersion = 1;
		std::uint64_t storeRevision = 0;
		std::vector<GraphicsSettingValue> committedProfile;
		std::optional<GraphicsStartupRecord> pending;
		std::vector<GraphicsRequestReceipt> terminalReceipts;
	};

	struct GraphicsStartupStoreResult
	{
		bool succeeded = false;
		std::uint64_t storeRevision = 0;
		GraphicsRequestReason reason = GraphicsRequestReason::None;
		std::string detail;
	};

	// The startup loader calls Claim before initializing a candidate renderer.
	// Promote atomically replaces the committed profile, clears the claimed
	// record, and writes Applied. Reject/Cancel clear it with the supplied terminal
	// receipt. No implementation may expose a successful result before durability.
	class GraphicsStartupStore
	{
	public:
		virtual ~GraphicsStartupStore() = default;
		virtual GraphicsStartupStoreResult Read(GraphicsStartupStoreSnapshot& snapshot) = 0;
		virtual GraphicsStartupStoreResult Stage(const GraphicsStartupTransaction& transaction,
			std::uint64_t expectedStoreRevision) = 0;
		virtual GraphicsStartupStoreResult Claim(GraphicsRequestId requestId,
			std::uint64_t claimGeneration, std::uint64_t expectedStoreRevision) = 0;
		virtual GraphicsStartupStoreResult Promote(GraphicsRequestId requestId,
			std::uint64_t claimGeneration, std::uint64_t expectedStoreRevision,
			const std::vector<GraphicsSettingValue>& committedProfile,
			const GraphicsRequestReceipt& appliedReceipt) = 0;
		virtual GraphicsStartupStoreResult Reject(GraphicsRequestId requestId,
			std::uint64_t expectedStoreRevision,
			const GraphicsRequestReceipt& rejectedReceipt) = 0;
		virtual GraphicsStartupStoreResult Cancel(GraphicsRequestId requestId,
			std::uint64_t expectedStoreRevision,
			const GraphicsRequestReceipt& cancelledReceipt) = 0;
	};
}
