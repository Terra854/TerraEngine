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

    void SetRunning(bool running) { m_Running = running; }
    void SetPaused(bool paused) { m_Paused = paused; }
    void Step(int frames = 1) { m_StepFrames = frames; }
    
    bool IsRunning() const { return m_Running; }
    bool IsPaused() const { return m_Paused; }

private:
    ScriptingSystem() = default;
    
    bool m_Running = true;
    bool m_Paused = false;
    int m_StepFrames = 0;

    typedef void (*InitFn)();
    InitFn managedInit = nullptr;

    typedef void (*UpdateFn)(uint32_t, const char*, float);
    UpdateFn managedUpdate = nullptr;
};
