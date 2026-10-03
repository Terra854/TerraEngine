// One invocation owns its child tree and captured output until settlement.
// Call Run on an existing task worker; this helper creates no executor/thread.
#pragma once
#include "Core/Export.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <string>

namespace TerraEngine::Utilities {
	struct OwnedProcessRequest {
		std::filesystem::path workingDirectory;
		std::string commandLine;
		// UTF-8 overrides are merged into a child-only copy of the environment.
		std::map<std::string, std::string> environment;
		std::chrono::milliseconds timeout = std::chrono::minutes(5);
	};
	struct OwnedProcessResult {
		bool started = false;
		bool canceled = false;
		bool timedOut = false;
		int exitCode = -1;
		std::string output;
	};
	class TERRAENGINE_API OwnedProcess final {
	public:
		// Request and cancellation outlive this synchronous invocation. Native
		// handles/groups are locally owned and cleaned even if capture throws.
		static OwnedProcessResult Run(const OwnedProcessRequest& request, const std::atomic_bool& cancellation);
	};
}
