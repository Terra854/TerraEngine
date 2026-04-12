#pragma once

#include <string>
#include <string_view>

#include "Core/Object/Component.h"

// Lightweight scene label that gives gameplay code a stable, human-readable
// lookup key without forcing every system to understand editor naming rules.
struct TagComponent : public Component<TagComponent>
{
	static constexpr std::string_view kComponentName = "TagComponent";

	// New objects start as "Untagged" so scene queries always have a defined
	// value even before the author assigns a gameplay-specific tag.
	std::string tag = "Untagged";

	static void reflect(rttr::registration::class_<TagComponent>& registration)
	{
		// Expose the authored tag string to reflection so serialization and
		// editor tools can inspect or modify it consistently.
		registration.property("tag", &TagComponent::tag);
	}
};
