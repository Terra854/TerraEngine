#pragma once
#include <iostream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <string>

// Define the log severity levels.
enum class LogLevel {
	Debug,
	Info,
	Warning,
	Error,
	Critical
};

// Structure for holding a log entry.
struct LogEntry {
	std::chrono::system_clock::time_point timestamp; // When the log was generated.
	LogLevel level;                                  // Severity level of the log.
	std::string file;                                // Source file where the log originated.
	int line = 0;                                    // Source line where the log macro was invoked.
	std::string functionSignature;                   // Function signature of the caller for richer context.
	std::string message;                             // Human-readable message payload.
	bool isManaged = false;                          // Whether the log originated from managed code (C#).
};

class Logger {
public:
	// Retrieve the singleton instance.
	static Logger& Instance() {
		static Logger instance;
		return instance;
	}

	// Delete copy constructor and assignment operator.
	Logger(const Logger&) = delete;
	Logger& operator=(const Logger&) = delete;

	// Thread-safe log function that records the call-site file, line, and function signature.
	void Log(LogLevel level, const std::string& file, int line, const std::string& functionSignature, const std::string& message, bool isManaged = false);

	void SetMinLogLevel(LogLevel level) { minLevel = level; }
	LogLevel GetMinLogLevel() const { return minLevel; }
	bool ShouldLog(LogLevel level) const { return level >= minLevel; }

	// Gracefully shutdown the logger thread.
	void Shutdown();

private:
	Logger();
	~Logger();

	// The worker thread function: waits for and processes log entries.
	void ProcessEntries();

	std::string LogLevelToString(LogLevel level);
	std::string GetColorCode(LogLevel level);

	std::string ExtractFileName(const std::string& filePath);

	std::queue<LogEntry> q;
	std::mutex m;
	std::condition_variable cv;
	std::thread t; // Seperate thread for logging outside of the main thread and threadpool
	std::atomic<bool> done;
	std::atomic<LogLevel> minLevel{LogLevel::Debug};
};

// Macro to simplify logging while capturing source file, line, and function signature.
#if defined(__GNUC__) || defined(__clang__)
#define LOG_FUNCTION __PRETTY_FUNCTION__
#elif defined(_MSC_VER)
#define LOG_FUNCTION __FUNCSIG__
#else
#define LOG_FUNCTION __FUNCTION__
#endif

// Macro to simplify logging while capturing source file, line, and function signature.
#define TERRAENGINE_LOG(level, message) do {                                 \
	if (Logger::Instance().ShouldLog(level)) {                               \
		std::ostringstream _terraengine_os;                                  \
		_terraengine_os << message;                                          \
		Logger::Instance().Log(level, __FILE__, __LINE__, LOG_FUNCTION, _terraengine_os.str());\
	}                                                                        \
} while(0)
