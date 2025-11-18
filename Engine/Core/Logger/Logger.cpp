#include "Logger.h"

void Logger::Log(LogLevel level, const std::string& file, int line, const std::string& functionSignature, const std::string& message)
{
    LogEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.level = level;
    entry.file = ExtractFileName(file);
    entry.line = line;
    entry.functionSignature = functionSignature;
    entry.message = message;

    {
        std::lock_guard<std::mutex> lock(m);
        q.push(std::move(entry));
    }
    cv.notify_one();
}

void Logger::Shutdown()
{
    done = true;
    cv.notify_one();
    if (t.joinable())
        t.join();
}

Logger::Logger() : done(false)
{
	// Start the worker thread that processes the log queue.
	t = std::thread(&Logger::ProcessEntries, this);
}

Logger::~Logger()
{
	Shutdown();
}

void Logger::ProcessEntries()
{
    while (true) {
        LogEntry entry;
        {
            std::unique_lock<std::mutex> lock(m);
            cv.wait(lock, [this] { return !q.empty() || done; });
            if (done && q.empty())
                break;
            entry = std::move(q.front());
            q.pop();
        }

        // Format the timestamp.
        std::time_t timeT = std::chrono::system_clock::to_time_t(entry.timestamp);
        std::tm tm_time;
#if defined(_WIN32) || defined(_WIN64)
        localtime_s(&tm_time, &timeT);
#else
        localtime_r(&timeT, &tm_time);
#endif
        std::stringstream timeStream;
        timeStream << std::put_time(&tm_time, "%Y-%m-%d %H:%M:%S");

        // Build the final log message with the originating file and line for quick debugging.
        std::stringstream logStream;
        logStream << "[" << timeStream.str() << "][" << LogLevelToString(entry.level) << "]["
                  << entry.file << ":" << entry.line << "][" << entry.functionSignature << "] "
                  << entry.message;

        // Get the appropriate color code and then output the message.
        std::string color = GetColorCode(entry.level);
        std::string resetColor = "\033[0m";  // ANSI reset code

        std::cout << color << logStream.str() << resetColor << std::endl;
    }
}

// Convert LogLevel to string.
std::string Logger::LogLevelToString(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug:   return "DEBUG";
    case LogLevel::Info:    return "INFO";
    case LogLevel::Warning: return "WARNING";
    case LogLevel::Error:   return "ERROR";
    case LogLevel::Critical:return "CRITICAL";
    default:                return "UNKNOWN";
    }
}

// Return ANSI color codes based on the log level.
std::string Logger::GetColorCode(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug:   return "\033[97m";  // Bright White
    case LogLevel::Info:    return "\033[92m";  // Bright Green
    case LogLevel::Warning: return "\033[93m";  // Bright Yellow
    case LogLevel::Error:   return "\033[91m";  // Bright Magenta
    case LogLevel::Critical:return "\033[31m";  // Bright Red
    default:                return "\033[0m";   // Reset (default)
    }
}

// Helper function to extract the file name from a full file path.
std::string Logger::ExtractFileName(const std::string& filePath) {
    size_t pos = filePath.find_last_of("/\\");
    if (pos != std::string::npos) {
        return filePath.substr(pos + 1);
    }
    return filePath;
}
