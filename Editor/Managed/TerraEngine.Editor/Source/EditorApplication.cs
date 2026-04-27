// EditorApplication.cs — Managed editor application entry point
//
// Coordinates the lifecycle of the managed editor environment, including ImGui context initialization and subsystem updates. It provides the UnmanagedCallersOnly entry points for the native engine to invoke managed logic for rendering, menu handling, and shutdown.

using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using Hexa.NET.ImGui;
using TerraEngine.Editor.Framework;

namespace TerraEngine.Editor
{
    public static unsafe class EditorApplication
    {
        private static bool _initialized = false;
        private static StatusWindow _statusWindow = null!;
        private static RenderSettingsWindow _renderSettingsWindow = null!;

        [UnmanagedCallersOnly]
        // Initialize bridges managed callers to engine state; validate arguments before forwarding to native code.
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

                EnsureWindowsRegistered();
            }
            catch (Exception e)
            {
                TerraEngine.Log.Error($"Failed to set context: {e}");
            }
            
            _initialized = true;
        }

        // EnsureWindowsRegistered bridges managed callers to engine state; validate arguments before forwarding to native code.
        private static void EnsureWindowsRegistered()
        {
            if (_statusWindow == null)
            {
                _statusWindow = new StatusWindow();
                WindowManager.RegisterWindow(_statusWindow);
            }

            // Validate this condition before crossing into native or editor state that may outlive the managed object.
            if (_renderSettingsWindow == null)
            {
                _renderSettingsWindow = new RenderSettingsWindow();
                WindowManager.RegisterWindow(_renderSettingsWindow);
            }
        }

        [UnmanagedCallersOnly]
        // Update bridges managed callers to engine state; validate arguments before forwarding to native code.
        public static void Update(float deltaTime)
        {
            if (!_initialized) return;

            try
            {
                EnsureWindowsRegistered();

                // Update specific window data if needed
                // Validate this condition before crossing into native or editor state that may outlive the managed object.
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
        // DrawWindowMenu bridges managed callers to engine state; validate arguments before forwarding to native code.
        public static void DrawWindowMenu()
        {
            try
            {
                EnsureWindowsRegistered();

                var windows = WindowManager.GetWindows();
                int count = 0;
                foreach (var window in windows)
                {
                    count++;
                    bool visible = window.IsVisible;
                    // Validate this condition before crossing into native or editor state that may outlive the managed object.
                    if (ImGui.MenuItem(window.Title, "", ref visible))
                    {
                        window.IsVisible = visible;
                    }
                }

                // Validate this condition before crossing into native or editor state that may outlive the managed object.
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
        // Shutdown bridges managed callers to engine state; validate arguments before forwarding to native code.
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

        [UnmanagedCallersOnly]
        // GetManagedWindows bridges managed callers to engine state; validate arguments before forwarding to native code.
        public static void GetManagedWindows(delegate* unmanaged<IntPtr, int, void> callback)
        {
            try
            {
                EnsureWindowsRegistered();
                foreach (var window in WindowManager.GetWindows())
                {
                    IntPtr titlePtr = Marshal.StringToHGlobalAnsi(window.Title);
                    callback(titlePtr, window.IsVisible ? 1 : 0);
                    Marshal.FreeHGlobal(titlePtr);
                }
            }
            catch (Exception ex)
            {
                TerraEngine.Log.Error($"Exception in GetManagedWindows:\n{ex}");
            }
        }

        [UnmanagedCallersOnly]
        // SetWindowVisibility bridges managed callers to engine state; validate arguments before forwarding to native code.
        public static void SetWindowVisibility(IntPtr titlePtr, int visible)
        {
            try
            {
                EnsureWindowsRegistered();
                string? title = Marshal.PtrToStringAnsi(titlePtr);
                // Validate this condition before crossing into native or editor state that may outlive the managed object.
                if (title == null) return;

                foreach (var window in WindowManager.GetWindows())
                {
                    // Validate this condition before crossing into native or editor state that may outlive the managed object.
                    if (window.Title == title)
                    {
                        window.IsVisible = visible != 0;
                        break;
                    }
                }
            }
            catch (Exception ex)
            {
                TerraEngine.Log.Error($"Exception in SetWindowVisibility:\n{ex}");
            }
        }

        [UnmanagedCallersOnly]
        // HideAllWindows bridges managed callers to engine state; validate arguments before forwarding to native code.
        public static void HideAllWindows()
        {
            try
            {
                EnsureWindowsRegistered();
                foreach (var window in WindowManager.GetWindows())
                {
                    window.IsVisible = false;
                }
            }
            catch (Exception ex)
            {
                TerraEngine.Log.Error($"Exception in HideAllWindows:\n{ex}");
            }
        }
    }
}
