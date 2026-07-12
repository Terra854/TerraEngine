// Logger.h — Interface for the engine's multi-threaded logging system.
//
// This file declares the Logger singleton and supporting structures for recording
// diagnostic messages with severity levels and source location metadata. It
// provides macros for convenient logging across the engine and defines the
// asynchronous processing model for log entries.

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
#include <cstdint>
#include <functional>
#include <map>
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
// struct LogEntry groups related state and operations so callers have one documented contract.
struct LogEntry {
	std::chrono::system_clock::time_point timestamp; // When the log was generated.
	LogLevel level;                                  // Severity level of the log.
	std::string file;                                // Source file where the log originated.
	int line = 0;                                    // Source line where the log macro was invoked.
	std::string functionSignature;                   // Function signature of the caller for richer context.
	std::string message;                             // Human-readable message payload.
	bool isManaged = false;                          // Whether the log originated from managed code (C#).
	// Automation context is optional. Ordinary engine logging remains unchanged,
	// while request and job scopes can attach stable identities without encoding
	// machine-readable data into the human-facing message.
	std::string category;
	std::string source;
	std::string correlationId;
	std::string requestId;
	std::string jobId;
};

// LogContext carries diagnostic identity across a synchronous request or one
// explicitly scoped worker task. Context is thread-local so concurrent MCP jobs
// cannot overwrite each other's correlation identifiers.
struct LogContext {
	std::string category;
	std::string source;
	std::string correlationId;
	std::string requestId;
	std::string jobId;
};

// class Logger groups related state and operations so callers have one documented contract.
class Logger {
public:
	using SinkId = std::uint64_t;
	using Sink = std::function<void(const LogEntry&)>;

	// ScopedContext restores the caller's previous context at scope exit. Worker
	// jobs must create their own scope because thread-local state is intentionally
	// not inherited implicitly by unrelated thread-pool work.
	class ScopedContext {
	public:
		explicit ScopedContext(LogContext context);
		~ScopedContext();
		ScopedContext(const ScopedContext&) = delete;
		ScopedContext& operator=(const ScopedContext&) = delete;

	private:
		LogContext previous;
	};

	// Retrieve the singleton instance.
	// Instance is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	static Logger& Instance() {
		static Logger instance;
		return instance;
	}

	// Delete copy constructor and assignment operator.
	Logger(const Logger&) = delete;
	Logger& operator=(const Logger&) = delete;

	// Thread-safe log function that records the call-site file, line, and function signature.
	void Log(LogLevel level, const std::string& file, int line, const std::string& functionSignature, const std::string& message, bool isManaged = false);

	// Sinks receive immutable entries synchronously before console publication.
	// RemoveSink waits for an in-flight callback, which makes it safe for an
	// editor-owned store to detach during shutdown without a dangling callback.
	[[nodiscard]] SinkId AddSink(Sink sink);
	void RemoveSink(SinkId sinkId);
	[[nodiscard]] static LogContext GetCurrentContext();

	void SetMinLogLevel(LogLevel level) { minLevel = level; }
	// GetMinLogLevel is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	LogLevel GetMinLogLevel() const { return minLevel; }
	// ShouldLog is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
	std::mutex sinkMutex;
	std::map<SinkId, Sink> sinks;
	std::atomic<SinkId> nextSinkId{1u};
	static thread_local LogContext currentContext;
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
