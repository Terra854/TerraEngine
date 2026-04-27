// WindowManager.cs — Managed window management registry
//
// Manages the registration and rendering of all active managed editor windows. It serves as a central registry that coordinates the update and draw calls for registered EditorWindow instances, ensuring they are integrated into the main editor UI loop.

using System.Collections.Generic;

namespace TerraEngine.Editor.Framework
{
    // WindowManager is the managed-facing contract for this subsystem; keep it predictable for scripts.
    public static class WindowManager
    {
        private static readonly List<EditorWindow> _windows = new();

        // RegisterWindow bridges managed callers to engine state; validate arguments before forwarding to native code.
        public static void RegisterWindow(EditorWindow window)
        {
            if (!_windows.Contains(window))
            {
                _windows.Add(window);
            }
        }

        // EnsureDefaultWindows bridges managed callers to engine state; validate arguments before forwarding to native code.
        public static void EnsureDefaultWindows()
        {
            if (_windows.Count == 0)
            {
                // Fallback if initialize failed to register for some reason
                // RegisterWindow(new StatusWindow()); // Cannot easily without ref
            }
        }

        // UnregisterWindow bridges managed callers to engine state; validate arguments before forwarding to native code.
        public static void UnregisterWindow(EditorWindow window)
        {
            _windows.Remove(window);
        }

        // GetWindows bridges managed callers to engine state; validate arguments before forwarding to native code.
        public static IEnumerable<EditorWindow> GetWindows() => _windows;

        public static void RenderWindows()
        {
            foreach (var window in _windows)
            {
                window.Draw();
            }
        }
    }
}
