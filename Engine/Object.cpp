#include "Object.h"

Object::Object(entt::registry& registry)
{
    id = registry.create();
}