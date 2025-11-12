#pragma once

#include "Component.h"
//#include <rttr/registration>

class Transform : public Component {
public:
	Transform() : x(0), y(0), z(0), rotationX(0), rotationY(0), rotationZ(0), scaleX(1), scaleY(1), scaleZ(1) {}
	// Placeholders for now, will replace with actual stuff later
	float x, y, z;
	float rotationX, rotationY, rotationZ;
	float scaleX, scaleY, scaleZ;
};

/* Can't compile for some reason */
/*
RTTR_REGISTRATION {
	rttr::registration::class_<Transform>("Transform")
		.property("x", &Transform::x)
		.property("y", &Transform::y)
		.property("z", &Transform::z)
		.property("rotationX", &Transform::rotationX)
		.property("rotationY", &Transform::rotationY)
		.property("rotationZ", &Transform::rotationZ)
		.property("scaleX", &Transform::scaleX)
		.property("scaleY", &Transform::scaleY)
		.property("scaleZ", &Transform::scaleZ);
}
*/