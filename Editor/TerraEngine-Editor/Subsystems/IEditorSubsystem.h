// IEditorSubsystem.h — Editor subsystem interface
//
// Defines the base interface for all editor-side systems. It establishes the standard lifecycle hooks for initialization, updating, and UI rendering, ensuring a consistent integration pattern for diverse editor functionalities.

#pragma once

#include "EditorContext.h"
#include <string>

class IEditorSubsystem {
public:
	virtual ~IEditorSubsystem() = default;
	
	virtual void Initialize(EditorContext& context) = 0;
	// OnBackendReady is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	virtual void OnBackendReady() {}
	virtual void Update(float deltaTime) = 0;
	virtual void DrawUI() = 0;
	virtual void Shutdown() = 0;
	virtual std::string GetName() const = 0;
};
