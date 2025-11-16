#pragma once

#include <rttr/registration>

#include <string>
#include <type_traits>
#include <utility>

// Helper utilities that keep the CRTP component base lightweight while still
// allowing each component to expose metadata to RTTR in a consistent way.
namespace te::component_detail
{
    template <typename T, typename = void>
    struct has_custom_name : std::false_type
    {
    };

    template <typename T>
    struct has_custom_name<T, std::void_t<decltype(T::kComponentName)>> : std::true_type
    {
    };

    template <typename T, typename = void>
    struct has_reflect : std::false_type
    {
    };

    template <typename T>
    struct has_reflect<T, std::void_t<decltype(T::reflect(std::declval<rttr::registration::class_<T>&>()))>> : std::true_type
    {
    };

    template <typename T>
    inline constexpr bool has_reflect_v = has_reflect<T>::value;

    template <typename T>
    inline std::string component_name()
    {
        if constexpr (has_custom_name<T>::value)
        {
            return std::string(T::kComponentName);
        }
        else
        {
            return rttr::type::get<T>().get_name().to_string();
        }
    }
}

// Base component that all other components will inherit via CRTP
template <typename Derived>
class Component
{
public:
    Component()
    {
        registerType();
    }

    virtual ~Component() = default;

    Component(const Component&) = default;
    Component(Component&&) noexcept = default;
    Component& operator=(const Component&) = default;
    Component& operator=(Component&&) noexcept = default;

    static void registerType()
    {
        ensureRegistration();
    }

protected:
    // Ensures that the derived component registers itself with RTTR exactly
    // once, regardless of how many instances are constructed at runtime.
    static void ensureRegistration()
    {
        static bool registered = []() {
            const std::string name = te::component_detail::component_name<Derived>();
            auto registration = rttr::registration::class_<Derived>(name.c_str()).template constructor<>();
            if constexpr (te::component_detail::has_reflect_v<Derived>)
            {
                Derived::reflect(registration);
            }
            return true;
        }();
        (void)registered;
    }
};

