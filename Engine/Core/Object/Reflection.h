// Reflection.h — Wrapper and utilities for the EnTT-based meta-programming system.
//
// This file provides a simplified and consistent interface for the engine's
// reflection layer, allowing types and properties to be registered and queried
// at runtime. It includes helpers for property access, type conversion, and
// metadata management, serving as the foundation for the editor and serialization.

#pragma once

#include <entt/entt.hpp>

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace TerraEngine::Reflection
{
	// struct DisplayName groups related state and operations so callers have one documented contract.
	struct DisplayName
	{
		std::string value;
	};

	using Variant = entt::meta_any;
	using Property = entt::meta_data;
	using Type = entt::meta_type;

	// HashIdentifier is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	inline entt::id_type HashIdentifier(std::string_view text)
	{
		return entt::hashed_string::value(text.data(), text.size());
	}

	inline bool IsValid(const Variant& value) noexcept
	{
		return static_cast<bool>(value);
	}

	inline bool IsValid(const Property& value) noexcept
	{
		return static_cast<bool>(value);
	}

	inline bool IsValid(const Type& value) noexcept
	{
		return static_cast<bool>(value);
	}

	// ResolveDisplayName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	inline std::string ResolveDisplayName(const entt::meta_custom& custom, std::string_view fallback = {})
	{
		if (auto* name = custom.operator DisplayName *(); name != nullptr)
		{
			return name->value;
		}

		return std::string(fallback);
	}

	template <typename ReflectedType>
	// ConfigureTypeMetadata is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void ConfigureTypeMetadata(entt::meta_factory<ReflectedType>& factory, std::string_view name)
	{
		factory.type(HashIdentifier(name)).template custom<DisplayName>(std::string(name));
	}

	template <auto Data, typename ReflectedType>
	// RegisterDataProperty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void RegisterDataProperty(entt::meta_factory<ReflectedType>& factory, std::string_view name)
	{
		factory.template data<Data>(HashIdentifier(name)).template custom<DisplayName>(std::string(name));
	}

	template <auto Setter, auto Getter, typename ReflectedType>
	// RegisterAccessorProperty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void RegisterAccessorProperty(entt::meta_factory<ReflectedType>& factory, std::string_view name)
	{
		factory.template data<Setter, Getter>(HashIdentifier(name)).template custom<DisplayName>(std::string(name));
	}

	template <typename ReflectedType>
	// ResolveType is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	Type ResolveType()
	{
		return entt::resolve<std::remove_cv_t<std::remove_reference_t<ReflectedType>>>();
	}

	// ResolveType is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	inline Type ResolveType(std::string_view name)
	{
		return entt::resolve(HashIdentifier(name));
	}

	inline bool TypesEqual(const Type& lhs, const Type& rhs) noexcept
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!IsValid(lhs) || !IsValid(rhs))
		{
			return IsValid(lhs) == IsValid(rhs);
		}

		return lhs.info().hash() == rhs.info().hash();
	}

	template <typename ReflectedType>
	// IsType is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool IsType(const Type& value)
	{
		using NormalizedType = std::remove_cv_t<std::remove_reference_t<ReflectedType>>;
		return IsValid(value) && value.info().hash() == entt::type_id<NormalizedType>().hash();
	}

	// GetTypeName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	inline std::string GetTypeName(const Type& value)
	{
		if (!IsValid(value))
		{
			return {};
		}

		return ResolveDisplayName(value.custom(), value.info().name());
	}

	// GetProperties is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	inline std::vector<Property> GetProperties(const Type& value)
	{
		std::vector<Property> properties;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!IsValid(value))
		{
			return properties;
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto&& [id, data] : value.data())
		{
			(void)id;
			properties.emplace_back(data);
		}

		return properties;
	}

	// FindProperty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	inline Property FindProperty(const Type& value, std::string_view name)
	{
		return IsValid(value) ? value.data(HashIdentifier(name)) : Property{};
	}

	// GetPropertyName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	inline std::string GetPropertyName(const Property& value)
	{
		return ResolveDisplayName(value.custom());
	}

	// GetPropertyType is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	inline Type GetPropertyType(const Property& value)
	{
		return IsValid(value) ? value.type() : Type{};
	}

	template <typename InstanceType>
	// GetPropertyValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	Variant GetPropertyValue(const Property& value, InstanceType& instance)
	{
		if (!IsValid(value))
		{
			return {};
		}

		Variant handle = entt::forward_as_meta(instance);
		return value.get(entt::meta_handle{ handle });
	}

	template <typename InstanceType>
	// SetPropertyValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool SetPropertyValue(const Property& value, InstanceType& instance, const Variant& incoming)
	{
		if (!IsValid(value))
		{
			return false;
		}

		Variant handle = entt::forward_as_meta(instance);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (value.set(entt::meta_handle{ handle }, incoming))
		{
			return true;
		}

		Variant converted = incoming.allow_cast(value.type());
		return IsValid(converted) ? value.set(entt::meta_handle{ handle }, converted) : false;
	}

	template <typename ReflectedType>
	// IsType is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool IsType(const Variant& value)
	{
		using NormalizedType = std::remove_cv_t<std::remove_reference_t<ReflectedType>>;
		return IsValid(value) && value.type().info().hash() == entt::type_id<NormalizedType>().hash();
	}

	template <typename ReflectedType>
	// GetValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	ReflectedType GetValue(const Variant& value)
	{
		using NormalizedType = std::remove_cv_t<std::remove_reference_t<ReflectedType>>;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (const auto* instance = value.template try_cast<const NormalizedType>(); instance != nullptr)
		{
			return *instance;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (const auto* instance = value.template try_cast<NormalizedType>(); instance != nullptr)
		{
			return *instance;
		}

		return NormalizedType{};
	}

	template <typename ReflectedType>
	// ConvertValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	ReflectedType ConvertValue(const Variant& value)
	{
		using NormalizedType = std::remove_cv_t<std::remove_reference_t<ReflectedType>>;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (const auto* instance = value.template try_cast<const NormalizedType>(); instance != nullptr)
		{
			return *instance;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (const auto* instance = value.template try_cast<NormalizedType>(); instance != nullptr)
		{
			return *instance;
		}

		Variant converted = value.template allow_cast<NormalizedType>();
		return IsValid(converted) ? converted.template cast<NormalizedType>() : NormalizedType{};
	}
}
