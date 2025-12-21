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

            // Begin without ref bool to hide the default close button 'X'
            bool expanded = ImGui.Begin(Title, Flags);

            // Right-click context menu on the window/tab to close it
            if (ImGui.BeginPopupContextWindow())
            {
                OnContextMenu();
                if (ImGui.MenuItem("Close"))
                {
                    IsVisible = false;
                }
                ImGui.EndPopup();
            }

            if (expanded)
            {
                OnGui();
            }
            ImGui.End();
        }

        protected abstract void OnGui();
        protected virtual void OnContextMenu() { }
    }
}
