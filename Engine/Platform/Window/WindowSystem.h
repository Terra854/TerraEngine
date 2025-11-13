#pragma once

#include <cstdint>
#include <string>

#include "Core/SystemBase.h"

#ifdef _WIN32
#    include <windows.h>
#endif

struct NativeWindowHandle {
#ifdef _WIN32
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
#else
    struct _XDisplay* display = nullptr;
    std::uint64_t window = 0;
#endif
};

class WindowSystem : public SystemsBase {
public:
    struct Description {
        std::string title = "TerraEngine";
        std::uint32_t width = 1280;
        std::uint32_t height = 720;
    };

    WindowSystem();
    explicit WindowSystem(const Description& description);
    ~WindowSystem() override;

    void Initialize() override;
    void Update() override;
    void shutdown() override;

    const NativeWindowHandle& GetNativeHandle() const { return handle; }

    std::uint32_t GetWidth() const { return description.width; }
    std::uint32_t GetHeight() const { return description.height; }

    bool ShouldClose() const { return shouldClose; }

private:
    Description description;
    NativeWindowHandle handle{};

    bool initialized = false;
    bool shouldClose = false;

#ifndef _WIN32
    unsigned long deleteMessageAtom = 0;
#else
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif
};

