// SystemBase.h — Base interface for all modular engine subsystems.
//
// This file defines the SystemsBase class, which provides the standard lifecycle
// hooks (Initialize, Update, Shutdown) required for any engine system. By
// inheriting from this class, subsystems can be registered with and managed by
// the MainEngine loop in a consistent manner.

#pragma once

#include "Export.h"
#include <string>

class TERRAENGINE_API SystemsBase {
public:
	SystemsBase() {};
	virtual ~SystemsBase() = default;

	// System Initialization
	virtual void Initialize() = 0;

	// System Update with Delta Time
	virtual void Update(float deltaTime) = 0;

	// System freeing resources
	virtual void Shutdown() = 0;

	// For debugging, getting System Name
	// SystemName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	virtual std::string SystemName() {
		return "Unnamed system (If you are seeing this, you need to implement SystemName() inside the system)";
	};
};
