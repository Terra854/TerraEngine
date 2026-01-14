using System.Runtime.CompilerServices;

namespace TerraEngine
{
    public enum LogLevel
    {
        Debug = 0,
        Info = 1,
        Warning = 2,
        Error = 3,
        Fatal = 4
    }

    public static class Log
    {
        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void Info(string message, [CallerLineNumber] int line = 0, [CallerMemberName] string memberName = "", [CallerFilePath] string filePath = "")
            => InternalCalls.Native_Log((int)LogLevel.Info, message, $"{System.IO.Path.GetFileNameWithoutExtension(filePath)}.{memberName}", line);

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void Debug(string message, [CallerLineNumber] int line = 0, [CallerMemberName] string memberName = "", [CallerFilePath] string filePath = "")
            => InternalCalls.Native_Log((int)LogLevel.Debug, message, $"{System.IO.Path.GetFileNameWithoutExtension(filePath)}.{memberName}", line);

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void Warning(string message, [CallerLineNumber] int line = 0, [CallerMemberName] string memberName = "", [CallerFilePath] string filePath = "")
            => InternalCalls.Native_Log((int)LogLevel.Warning, message, $"{System.IO.Path.GetFileNameWithoutExtension(filePath)}.{memberName}", line);

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void Error(string message, [CallerLineNumber] int line = 0, [CallerMemberName] string memberName = "", [CallerFilePath] string filePath = "")
            => InternalCalls.Native_Log((int)LogLevel.Error, message, $"{System.IO.Path.GetFileNameWithoutExtension(filePath)}.{memberName}", line);
    }
}
