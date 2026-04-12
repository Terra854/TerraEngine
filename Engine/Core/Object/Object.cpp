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

std::string Object::getName() const
{
	return ObjectManager::instance().getObjectName(*this);
}

void Object::setName(const std::string& name)
{
	ObjectManager::instance().setObjectName(*this, name);
}

std::string Object::getTag() const
{
	return ObjectManager::instance().getObjectTag(*this);
}

void Object::setTag(const std::string& tag)
{
	ObjectManager::instance().setObjectTag(*this, tag);
}

std::string Object::getLayer() const
{
	return ObjectManager::instance().getObjectLayer(*this);
}

void Object::setLayer(const std::string& layer)
{
	ObjectManager::instance().setObjectLayer(*this, layer);
}

bool Object::isActive() const
{
	return ObjectManager::instance().isObjectActive(*this);
}

void Object::setActive(bool active)
{
	ObjectManager::instance().setObjectActive(*this, active);
}

bool Object::isStatic() const
{
	return ObjectManager::instance().isObjectStatic(*this);
}

void Object::setStatic(bool isStatic)
{
	ObjectManager::instance().setObjectStatic(*this, isStatic);
}


