#pragma once

#include "Core/SystemBase.h"
#include <memory>

class ScriptingSystem : public SystemsBase {
public:
    static ScriptingSystem& Instance();

    void Initialize() override;
    void Update(float deltaTime) override;
    void Shutdown() override;
    std::string SystemName() override;

private:
    ScriptingSystem() = default;
    
    typedef void (*InitFn)();
    InitFn managedInit = nullptr;

    typedef void (*UpdateFn)(uint32_t, const char*, float);
    UpdateFn managedUpdate = nullptr;
};
