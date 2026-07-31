// GeometryClassPolicy.h - Production contracts for generated geometry classes.
//
// Policies describe truthfully supported views, routes, bounds, transitions,
// and fallbacks. Builders may prepare candidates on workers, but support is
// advertised only after the immutable evidence represented here is complete.

#pragma once

#include "Core/Export.h"
#include "Renderer/VirtualGeometry/VirtualGeometryTypes.h"

#include <cstdint>
#include <string>

namespace TerraEngine::Renderer
{
	using GeometrySupportState = GeometryClassSupportState;

	enum class GeometryRepresentation : std::uint8_t
	{
		None = 0u,
		DirectMeshlet = 1u,
		VirtualGeometry = 2u,
		GeneratedMeshlet = 3u,
		DistantProxy = 4u
	};

	enum class GeometryReadiness : std::uint8_t
	{
		Unavailable = 0u,
		Candidate = 1u,
		Ready = 2u
	};

	enum class GeometryCapability : std::uint32_t
	{
		None = 0u,
		Vulkan = 1u << 0u,
		MeshShader = 1u << 1u,
		Compute = 1u << 2u
	};

	struct GeometryViewContract
	{
		bool camera = true;
		bool shadow = true;
		bool capture = true;
		bool picking = true;
		bool editor = true;
	};

	struct GeometryFallbackContract
	{
		GeometryRepresentation representation = GeometryRepresentation::DirectMeshlet;
		std::string reason = "safe-direct-representation";
	};

	struct GeometryBoundsContractInput
	{
		float errorPixels = 0.0F;
		bool finite = true;
		bool includesQuantizationHalfStep = true;
		bool includesDeformationEnvelope = true;
	};

	struct GeometryTransitionContractInput
	{
		float hysteresisPixels = 1.0F;
		bool sharedEdgeFactors = true;
		bool stableIdentity = true;
	};

	struct GeometryReadinessInput
	{
		GeometryCapability requiredCapabilities = GeometryCapability::Vulkan;
		GeometryCapability availableCapabilities = GeometryCapability::Vulkan;
		// Readiness is fail-closed. Offline builders and unit tests may establish
		// a validated candidate, but only installed live evidence may set this.
		bool liveEvidencePassed = false;
	};

	struct GeometryClassPolicyInput
	{
		GeometryClass geometryClass = GeometryClass::Static;
		GeometryBoundsContractInput bounds{};
		GeometryTransitionContractInput transition{};
		GeometryReadinessInput readiness{};
	};

	struct ProductionGeometryClassPolicy
	{
		GeometryClass geometryClass = GeometryClass::Static;
		GeometrySupportState state = GeometrySupportState::Unavailable;
		GeometryViewContract views{};
		GeometryFallbackContract fallback{};
		std::uint64_t policyIdentity = 0u;
		std::uint32_t policyVersion = 1u;
	};

	struct GeometryClassPolicyResult
	{
		bool boundsConservative = false;
		bool transitionStable = false;
		GeometryReadiness readiness = GeometryReadiness::Unavailable;
		std::string diagnostic;
	};

	enum class GeometryClassTransitionCase : std::uint8_t
	{
		Ordinary = 0u,
		NeighborEdgeMismatch = 1u,
		RepeatedParts = 2u,
		UnsupportedMaterial = 3u
	};

	struct GeometryClassFixtureResult
	{
		bool deterministic = false;
		bool coverageValid = false;
		bool fallbackActive = false;
		bool rejectedAtomically = false;
	};

	struct GeometryClassTransitionResult
	{
		bool edgeAgreement = false;
		bool noCracks = false;
		bool coveragePreserved = false;
		bool identityStable = false;
		bool fallbackActive = false;
	};

	[[nodiscard]] TERRAENGINE_API ProductionGeometryClassPolicy
		MakeProductionGeometryClassPolicy(GeometryClass geometryClass);

	[[nodiscard]] TERRAENGINE_API GeometryClassPolicyResult
		EvaluateGeometryClassPolicy(const GeometryClassPolicyInput &input);

	[[nodiscard]] TERRAENGINE_API GeometryClassFixtureResult
		BuildGeometryClassFixture(GeometryClass geometryClass, bool adversarial);

	[[nodiscard]] TERRAENGINE_API GeometryClassTransitionResult
		ValidateGeometryClassTransition(
			GeometryClass geometryClass,
			GeometryClassTransitionCase transitionCase);
} // namespace TerraEngine::Renderer
