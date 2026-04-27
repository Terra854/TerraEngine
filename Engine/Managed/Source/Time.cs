// Time.cs — Contains the managed Time API surface, properties, native-call bindings, and script/editor workflow helpers for C# scripting API types that mirror native engine objects, components, and services.

namespace TerraEngine
{
    // Time is the managed-facing contract for this subsystem; keep it predictable for scripts.
    public static class Time
    {
        public static float DeltaTime => InternalCalls.Native_Time_GetDeltaTime();
    }
}