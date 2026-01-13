using System;
using System.Runtime.InteropServices;
using Hexa.NET.ImGui;
using TerraEngine.Editor.Framework;

namespace TerraEngine.Editor
{
    public static unsafe class EditorApplication
    {
        private static bool _initialized = false;
        private static StatusWindow _statusWindow = null!;

        [UnmanagedCallersOnly]
        public static void Initialize(IntPtr imguiContext)
        {
            if (_initialized) return;

            Console.WriteLine("[C#] Setting Context...");
            try 
            {
                ImGui.SetCurrentContext((ImGuiContext*)imguiContext);
                Console.WriteLine("[C#] Context Set.");

                // Initialize Windows
                _statusWindow = new StatusWindow();
                WindowManager.RegisterWindow(_statusWindow);
            }
            catch (Exception e)
            {
                Console.WriteLine($"[C#] Failed to set context: {e}");
            }
            
            _initialized = true;
        }

        [UnmanagedCallersOnly]
        public static void Update(float deltaTime)
        {
            if (!_initialized) return;

            // Update specific window data if needed
            if (_statusWindow != null)
            {
                _statusWindow.UpdateData(deltaTime);
            }

            // Render all registered windows
            WindowManager.RenderWindows();
        }

        [UnmanagedCallersOnly]
        public static void DrawWindowMenu()
        {
            var windows = WindowManager.GetWindows();
            
            // Fallback for debugging
            if (!System.Linq.Enumerable.Any(windows))
            {
                if (_statusWindow == null) _statusWindow = new StatusWindow();
                WindowManager.RegisterWindow(_statusWindow);
                windows = WindowManager.GetWindows();
            }

            int count = 0;
            foreach (var window in windows)
            {
                count++;
                if (ImGui.MenuItem(window.Title))
                {
                    window.IsVisible = true;
                }
            }

            if (count == 0)
            {
                ImGui.TextDisabled("No registered C# windows");
            }
        }

        [UnmanagedCallersOnly]
        public static void Shutdown()
        {
            Console.WriteLine("[C#] Editor Shutdown");
            _initialized = false;
        }
    }
}
