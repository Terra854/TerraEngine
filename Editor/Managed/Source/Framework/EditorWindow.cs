    using System;
using Hexa.NET.ImGui;

namespace TerraEngine.Editor.Framework
{
    public abstract class EditorWindow
    {
        public string Title { get; set; } = "Window";
        public bool IsVisible { get; set; } = true;
        public ImGuiWindowFlags Flags { get; set; } = ImGuiWindowFlags.None;

        protected EditorWindow(string title)
        {
            Title = title;
        }

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
                if (ImGui.BeginPopupContextItem())
                {
                    OnContextMenu();
                    if (ImGui.MenuItem("Close"))
                    {
                        IsVisible = false;
                    }
                    ImGui.EndPopup();
                }
                */

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

        protected abstract void OnGui();
        protected virtual void OnContextMenu() { }
    }
}
