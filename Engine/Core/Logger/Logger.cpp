// Logger.cpp — Implementation of the engine's multi-threaded logging system.
//
// This file implements the message routing, formatting, and worker thread logic
// for the engine's logger. It handles timestamping, log level colorization, and
// cleans up function signatures for both native and managed (C#) log entries to
// provide clean console output.

#include "Logger.h"

thread_local LogContext Logger::currentContext{};

Logger& Logger::Instance()
{
	// Native managed-call exports and editor subscribers share this one owner,
	// so gameplay messages reach the same sinks as editor-originated messages.
	static Logger instance;
	return instance;
}

Logger::ScopedContext::ScopedContext(LogContext context)
	: previous(Logger::currentContext)
{
	Logger::currentContext = std::move(context);
}

Logger::ScopedContext::~ScopedContext()
{
	Logger::currentContext = std::move(previous);
}

void Logger::Log(LogLevel level, const std::string& file, int line, const std::string& functionSignature, const std::string& message, bool isManaged)
{
	LogEntry entry;
	entry.timestamp = std::chrono::system_clock::now();
	entry.level = level;
	entry.file = isManaged ? file : ExtractFileName(file);
	entry.line = line;
	
	std::string cleanedSignature = functionSignature;

	if (!isManaged) {
		// Simple cleanup for __PRETTY_FUNCTION__ (GCC/Clang) or __FUNCSIG__ (MSVC)
		// We want to remove the return type and arguments, while preserving template parameters.
		
		// 1. Remove arguments: 
		// "void Class<T>::Method(int)" -> "void Class<T>::Method"
		// "void Class::Method<T>(int)" -> "void Class::Method<T>"
		// Note: we must find the LAST '(', because templates can contain '(' in some complex cases, 
		// but the argument list always ends with ')' and starts after the method name.
		// A better approach is to find the '(' that is NOT inside '< >'.
		
		size_t parenPos = std::string::npos;
		int angleBracketDepth = 0;

		for (size_t i = 0; i < cleanedSignature.length(); ++i) {

			if (cleanedSignature[i] == '<') angleBracketDepth++;
			else if (cleanedSignature[i] == '>') angleBracketDepth--;
			else if (cleanedSignature[i] == '(' && angleBracketDepth == 0) {
				parenPos = i;
				break;
			}
		}


		if (parenPos != std::string::npos) {
			cleanedSignature = cleanedSignature.substr(0, parenPos);
		}

		// 2. Trim trailing spaces
		size_t lastNonSpace = cleanedSignature.find_last_not_of(" \t\r\n");
		if (lastNonSpace != std::string::npos) {
			cleanedSignature = cleanedSignature.substr(0, lastNonSpace + 1);
		} else {
			cleanedSignature.clear();
		}
		
		// 3. Remove return type: "void Class<T>::Method" -> "Class<T>::Method"
		// Again, find the space that is NOT inside '< >'.
		size_t lastSpace = std::string::npos;
		angleBracketDepth = 0;
		for (int i = (int)cleanedSignature.length() - 1; i >= 0; --i) {

			if (cleanedSignature[i] == '>') angleBracketDepth++;
			else if (cleanedSignature[i] == '<') angleBracketDepth--;
			else if (cleanedSignature[i] == ' ' && angleBracketDepth == 0) {
				lastSpace = i;
				break;
			}
		}


		if (lastSpace != std::string::npos) {
			cleanedSignature = cleanedSignature.substr(lastSpace + 1);
		}

		// 4. Remove redundant keywords
		auto removeKeyword = [&](const std::string& keyword) {
			size_t pos = 0;
			while ((pos = cleanedSignature.find(keyword, pos)) != std::string::npos) {
				cleanedSignature.erase(pos, keyword.length());
			}
		};
		removeKeyword("struct ");
		removeKeyword("class ");
		removeKeyword("enum ");
		removeKeyword("const ");
		removeKeyword("volatile ");
	}


	if (cleanedSignature.empty()) {
		cleanedSignature = "Unknown";
	}

	entry.functionSignature = cleanedSignature;
	entry.message = message;
	entry.isManaged = isManaged;
	const LogContext context = GetCurrentContext();
	entry.category = context.category;
	entry.source = context.source;
	entry.correlationId = context.correlationId;
	entry.requestId = context.requestId;
	entry.jobId = context.jobId;

	{
		// Holding the sink lock through callbacks gives RemoveSink a strict lifetime
		// boundary. Sinks must only copy/queue the entry and must not log recursively.
		std::lock_guard<std::mutex> lock(sinkMutex);
		for (const auto& [sinkId, sink] : sinks)
		{
			(void)sinkId;
			if (sink)
				sink(entry);
		}
	}

	bool queued = false;
	{
		// Queue admission and terminal publication use one linearization lock.
		// Shutdown never leaves a newly accepted message without a consumer.
		std::lock_guard<std::mutex> lock(m);
		if (!done.load(std::memory_order_relaxed))
		{
			q.push(std::move(entry));
			queued = true;
		}
	}
	if (queued) cv.notify_one();
	else
	{
		// Terminal reuse is synchronous and bounded by this caller's entry, not
		// a restarted/detached worker or an orphan queue. Sink callbacks already
		// received this entry once; never invoke them again from console output.
		PublishConsoleEntry(entry);
	}
}

Logger::SinkId Logger::AddSink(Sink sink)
{
	if (!sink)
		return 0u;
	const SinkId sinkId = nextSinkId.fetch_add(1u, std::memory_order_relaxed);
	std::lock_guard<std::mutex> lock(sinkMutex);
	sinks.emplace(sinkId, std::move(sink));
	return sinkId;
}

void Logger::RemoveSink(SinkId sinkId)
{
	if (sinkId == 0u)
		return;
	std::lock_guard<std::mutex> lock(sinkMutex);
	sinks.erase(sinkId);
}

LogContext Logger::GetCurrentContext()
{
	return currentContext;
}


void Logger::Shutdown()
{
	// Only one caller may inspect/join the std::thread at a time. The consumer
	// never invokes user sinks, so a synchronous sink may call Shutdown without
	// joining a worker that needs sinkMutex. Queue ownership is released first.
	std::lock_guard<std::mutex> shutdownLock(shutdownMutex);
	{
		std::lock_guard<std::mutex> queueLock(m);
		done.store(true, std::memory_order_relaxed);
	}
	cv.notify_one();
	if (t.joinable()) t.join();
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

		PublishConsoleEntry(entry);
	}
}

void Logger::PublishConsoleEntry(const LogEntry& entry)
{
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
	if (entry.isManaged) {
		logStream << "[" << timeStream.str() << "][" << LogLevelToString(entry.level) << "][C#]["
			<< entry.functionSignature << ":" << entry.line << "] "
			<< entry.message;
	}
	else {
		logStream << "[" << timeStream.str() << "][" << LogLevelToString(entry.level) << "]["
			<< entry.functionSignature << ":" << entry.line << "] "
			<< entry.message;
	}

	// Get the appropriate color code and then output the message.
	std::string color = GetColorCode(entry.level);
	std::string resetColor = "\033[0m";  // ANSI reset code

	// Serialize only final console emission, after formatting and outside queue
	// and sink locks. A standard mutex also works on C++20 libraries that still
	// require experimental flags for syncstream; terminal callers use this lock too.
	std::lock_guard<std::mutex> consoleLock(consoleMutex);
	std::cout << color << logStream.str() << resetColor << std::endl;
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
