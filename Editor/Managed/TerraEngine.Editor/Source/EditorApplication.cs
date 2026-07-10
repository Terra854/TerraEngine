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
	[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
	public delegate void EditorInitializeDelegate(IntPtr imguiContext);

	[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
	public delegate void EditorUpdateDelegate(float deltaTime);

	[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
	public delegate void EditorShutdownDelegate();

	[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
	public delegate void EditorDrawWindowMenuDelegate();

	[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
	public delegate void EditorGetWindowsDelegate(IntPtr callback);

	[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
	public delegate void EditorSetWindowVisibilityDelegate(IntPtr titlePtr, int visible);

	[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
	public delegate void EditorHideAllWindowsDelegate();

    public static unsafe class EditorApplication
    {
        private static bool _initialized = false;
        private static StatusWindow _statusWindow = null!;

		private sealed class WindowMenuNode
		{
			public WindowMenuNode(string label)
			{
				Label = label;
			}

			public string Label { get; }
			public EditorWindow? Window { get; set; }
			public List<WindowMenuNode> Children { get; } = new();
		}

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
        }

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

		private static void AddWindowMenuPath(List<WindowMenuNode> roots, EditorWindow window)
		{
			string? categoryPath = window.WindowMenuCategoryPath;
			if (string.IsNullOrWhiteSpace(categoryPath))
			{
				// Registration controls drawing and workspace persistence only. Menu
				// visibility requires an explicit path owned by the window class.
				return;
			}

			List<WindowMenuNode> siblings = roots;
			WindowMenuNode? current = null;
			foreach (string rawSegment in categoryPath.Split('/', StringSplitOptions.RemoveEmptyEntries))
			{
				string segment = rawSegment.Trim();
				if (segment.Length == 0)
				{
					continue;
				}

				current = siblings.Find(node => node.Label == segment);
				if (current == null)
				{
					current = new WindowMenuNode(segment);
					siblings.Add(current);
				}
				siblings = current.Children;
			}

			if (current != null)
			{
				// The window title is always the leaf label beneath its declared
				// category path, so category names never need to repeat that title.
				current.Children.Add(new WindowMenuNode(window.Title) { Window = window });
			}
		}

		private static void DrawWindowMenuNodes(IEnumerable<WindowMenuNode> nodes)
		{
			foreach (WindowMenuNode node in nodes)
			{
				if (node.Children.Count > 0)
				{
					if (ImGui.BeginMenu(node.Label))
					{
						DrawWindowMenuNodes(node.Children);
						ImGui.EndMenu();
					}
					continue;
				}

				if (node.Window != null)
				{
					bool visible = node.Window.IsVisible;
					if (ImGui.MenuItem(node.Label, "", ref visible))
					{
						node.Window.IsVisible = visible;
					}
				}
			}
		}

        // DrawWindowMenu bridges managed callers to engine state; validate arguments before forwarding to native code.
        public static void DrawWindowMenu()
        {
            try
            {
                EnsureWindowsRegistered();

				List<WindowMenuNode> roots = new();
				foreach (EditorWindow window in WindowManager.GetWindows())
				{
					AddWindowMenuPath(roots, window);
				}
				DrawWindowMenuNodes(roots);

				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (roots.Count == 0)
                {
                    ImGui.TextDisabled("No registered C# windows");
                }
            }
            catch (Exception ex)
            {
                TerraEngine.Log.Error($"Exception in DrawWindowMenu:\n{ex}");
            }
        }

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

        // GetManagedWindows bridges managed callers to engine state; validate arguments before forwarding to native code.
        public static void GetManagedWindows(IntPtr callbackPtr)
        {
            try
            {
                // The native host passes a raw function pointer through the
                // hosted delegate thunk. Keep it as IntPtr at the public
                // boundary so hostfxr can generate the delegate wrapper, then
                // reinterpret it inside the unsafe editor assembly.
                delegate* unmanaged<IntPtr, int, void> callback = (delegate* unmanaged<IntPtr, int, void>)callbackPtr;
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
