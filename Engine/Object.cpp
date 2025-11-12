#include "Object.h"

Object::Object(entt::registry& registry)
    : registry(&registry)
{
    id = this->registry->create();
}

