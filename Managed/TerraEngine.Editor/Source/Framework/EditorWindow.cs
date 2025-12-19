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

            // We need to use a bool reference for the close button to work
            bool visible = IsVisible;
            
            // Hexa.NET.ImGui uses pointers or ref for the bool open argument
            // ImGui.Begin returns true if the window is not collapsed
            if (ImGui.Begin(Title, ref visible, Flags))
            {
                OnGui();
            }
            ImGui.End();

            IsVisible = visible;
        }

        protected abstract void OnGui();
    }
}
