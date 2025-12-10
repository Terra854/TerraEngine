#include "Component.h"

rttr::variant GetComponentProperty(Object& object, std::string_view componentName, std::string_view propertyName)
{
	// Locate the pre-registered getter for this component name without
	// holding the registry lock during the actual property access.
	std::function<rttr::variant(Object&, std::string_view)> getter;
	{
		std::lock_guard lock(ComponentAccessRegistryMutex());
		auto it = ComponentAccessRegistry().find(std::string(componentName));
		if (it != ComponentAccessRegistry().end())
		{
			getter = it->second.getter;
		}
	}

	if (!getter)
	{
		return {};
	}
	return getter(object, propertyName);
}

bool SetComponentProperty(Object& object, std::string_view componentName, std::string_view propertyName, const rttr::variant& value)
{
	// Locate the setter and let the component's write lock guard the
	// mutation once the correct component instance is resolved.
	std::function<bool(Object&, std::string_view, const rttr::variant&)> setter;
	{
		std::lock_guard lock(ComponentAccessRegistryMutex());
		auto it = ComponentAccessRegistry().find(std::string(componentName));
		if (it != ComponentAccessRegistry().end())
		{
			setter = it->second.setter;
		}
	}

	if (!setter)
	{
		return false;
	}
	return setter(object, propertyName, value);
}
