#include "imgui.h"
#include <stddef.h>

// Polyfill for missing exported functions in cimgui DLL
// These are defined in imgui.cpp but not exported via IMGUI_API in the current version of cimgui/imgui submodule.

void ImGuiPlatformIO::ClearPlatformHandlers()
{
    Platform_GetClipboardTextFn = NULL;
    Platform_SetClipboardTextFn = NULL;
    Platform_OpenInShellFn = NULL;
    Platform_SetImeDataFn = NULL;
    Platform_ClipboardUserData = Platform_OpenInShellUserData = Platform_ImeUserData = NULL;
    Platform_CreateWindow = Platform_DestroyWindow = Platform_ShowWindow = NULL;
    Platform_SetWindowPos = Platform_SetWindowSize = NULL;
    Platform_GetWindowPos = Platform_GetWindowSize = Platform_GetWindowFramebufferScale = NULL;
    Platform_SetWindowFocus = NULL;
    Platform_GetWindowFocus = Platform_GetWindowMinimized = NULL;
    Platform_SetWindowTitle = NULL;
    Platform_SetWindowAlpha = NULL;
    Platform_UpdateWindow = NULL;
    Platform_RenderWindow = Platform_SwapBuffers = NULL;
    Platform_GetWindowDpiScale = NULL;
    Platform_OnChangedViewport = NULL;
    Platform_GetWindowWorkAreaInsets = NULL;
    Platform_CreateVkSurface = NULL;
}

void ImGuiPlatformIO::ClearRendererHandlers()
{
    Renderer_TextureMaxWidth = Renderer_TextureMaxHeight = 0;
    Renderer_RenderState = NULL;
    Renderer_CreateWindow = Renderer_DestroyWindow = NULL;
    Renderer_SetWindowSize = NULL;
    Renderer_RenderWindow = Renderer_SwapBuffers = NULL;
}
