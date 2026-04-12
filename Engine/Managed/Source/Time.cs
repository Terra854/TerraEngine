namespace TerraEngine
{
    public static class Time
    {
        public static float DeltaTime => InternalCalls.Native_Time_GetDeltaTime();
    }
}