// StatusWindow.cs — Managed engine status diagnostic window
//
// Provides a real-time diagnostic window within the managed editor, displaying
// frame timings and FPS during active development.

using Hexa.NET.ImGui;
using System;
using TerraEngine.Editor.Framework;

namespace TerraEngine.Editor
{
    // StatusWindow is the managed-facing contract for this subsystem; keep it predictable for scripts.
    public class StatusWindow : EditorWindow
    {
        private float _deltaTime;

        public StatusWindow() : base("Engine Status")
        {
            IsVisible = false;
        }

        // UpdateData bridges managed callers to engine state; validate arguments before forwarding to native code.
        public void UpdateData(float deltaTime)
        {
            _deltaTime = deltaTime;
        }

        // OnGui bridges managed callers to engine state; validate arguments before forwarding to native code.
        protected override void OnGui()
        {
            ImGui.TextUnformatted($"Delta Time: {_deltaTime * 1000.0f:F2} ms");
            ImGui.TextUnformatted($"FPS: {1.0f / _deltaTime:F1}");

            ImGui.Separator();

            // Validate this condition before crossing into native or editor state that may outlive the managed object.
            if (ImGui.Button("Hello from Abstraction!"))
            {
                Console.WriteLine("Abstraction button clicked!");
            }
        }
    }
}
