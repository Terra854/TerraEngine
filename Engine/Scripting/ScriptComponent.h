#pragma once

#include "Core/Object/Component.h"
#include <string>

/**
 * @class ScriptComponent
 * @brief Attaches a C# script to an entity.
 */
class ScriptComponent : public Component<ScriptComponent> {
public:
    static constexpr std::string_view kComponentName = "Script";

    ScriptComponent() = default;
    
    // The fully qualified name of the C# class (e.g. "MyGame.PlayerController")
    std::string className;

    static void reflect(rttr::registration::class_<ScriptComponent>& registration)
    {
        registration.property("ClassName", &ScriptComponent::className);
    }
};
