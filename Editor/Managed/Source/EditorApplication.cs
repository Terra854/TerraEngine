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

            AppDomain.CurrentDomain.UnhandledException += (s, e) =>
            {
                TerraEngine.Log.Error($"Unhandled C# Exception:\n{e.ExceptionObject}");
            };

            TerraEngine.Log.Info("Setting ImGui Context...");
            try 
            {
                ImGui.SetCurrentContext((ImGuiContext*)imguiContext);
                TerraEngine.Log.Info("Context Set.");

                // Initialize Windows
                _statusWindow = new StatusWindow();
                WindowManager.RegisterWindow(_statusWindow);
            }
            catch (Exception e)
            {
                TerraEngine.Log.Error($"Failed to set context: {e}");
            }
            
            _initialized = true;
        }

        [UnmanagedCallersOnly]
        public static void Update(float deltaTime)
        {
            if (!_initialized) return;

            try
            {
                // Update specific window data if needed
                if (_statusWindow != null)
                {
                    _statusWindow.UpdateData(deltaTime);
                }

                // Render all registered windows
                WindowManager.RenderWindows();
            }
            catch (Exception ex)
            {
                TerraEngine.Log.Error($"Exception in Editor Update:\n{ex}");
            }
        }

        [UnmanagedCallersOnly]
        public static void DrawWindowMenu()
        {
            try
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
            catch (Exception ex)
            {
                TerraEngine.Log.Error($"Exception in DrawWindowMenu:\n{ex}");
            }
        }

        [UnmanagedCallersOnly]
        public static void Shutdown()
        {
            try
            {
                TerraEngine.Log.Info("Editor Managed Shutdown");
                _initialized = false;
            }
            catch (Exception ex)
            {
                TerraEngine.Log.Error($"Exception in Shutdown:\n{ex}");
            }
        }
    }
}
