#pragma once

#include "Export.h"
#include <string>

class TERRAENGINE_API SystemsBase {
public:
	SystemsBase() {};
	virtual ~SystemsBase() = default;

	// System Initialization
	virtual void Initialize() = 0;

	// System Update
	virtual void Update() = 0;

	// System freeing resources
	virtual void Shutdown() = 0;

	// For debugging, getting System Name
	virtual std::string SystemName() {
		return "Unnamed system (If you are seeing this, you need to implement SystemName() inside the system)";
	};
};
