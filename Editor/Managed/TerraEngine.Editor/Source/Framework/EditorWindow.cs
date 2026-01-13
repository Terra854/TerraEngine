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

            var window = ImGuiP.GetCurrentWindow();
            bool tabHovered = false;
            if (!window.DockNode.IsNull && !window.DockNode.TabBar.IsNull)
            {
                var tabBar = window.DockNode.TabBar;
                for (int i = 0; i < tabBar.Tabs.Size; i++)
                {
                    var tab = tabBar.Tabs[i];
                    if (tab.ID == window.TabId)
                    {
                        var tabMin = tabBar.BarRect.Min;
                        tabMin.X += tab.Offset - tabBar.ScrollingAnim;
                        var tabMax = new System.Numerics.Vector2(tabMin.X + tab.Width, tabBar.BarRect.Max.Y);

                        var mousePos = ImGui.GetMousePos();
                        if (mousePos.X >= tabMin.X && mousePos.X <= tabMax.X &&
                            mousePos.Y >= tabMin.Y && mousePos.Y <= tabMax.Y)
                        {
                            tabHovered = true;
                        }
                        break;
                    }
                }
            }

            bool titleBarHovered = false;
            if (window.DockNode.IsNull && (window.Flags & ImGuiWindowFlags.NoTitleBar) == 0)
            {
                var mousePos = ImGui.GetMousePos();
                if (mousePos.X >= window.Pos.X && mousePos.X <= (window.Pos.X + window.SizeFull.X) &&
                    mousePos.Y >= window.Pos.Y && mousePos.Y <= (window.Pos.Y + window.TitleBarHeight))
                {
                    titleBarHovered = true;
                }
            }

            if (ImGui.IsMouseReleased(ImGuiMouseButton.Right))
            {
                if (titleBarHovered || tabHovered)
                {
                    ImGui.OpenPopup("WindowContextMenu");
                }
            }

            if (ImGui.BeginPopup("WindowContextMenu"))
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
