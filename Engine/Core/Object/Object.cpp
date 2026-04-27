// Object.cpp — Implementation of the handle-based object interface for the ECS.
//
// This file implements the Object class methods, which provide a high-level,
// user-friendly interface for manipulating EnTT entities. It coordinates with
// the ObjectManager to handle common tasks such as naming, tagging, active
// state management, and component access.

#include "Object.h"
#include "ObjectManager.h"

Object::Object(entt::registry& registry)
	: registry(&registry)
{
	id = this->registry->create();
}

Object::Object(entt::registry& registry, entt::entity existingId)
	// registry is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: registry(&registry)
	, id(existingId)
{
}

// Object::getUUID is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::UUID Object::getUUID() const
{
	return ObjectManager::instance().getUUID(id);
}

// Object::getName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string Object::getName() const
{
	return ObjectManager::instance().getObjectName(*this);
}

// Object::setName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Object::setName(const std::string& name)
{
	ObjectManager::instance().setObjectName(*this, name);
}

// Object::getTag is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string Object::getTag() const
{
	return ObjectManager::instance().getObjectTag(*this);
}

// Object::setTag is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Object::setTag(const std::string& tag)
{
	ObjectManager::instance().setObjectTag(*this, tag);
}

// Object::getLayer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string Object::getLayer() const
{
	return ObjectManager::instance().getObjectLayer(*this);
}

// Object::setLayer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Object::setLayer(const std::string& layer)
{
	ObjectManager::instance().setObjectLayer(*this, layer);
}

// Object::isActive is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool Object::isActive() const
{
	return ObjectManager::instance().isObjectActive(*this);
}

// Object::setActive is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Object::setActive(bool active)
{
	ObjectManager::instance().setObjectActive(*this, active);
}

// Object::isStatic is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool Object::isStatic() const
{
	return ObjectManager::instance().isObjectStatic(*this);
}

// Object::setStatic is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Object::setStatic(bool isStatic)
{
	ObjectManager::instance().setObjectStatic(*this, isStatic);
}
