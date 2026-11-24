// PostProcessCustomPass.h - pure, immutable fullscreen-effect admission.
#pragma once
#include "Core/Export.h"
#include "Utilities/UUID.h"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

struct Shader;
namespace TerraEngine::Renderer
{
	// Dedicated fullscreen passes read declared sampled inputs and write one
	// distinct color image. Arbitrary graph editing and in-place UAVs are outside
	// this contract; failed admission leaves the prior accepted set untouched.
	enum class CustomPostProcessStage : std::uint32_t { HdrScene, DisplayImage, BeforeUI };
	enum class CustomPostProcessDomain : std::uint32_t { LinearHdr, DisplayLinear };
	enum class CustomPostProcessInputKind : std::uint32_t { SourceColor, Depth, Normals, Roughness, Motion, AssetTexture };
	enum class CustomPostProcessAccess : std::uint32_t { SampledRead, StorageRead, StorageWrite, ReadWrite };
	enum class CustomPostProcessOutputAccess : std::uint32_t { DistinctColorWrite, InPlaceReadWrite };
	enum class CustomPostProcessParameterKind : std::uint32_t { Boolean, Signed, Unsigned, Scalar, Vector2, Vector3, Vector4 };
	using CustomPostProcessValue = std::variant<bool, std::int32_t, std::uint32_t, float,
		std::array<float, 2>, std::array<float, 3>, std::array<float, 4>>;

	struct CustomPostProcessParameterLayout
	{
		std::string name;
		CustomPostProcessParameterKind kind = CustomPostProcessParameterKind::Scalar;
		std::uint32_t offsetBytes = 0;
		bool operator==(const CustomPostProcessParameterLayout&) const = default;
	};
	struct CustomPostProcessParameter
	{
		CustomPostProcessParameterLayout layout;
		CustomPostProcessValue defaultValue = 0.0f;
		std::optional<CustomPostProcessValue> minimum, maximum;
	};
	struct CustomPostProcessParameterValue
	{
		std::string name;
		CustomPostProcessValue value = 0.0f;
	};
	struct CustomPostProcessInput
	{
		CustomPostProcessInputKind kind = CustomPostProcessInputKind::SourceColor;
		CustomPostProcessAccess access = CustomPostProcessAccess::SampledRead;
		std::uint32_t binding = 0;
		bool combinedSampler = false; // Combined-image-sampler or separate sampled image.
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
	};
	struct PostProcessCustomPassDescriptor
	{
		std::uint32_t schemaVersion = 1;
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID shaderAssetId = TerraEngine::EMPTY_UUID;
		std::uint64_t revision = 0;
		std::string stableId, entryPoint = "PSMain";
		CustomPostProcessStage stage = CustomPostProcessStage::HdrScene;
		CustomPostProcessDomain domain = CustomPostProcessDomain::LinearHdr;
		CustomPostProcessOutputAccess outputAccess = CustomPostProcessOutputAccess::DistinctColorWrite;
		std::vector<CustomPostProcessInput> inputs;
		std::optional<std::uint32_t> samplerBinding;
		// Parameters occupy a reflected push-constant block starting at byte0.
		std::uint32_t parameterBytes = 0;
		std::vector<CustomPostProcessParameter> parameters;
		std::vector<std::string> after;
		std::vector<TerraEngine::UUID> assetDependencies;
		std::uint64_t requiredCapabilities = 0;
	};

	struct PostProcessCustomPassRegistration
	{
		PostProcessCustomPassDescriptor descriptor;
		std::shared_ptr<const Shader> shader;
		// TSPF lacks per-member reflection. Import must derive these from the same
		// actual compiled binary; descriptor declarations cannot certify themselves.
		std::vector<CustomPostProcessParameterLayout> reflectedParameters;
		bool enabled = true;
		std::vector<CustomPostProcessParameterValue> values;
	};
	struct CustomPostProcessAvailability
	{
		// Bit(kind) per insertion point. These describe qualified actual inputs,
		// not requested effects. Asset textures additionally require available IDs.
		std::array<std::uint32_t, 3> inputMasks{};
		std::vector<TerraEngine::UUID> availableAssets;
		std::uint64_t supportedCapabilities = 0;
		std::uint32_t maximumParameterBytes = 128;
	};
	struct PreparedPostProcessCustomPass
	{
		PostProcessCustomPassDescriptor descriptor;
		std::shared_ptr<const Shader> shader;
		bool enabled = false;
		// Complete schema-order values, with defaults resolved before publication.
		std::vector<CustomPostProcessParameterValue> values;
	};
	struct PreparedPostProcessCustomPassSet
	{
		// Stable stage/dependency order, tie-broken by stableId, never input order.
		std::vector<PreparedPostProcessCustomPass> passes;
	};
	enum class CustomPostProcessFailure : std::uint32_t
	{
		None, InvalidDescriptor, DuplicateIdentity, MissingShader, InterfaceMismatch,
		InvalidParameter, MissingInput, MissingDependency, InvalidOrdering,
		UnsupportedCapability, AllocationFailed
	};
	struct CustomPostProcessDiagnostic
	{
		CustomPostProcessFailure reason = CustomPostProcessFailure::None;
		// Diagnostics remain writable even when candidate allocation fails.
		std::array<char, 129> stableId{};
		std::array<char, 257> detail{};
	};

	// Worker-safe: consumes only copied declarations and immutable asset owners.
	// Only the caller's protected boundary may publish the returned accepted root.
	// This validates CPU contracts; it does not qualify a Vulkan pipeline or claim
	// native shader execution, asset export, managed control, or timing support.
	[[nodiscard]] TERRAENGINE_API bool TryPreparePostProcessCustomPasses(
		std::span<const PostProcessCustomPassRegistration> registrations,
		const CustomPostProcessAvailability& availability,
		std::shared_ptr<const PreparedPostProcessCustomPassSet>& accepted,
		CustomPostProcessDiagnostic& diagnostic) noexcept;

	// Packaging validates detached bytes and dependency order. It has no
	// renderer publication, resource view or device capability to certify.
	[[nodiscard]] TERRAENGINE_API bool ValidateDetachedPostProcessCustomPassAssets(
		std::span<const PostProcessCustomPassRegistration> registrations,
		CustomPostProcessDiagnostic& diagnostic) noexcept;
}
