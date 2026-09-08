// NavigationSourceComponent.h - durable, provider-neutral ground source authoring.
//
// Rendering visibility is intentionally not part of physical eligibility. The
// navigation worker consumes immutable source snapshots captured from authored
// geometry, while runtime requests and provider handles remain elsewhere.
#pragma once

#include "Core/Object/Component.h"
#include "Navigation/NavigationTypes.h"
#include "Utilities/UUID.h"

#include <cmath>
#include <cstdint>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

namespace TerraEngine::Navigation
{
	enum class NavigationSourceValidationCode : std::uint8_t
	{
		None,
		InvalidKind,
		InvalidDeduplicationGroup,
		DuplicateSelection
	};

	// Test and diagnostic helpers need a printable stable code; serialization
	// continues to use the numeric enum value through the reflected component.
	inline std::ostream& operator<<(std::ostream& stream, NavigationSourceValidationCode code)
	{
		return stream << static_cast<std::uint32_t>(code);
	}

	struct NavigationSourceValidationResult
	{
		bool accepted = false;
		NavigationSourceValidationCode code = NavigationSourceValidationCode::None;
		std::string message;
	};
}

struct NavigationSourceComponent final : public Component<NavigationSourceComponent>
{
	static constexpr std::string_view kComponentName = "NavigationSourceComponent";

	TerraEngine::UUID sourceId = TerraEngine::GenerateUUID();
	bool enabled = true;
	TerraEngine::Navigation::NavigationSourceKind sourceKind = TerraEngine::Navigation::NavigationSourceKind::Auto;
	TerraEngine::UUID sourceAssetId = TerraEngine::EMPTY_UUID;
	std::uint32_t subshapeSelection = 0U;
	std::uint32_t inclusionMask = 0xFFFFFFFFU;
	std::uint32_t areaId = 0U;
	std::uint32_t deduplicationGroup = 1U;
	std::uint64_t geometryRevision = 1U;
	bool renderVisible = true;

	[[nodiscard]] TerraEngine::Navigation::NavigationSourceValidationResult Validate() const
	{
		using SourceKind = TerraEngine::Navigation::NavigationSourceKind;
		// Procedural geometry uses the same runtime snapshot vocabulary, but it is
		// supplied by a typed producer rather than persisted as this scene source.
		if(!(sourceKind == SourceKind::Auto || sourceKind == SourceKind::Mesh
			|| sourceKind == SourceKind::Collider || sourceKind == SourceKind::Terrain))
			return {false, TerraEngine::Navigation::NavigationSourceValidationCode::InvalidKind, "Unknown navigation source kind."};
		if(enabled && deduplicationGroup == 0U)
			return {false, TerraEngine::Navigation::NavigationSourceValidationCode::InvalidDeduplicationGroup, "Enabled sources require a non-zero deduplication group."};
		return {true, TerraEngine::Navigation::NavigationSourceValidationCode::None, {}};
	}

	template<typename SourceRange>
	[[nodiscard]] static TerraEngine::Navigation::NavigationSourceValidationResult ValidateSelection(const SourceRange& sources)
	{
		std::span<const NavigationSourceComponent> view{sources};
		for(std::size_t index = 0; index < view.size(); ++index)
		{
			if(!view[index].Validate().accepted)
				return view[index].Validate();
			if(!view[index].enabled) continue;
			for(std::size_t prior = 0; prior < index; ++prior)
				if(view[prior].enabled && view[prior].deduplicationGroup == view[index].deduplicationGroup)
					return {false, TerraEngine::Navigation::NavigationSourceValidationCode::DuplicateSelection, "Only one enabled physical representation may occupy a deduplication group."};
		}
		return {true, TerraEngine::Navigation::NavigationSourceValidationCode::None, {}};
	}

	// A renderer may hide an object without removing its physical ground source.
	[[nodiscard]] bool IsPhysicallyEligible() const noexcept { return enabled; }

	[[nodiscard]] std::uint32_t GetSourceKindValue() const noexcept { return static_cast<std::uint32_t>(sourceKind); }
	void SetSourceKindValue(std::uint32_t value) noexcept { sourceKind = static_cast<TerraEngine::Navigation::NavigationSourceKind>(value); }

	static void reflect(entt::meta_factory<NavigationSourceComponent>& factory)
	{
		TerraEngine::Reflection::RegisterDataProperty<&NavigationSourceComponent::sourceId>(factory, "sourceId");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationSourceComponent::enabled>(factory, "enabled");
		TerraEngine::Reflection::RegisterAccessorProperty<&NavigationSourceComponent::SetSourceKindValue, &NavigationSourceComponent::GetSourceKindValue>(factory, "sourceKind");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationSourceComponent::sourceAssetId>(factory, "sourceAssetId");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationSourceComponent::subshapeSelection>(factory, "subshapeSelection");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationSourceComponent::inclusionMask>(factory, "inclusionMask");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationSourceComponent::areaId>(factory, "areaId");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationSourceComponent::deduplicationGroup>(factory, "deduplicationGroup");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationSourceComponent::geometryRevision>(factory, "geometryRevision");
	}
};
