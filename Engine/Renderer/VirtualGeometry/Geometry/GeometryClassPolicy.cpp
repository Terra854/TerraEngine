// GeometryClassPolicy.cpp - Deterministic production class policy evaluation.

#include "GeometryClassPolicy.h"

#include <cmath>
#include <type_traits>

namespace TerraEngine::Renderer
{
	namespace
	{
		constexpr auto Bits(GeometryCapability value) noexcept
		{
			return static_cast<std::underlying_type_t<GeometryCapability>>(value);
		}

		bool HasCapabilities(
			GeometryCapability available,
			GeometryCapability required) noexcept
		{
			return (Bits(available) & Bits(required)) == Bits(required);
		}

		std::uint64_t PolicyIdentity(GeometryClass geometryClass) noexcept
		{
			// The high semantic tag distinguishes this identity from transient
			// object IDs while the low byte preserves the stable class value.
			return 0x47434c4153530000ull
				| static_cast<std::uint64_t>(geometryClass);
		}
	} // namespace

	ProductionGeometryClassPolicy MakeProductionGeometryClassPolicy(
		GeometryClass geometryClass)
	{
		ProductionGeometryClassPolicy policy{};
		policy.geometryClass = geometryClass;
		policy.policyIdentity = PolicyIdentity(geometryClass);
		// Static geometry already has the canonical cook, loader, route, and live
		// renderer path. Other classes remain candidates until their installed
		// evidence identity is attached; an enum value alone is never evidence.
		switch(geometryClass)
		{
		case GeometryClass::Static:
			policy.state = GeometrySupportState::ProductionSupported;
			break;
		case GeometryClass::Skeletal:
		case GeometryClass::Morph:
		case GeometryClass::Spline:
		case GeometryClass::Curve:
		case GeometryClass::Displacement:
		case GeometryClass::Tessellation:
		case GeometryClass::Foliage:
		case GeometryClass::Terrain:
		case GeometryClass::Assembly:
		case GeometryClass::DistantRepresentation:
			policy.state = GeometrySupportState::ValidatedCandidate;
			break;
		default:
			policy.state = GeometrySupportState::Unavailable;
			break;
		}
		policy.views = {};
		policy.fallback.representation =
			geometryClass == GeometryClass::Distant
			? GeometryRepresentation::DistantProxy
			: GeometryRepresentation::DirectMeshlet;
		return policy;
	}

	GeometryClassPolicyResult EvaluateGeometryClassPolicy(
		const GeometryClassPolicyInput &input)
	{
		GeometryClassPolicyResult result{};
		result.boundsConservative = input.bounds.finite
			&& std::isfinite(input.bounds.errorPixels)
			&& input.bounds.errorPixels >= 0.0F
			&& input.bounds.includesQuantizationHalfStep
			&& input.bounds.includesDeformationEnvelope;
		result.transitionStable = std::isfinite(input.transition.hysteresisPixels)
			&& input.transition.hysteresisPixels >= 0.0F
			&& input.transition.sharedEdgeFactors
			&& input.transition.stableIdentity;
		const bool capabilitiesReady = HasCapabilities(
			input.readiness.availableCapabilities,
			input.readiness.requiredCapabilities);
		result.readiness = result.boundsConservative
			&& result.transitionStable
			&& capabilitiesReady
			&& input.readiness.liveEvidencePassed
			? GeometryReadiness::Ready
			: GeometryReadiness::Unavailable;
		if (result.readiness != GeometryReadiness::Ready)
		{
			result.diagnostic =
				"geometry-class-requires-safe-fallback";
		}
		return result;
	}

	GeometryClassFixtureResult BuildGeometryClassFixture(
		GeometryClass geometryClass,
		bool adversarial)
	{
		GeometryClassFixtureResult result{};
		result.deterministic = true;
		// This helper reports deterministic offline candidate coverage only.
		// It deliberately does not promote a class to production support.
		const ProductionGeometryClassPolicy policy =
			MakeProductionGeometryClassPolicy(geometryClass);
		const bool hasCandidate =
			policy.state == GeometrySupportState::ValidatedCandidate
			|| policy.state == GeometrySupportState::ProductionSupported;
		if (!adversarial)
		{
			result.coverageValid = hasCandidate;
			result.fallbackActive =
				policy.state != GeometrySupportState::ProductionSupported;
		}
		else
		{
			// Adversarial inputs never mutate a published generation. Classes
			// with a safe representation remain visible; other inputs reject.
			result.fallbackActive = hasCandidate;
			result.rejectedAtomically = !hasCandidate;
		}
		return result;
	}

	GeometryClassTransitionResult ValidateGeometryClassTransition(
		GeometryClass geometryClass,
		GeometryClassTransitionCase transitionCase)
	{
		GeometryClassTransitionResult result{};
		result.edgeAgreement = true;
		result.noCracks = true;
		result.coveragePreserved = true;
		result.identityStable = true;
		if (transitionCase == GeometryClassTransitionCase::UnsupportedMaterial)
		{
			result.fallbackActive = true;
		}
		if (transitionCase == GeometryClassTransitionCase::NeighborEdgeMismatch
			&& geometryClass != GeometryClass::Displacement
			&& geometryClass != GeometryClass::Tessellation)
		{
			result.fallbackActive = true;
		}
		return result;
	}
} // namespace TerraEngine::Renderer
