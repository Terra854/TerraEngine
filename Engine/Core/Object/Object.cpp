#include "Object.h"

Object::Object(entt::registry& registry)
	: registry(&registry)
{
	id = this->registry->create();
}

Object::Object(entt::registry& registry, entt::entity existingId)
	: registry(&registry)
	, id(existingId)
{
}


