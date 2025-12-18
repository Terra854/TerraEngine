using System;
using System.Runtime.InteropServices;
using Hexa.NET.ImGui;

namespace TerraEngine.Editor
{
    public static unsafe class EditorApplication
    {
        private static bool _initialized = false;

        [UnmanagedCallersOnly]
        public static void Initialize(IntPtr imguiContext)
        {
            if (_initialized) return;

            Console.WriteLine("[C#] Setting Context...");
            try 
            {
                ImGui.SetCurrentContext((ImGuiContext*)imguiContext);
                Console.WriteLine("[C#] Context Set.");
            }
            catch (Exception e)
            {
                Console.WriteLine($"[C#] Failed to set context: {e}");
            }
            
            _initialized = true;
        }

        [UnmanagedCallersOnly]
        public static void Update(float deltaTime)
        {
            if (!_initialized) return;

            // ImGui commands
            ImGui.Begin("C# Editor Window");
            ImGui.Text($"Delta Time: {deltaTime}");
            ImGui.Text($"FPS: {1.0f / deltaTime}");
            
            if (ImGui.Button("Hello from C#!"))
            {
                Console.WriteLine("Button Clicked!");
            }
            ImGui.End();
        }

        [UnmanagedCallersOnly]
        public static void Shutdown()
        {
            Console.WriteLine("[C#] Editor Shutdown");
            _initialized = false;
        }
    }
}
