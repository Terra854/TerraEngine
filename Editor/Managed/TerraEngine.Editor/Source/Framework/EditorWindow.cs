// EditorWindow.cs — Base class for managed editor windows
//
// Defines the core abstraction for windows within the managed editor framework. It provides properties for title and visibility, and manages the ImGui lifecycle by wrapping the native Begin/End calls around an abstract OnGui method.

    using System;
using Hexa.NET.ImGui;

namespace TerraEngine.Editor.Framework
{
    public abstract class EditorWindow
    {
        public string Title { get; set; } = "Window";
        public bool IsVisible { get; set; } = true;
        public ImGuiWindowFlags Flags { get; set; } = ImGuiWindowFlags.None;
		// WindowMenuCategoryPath opts a managed window into the Windows menu. Each
		// slash-separated segment is a window-owned subcategory; the window title
		// becomes its leaf item. Null or empty keeps the window out of the menu.
		public string? WindowMenuCategoryPath { get; }

		protected EditorWindow(string title, string? windowMenuCategoryPath = null)
        {
            Title = title;
			WindowMenuCategoryPath = windowMenuCategoryPath;
        }

        // Draw bridges managed callers to engine state; validate arguments before forwarding to native code.
        public void Draw()
        {
            if (!IsVisible) return;
            _isVisible = IsVisible;

            // Simple Begin/End loop
            bool expanded = ImGui.Begin(Title, ref _isVisible, Flags);
            // Sync visibility back to property
            IsVisible = _isVisible;

            try
            {
                // Standard Context Menu on the window background
                /*
                // Validate this condition before crossing into native or editor state that may outlive the managed object.
                if (ImGui.BeginPopupContextItem())
                {
                    OnContextMenu();
                    // Validate this condition before crossing into native or editor state that may outlive the managed object.
                    if (ImGui.MenuItem("Close"))
                    {
                        IsVisible = false;
                    }
                    ImGui.EndPopup();
                }
                */

                // Validate this condition before crossing into native or editor state that may outlive the managed object.
                if (expanded)
                {
                    OnGui();
                }
            }
            finally
            {
                ImGui.End();
            }
        }

        private bool _isVisible = true;

        // OnGui bridges managed callers to engine state; validate arguments before forwarding to native code.
        protected abstract void OnGui();
        protected virtual void OnContextMenu() { }
    }
}
