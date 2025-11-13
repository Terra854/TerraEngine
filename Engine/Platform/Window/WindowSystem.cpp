#include "WindowSystem.h"

#include <stdexcept>
#include <string>

#ifndef _WIN32
#    include <X11/Xlib.h>
#    include <X11/Xutil.h>
#    include <cstring>
#else
#    include <windowsx.h>
#endif

WindowSystem::WindowSystem()
    : WindowSystem(Description{}) {}

WindowSystem::WindowSystem(const Description& description)
    : description(description) {}

WindowSystem::~WindowSystem()
{
    shutdown();
}

void WindowSystem::Initialize()
{
    if (initialized) {
        return;
    }

#ifdef _WIN32
    const wchar_t* className = L"TerraEngineWindow";

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;

    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw std::runtime_error("Failed to register window class");
    }

    handle.instance = wc.hInstance;

    RECT rect{ 0, 0, static_cast<LONG>(description.width), static_cast<LONG>(description.height) };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    std::wstring titleWide(description.title.begin(), description.title.end());

    handle.window = CreateWindowExW(
        0,
        className,
        titleWide.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        handle.instance,
        nullptr);

    if (!handle.window) {
        throw std::runtime_error("Failed to create window");
    }

    ShowWindow(handle.window, SW_SHOW);
#else
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        throw std::runtime_error("Failed to open X11 display");
    }

    int screen = DefaultScreen(display);
    ::Window root = RootWindow(display, screen);

    XSetWindowAttributes attributes{};
    attributes.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask;

    ::Window window = XCreateWindow(
        display,
        root,
        0,
        0,
        description.width,
        description.height,
        0,
        CopyFromParent,
        InputOutput,
        CopyFromParent,
        CWEventMask,
        &attributes);

    if (!window) {
        XCloseDisplay(display);
        throw std::runtime_error("Failed to create X11 window");
    }

    XStoreName(display, window, description.title.c_str());

    Atom deleteMessage = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &deleteMessage, 1);

    XMapWindow(display, window);
    XFlush(display);

    handle.display = display;
    handle.window = static_cast<std::uint64_t>(window);
    deleteMessageAtom = deleteMessage;
#endif

    initialized = true;
    shouldClose = false;
}

void WindowSystem::Update()
{
    if (!initialized) {
        return;
    }

#ifdef _WIN32
    MSG msg{};
    while (PeekMessageW(&msg, handle.window, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT || msg.message == WM_CLOSE) {
            shouldClose = true;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
#else
    Display* display = reinterpret_cast<Display*>(handle.display);
    if (!display) {
        return;
    }

    while (XPending(display) > 0) {
        XEvent event;
        std::memset(&event, 0, sizeof(event));
        XNextEvent(display, &event);
        if (event.type == ClientMessage && static_cast<unsigned long>(event.xclient.data.l[0]) == deleteMessageAtom) {
            shouldClose = true;
        }
    }
#endif
}

void WindowSystem::shutdown()
{
    if (!initialized) {
        return;
    }

#ifdef _WIN32
    if (handle.window) {
        DestroyWindow(handle.window);
        handle.window = nullptr;
    }
#else
    Display* display = reinterpret_cast<Display*>(handle.display);
    if (display) {
        ::Window window = static_cast<::Window>(handle.window);
        if (window) {
            XDestroyWindow(display, window);
        }
        XCloseDisplay(display);
    }
    handle.display = nullptr;
    handle.window = 0;
    deleteMessageAtom = 0;
#endif

    initialized = false;
    shouldClose = false;
}

