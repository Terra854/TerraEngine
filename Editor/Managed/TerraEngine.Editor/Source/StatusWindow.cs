using Hexa.NET.ImGui;
using System;
using TerraEngine.Editor.Framework;

namespace TerraEngine.Editor
{
    public class StatusWindow : EditorWindow
    {
        private float _deltaTime;

        public StatusWindow() : base("Engine Status")
        {
            IsVisible = false;
        }

        public void UpdateData(float deltaTime)
        {
            _deltaTime = deltaTime;
        }

        protected override void OnGui()
        {
            ImGui.TextUnformatted($"Delta Time: {_deltaTime * 1000.0f:F2} ms");
            ImGui.TextUnformatted($"FPS: {1.0f / _deltaTime:F1}");
            
            ImGui.Separator();

            if (ImGui.Button("Hello from Abstraction!"))
            {
                Console.WriteLine("Abstraction button clicked!");
            }
        }
    }
}
