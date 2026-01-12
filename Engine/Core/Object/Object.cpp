#include "Object.h"
#include "ObjectManager.h"

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

TerraEngine::UUID Object::getUUID() const
{
	return ObjectManager::instance().getUUID(id);
}


