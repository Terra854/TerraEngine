// MemoryStrategy.h - Disabled-by-default policy for measured memory experiments.

#pragma once

#include "Core/Export.h"

#include <array>
#include <string_view>

namespace TerraEngine::Memory {

	enum class MemoryDomain : std::size_t {
		AnimationBinding,
		PhysicsPreparation,
		RendererPreparation,
		SceneTransaction,
		Count
	};

	enum class MemoryStrategy {
		Standard,
		ReusableArena
	};

	// Configuration is immutable after process startup. The benchmark sets the
	// environment before launching the editor, so hot paths perform only an array
	// lookup and never parse strings or allocate memory.
	class TERRAENGINE_API MemoryStrategyConfiguration {
	public:
		MemoryStrategyConfiguration() noexcept;

		[[nodiscard]] static MemoryStrategyConfiguration Parse(std::string_view text) noexcept;
		[[nodiscard]] MemoryStrategy For(MemoryDomain domain) const noexcept;
		[[nodiscard]] std::string_view NameFor(MemoryDomain domain) const noexcept;

	private:
		std::array<MemoryStrategy, static_cast<std::size_t>(MemoryDomain::Count)> strategies_{};
	};

	[[nodiscard]] TERRAENGINE_API const MemoryStrategyConfiguration& ActiveMemoryStrategies() noexcept;

} // namespace TerraEngine::Memory
