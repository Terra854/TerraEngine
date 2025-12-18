using System;
using System.Runtime.InteropServices;

namespace TerraEngine.Core
{
    public static class InternalCalls
    {
        [UnmanagedCallersOnly]
        public static void Initialize()
        {
            Console.WriteLine("[.NET] TerraEngine.Core Initialized!");
        }
    }
}
