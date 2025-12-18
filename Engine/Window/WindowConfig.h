#pragma once

#include <string>

// Basic configuration structure used by the window system to choose a backend
// independent window size and title.
struct WindowConfig {
    int width = 1280;
    int height = 720;
    std::string title = "TerraEngine";
    bool resizable = true;
};
