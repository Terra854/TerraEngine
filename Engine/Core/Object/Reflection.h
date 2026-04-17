#pragma once

#include <entt/entt.hpp>

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace TerraEngine::Reflection
{
	struct DisplayName
	{
		std::string value;
	};

	using Variant = entt::meta_any;
	using Property = entt::meta_data;
	using Type = entt::meta_type;

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

	inline std::string ResolveDisplayName(const entt::meta_custom& custom, std::string_view fallback = {})
	{
		if (auto* name = custom.operator DisplayName *(); name != nullptr)
		{
			return name->value;
		}

		return std::string(fallback);
	}

	template <typename ReflectedType>
	void ConfigureTypeMetadata(entt::meta_factory<ReflectedType>& factory, std::string_view name)
	{
		factory.type(HashIdentifier(name)).template custom<DisplayName>(std::string(name));
	}

	template <auto Data, typename ReflectedType>
	void RegisterDataProperty(entt::meta_factory<ReflectedType>& factory, std::string_view name)
	{
		factory.template data<Data>(HashIdentifier(name)).template custom<DisplayName>(std::string(name));
	}

	template <auto Setter, auto Getter, typename ReflectedType>
	void RegisterAccessorProperty(entt::meta_factory<ReflectedType>& factory, std::string_view name)
	{
		factory.template data<Setter, Getter>(HashIdentifier(name)).template custom<DisplayName>(std::string(name));
	}

	template <typename ReflectedType>
	Type ResolveType()
	{
		return entt::resolve<std::remove_cv_t<std::remove_reference_t<ReflectedType>>>();
	}

	inline Type ResolveType(std::string_view name)
	{
		return entt::resolve(HashIdentifier(name));
	}

	inline bool TypesEqual(const Type& lhs, const Type& rhs) noexcept
	{
		if (!IsValid(lhs) || !IsValid(rhs))
		{
			return IsValid(lhs) == IsValid(rhs);
		}

		return lhs.info().hash() == rhs.info().hash();
	}

	template <typename ReflectedType>
	bool IsType(const Type& value)
	{
		using NormalizedType = std::remove_cv_t<std::remove_reference_t<ReflectedType>>;
		return IsValid(value) && value.info().hash() == entt::type_id<NormalizedType>().hash();
	}

	inline std::string GetTypeName(const Type& value)
	{
		if (!IsValid(value))
		{
			return {};
		}

		return ResolveDisplayName(value.custom(), value.info().name());
	}

	inline std::vector<Property> GetProperties(const Type& value)
	{
		std::vector<Property> properties;
		if (!IsValid(value))
		{
			return properties;
		}

		for (auto&& [id, data] : value.data())
		{
			(void)id;
			properties.emplace_back(data);
		}

		return properties;
	}

	inline Property FindProperty(const Type& value, std::string_view name)
	{
		return IsValid(value) ? value.data(HashIdentifier(name)) : Property{};
	}

	inline std::string GetPropertyName(const Property& value)
	{
		return ResolveDisplayName(value.custom());
	}

	inline Type GetPropertyType(const Property& value)
	{
		return IsValid(value) ? value.type() : Type{};
	}

	template <typename InstanceType>
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
	bool SetPropertyValue(const Property& value, InstanceType& instance, const Variant& incoming)
	{
		if (!IsValid(value))
		{
			return false;
		}

		Variant handle = entt::forward_as_meta(instance);
		if (value.set(entt::meta_handle{ handle }, incoming))
		{
			return true;
		}

		Variant converted = incoming.allow_cast(value.type());
		return IsValid(converted) ? value.set(entt::meta_handle{ handle }, converted) : false;
	}

	template <typename ReflectedType>
	bool IsType(const Variant& value)
	{
		using NormalizedType = std::remove_cv_t<std::remove_reference_t<ReflectedType>>;
		return IsValid(value) && value.type().info().hash() == entt::type_id<NormalizedType>().hash();
	}

	template <typename ReflectedType>
	ReflectedType GetValue(const Variant& value)
	{
		using NormalizedType = std::remove_cv_t<std::remove_reference_t<ReflectedType>>;

		if (const auto* instance = value.template try_cast<const NormalizedType>(); instance != nullptr)
		{
			return *instance;
		}

		if (const auto* instance = value.template try_cast<NormalizedType>(); instance != nullptr)
		{
			return *instance;
		}

		return NormalizedType{};
	}

	template <typename ReflectedType>
	ReflectedType ConvertValue(const Variant& value)
	{
		using NormalizedType = std::remove_cv_t<std::remove_reference_t<ReflectedType>>;

		if (const auto* instance = value.template try_cast<const NormalizedType>(); instance != nullptr)
		{
			return *instance;
		}

		if (const auto* instance = value.template try_cast<NormalizedType>(); instance != nullptr)
		{
			return *instance;
		}

		Variant converted = value.template allow_cast<NormalizedType>();
		return IsValid(converted) ? converted.template cast<NormalizedType>() : NormalizedType{};
	}
}
