// MemoryStrategy.cpp - Process-start parsing for independently selectable domains.

#include "MemoryStrategy.h"

#include <cstdlib>
#include <optional>

namespace TerraEngine::Memory {

	namespace {
		std::optional<MemoryDomain> ParseDomain(const std::string_view name) noexcept
		{
			if (name == "animation") return MemoryDomain::AnimationBinding;
			if (name == "physics") return MemoryDomain::PhysicsPreparation;
			if (name == "renderer") return MemoryDomain::RendererPreparation;
			if (name == "scene") return MemoryDomain::SceneTransaction;
			return std::nullopt;
		}
	}

	MemoryStrategyConfiguration::MemoryStrategyConfiguration() noexcept
	{
		strategies_.fill(MemoryStrategy::Standard);
	}

	MemoryStrategyConfiguration MemoryStrategyConfiguration::Parse(const std::string_view text) noexcept
	{
		MemoryStrategyConfiguration result;
		std::size_t begin = 0U;
		while (begin < text.size()) {
			const std::size_t end = text.find(',', begin);
			const std::string_view item = text.substr(
				begin, end == std::string_view::npos ? text.size() - begin : end - begin);
			const std::size_t equals = item.find('=');
			if (equals != std::string_view::npos) {
				const std::optional<MemoryDomain> domain = ParseDomain(item.substr(0U, equals));
				const std::string_view strategy = item.substr(equals + 1U);
				if (domain && strategy == "reusable-arena") {
					result.strategies_[static_cast<std::size_t>(*domain)] = MemoryStrategy::ReusableArena;
				}
			}
			if (end == std::string_view::npos) break;
			begin = end + 1U;
		}
		return result;
	}

	MemoryStrategy MemoryStrategyConfiguration::For(const MemoryDomain domain) const noexcept
	{
		return strategies_[static_cast<std::size_t>(domain)];
	}

	std::string_view MemoryStrategyConfiguration::NameFor(const MemoryDomain domain) const noexcept
	{
		return For(domain) == MemoryStrategy::ReusableArena ? "reusable-arena" : "standard";
	}

	const MemoryStrategyConfiguration& ActiveMemoryStrategies() noexcept
	{
		static const MemoryStrategyConfiguration configuration = [] {
			const char* text = std::getenv("TERRAENGINE_MEMORY_STRATEGIES");
			return MemoryStrategyConfiguration::Parse(text == nullptr ? std::string_view{} : std::string_view{text});
		}();
		return configuration;
	}

} // namespace TerraEngine::Memory
