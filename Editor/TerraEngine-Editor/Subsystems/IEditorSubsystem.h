#pragma once

#include "EditorContext.h"
#include <string>

class IEditorSubsystem {
public:
    virtual ~IEditorSubsystem() = default;
    
    virtual void Initialize(EditorContext& context) = 0;
    virtual void OnBackendReady() {}
    virtual void Update(float deltaTime) = 0;
    virtual void DrawUI() = 0;
    virtual void Shutdown() = 0;
    virtual std::string GetName() const = 0;
};
