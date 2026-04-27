// Log.cs — Managed API for engine logging and diagnostic output.
//
// This file provides the C# interface for the engine's logging system, allowing
// scripts to emit Info, Debug, Warning, and Error messages. It automatically
// captures caller metadata like file names and line numbers to ensure that
// managed log entries are as informative as native ones.

using System.Runtime.CompilerServices;

namespace TerraEngine
{
    // LogLevel is the managed-facing contract for this subsystem; keep it predictable for scripts.
    public enum LogLevel
    {
        Debug = 0,
        Info = 1,
        Warning = 2,
        Error = 3,
        Fatal = 4
    }

    // Log is the managed-facing contract for this subsystem; keep it predictable for scripts.
    public static class Log
    {
        [MethodImpl(MethodImplOptions.NoInlining)]
        // Info bridges managed callers to engine state; validate arguments before forwarding to native code.
        public static void Info(string message, [CallerLineNumber] int line = 0, [CallerMemberName] string memberName = "", [CallerFilePath] string filePath = "")
            => InternalCalls.Native_Log((int)LogLevel.Info, message, $"{System.IO.Path.GetFileNameWithoutExtension(filePath)}.{memberName}", line);

        [MethodImpl(MethodImplOptions.NoInlining)]
        // Debug bridges managed callers to engine state; validate arguments before forwarding to native code.
        public static void Debug(string message, [CallerLineNumber] int line = 0, [CallerMemberName] string memberName = "", [CallerFilePath] string filePath = "")
            => InternalCalls.Native_Log((int)LogLevel.Debug, message, $"{System.IO.Path.GetFileNameWithoutExtension(filePath)}.{memberName}", line);

        [MethodImpl(MethodImplOptions.NoInlining)]
        // Warning bridges managed callers to engine state; validate arguments before forwarding to native code.
        public static void Warning(string message, [CallerLineNumber] int line = 0, [CallerMemberName] string memberName = "", [CallerFilePath] string filePath = "")
            => InternalCalls.Native_Log((int)LogLevel.Warning, message, $"{System.IO.Path.GetFileNameWithoutExtension(filePath)}.{memberName}", line);

        [MethodImpl(MethodImplOptions.NoInlining)]
        // Error bridges managed callers to engine state; validate arguments before forwarding to native code.
        public static void Error(string message, [CallerLineNumber] int line = 0, [CallerMemberName] string memberName = "", [CallerFilePath] string filePath = "")
            => InternalCalls.Native_Log((int)LogLevel.Error, message, $"{System.IO.Path.GetFileNameWithoutExtension(filePath)}.{memberName}", line);
    }
}
