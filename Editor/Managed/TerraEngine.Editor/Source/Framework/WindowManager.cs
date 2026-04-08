using System.Collections.Generic;

namespace TerraEngine.Editor.Framework
{
    public static class WindowManager
    {
        private static readonly List<EditorWindow> _windows = new();

        public static void RegisterWindow(EditorWindow window)
        {
            if (!_windows.Contains(window))
            {
                _windows.Add(window);
            }
        }

        public static void EnsureDefaultWindows()
        {
            if (_windows.Count == 0)
            {
                // Fallback if initialize failed to register for some reason
                // RegisterWindow(new StatusWindow()); // Cannot easily without ref
            }
        }

        public static void UnregisterWindow(EditorWindow window)
        {
            _windows.Remove(window);
        }

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
