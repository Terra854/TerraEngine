// AtomicFileWrite.h — checked sibling staging for synchronous authored saves.
#pragma once

#include "Core/Export.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace TerraEngine::Utilities {
	// A checkpoint names one real operation. Native callers can reject that
	// operation for deterministic tests without replacing the filesystem backend.
	enum class AtomicFileWriteStage { Capture, Serialize, Open, Write, Flush, Finalize, Close, Replace, Complete };
	struct AtomicFileWriteOptions {
		std::function<std::string(AtomicFileWriteStage, const std::filesystem::path&)> checkpoint;
	};
	struct AtomicFileWriteResult {
		bool committed = false;
		AtomicFileWriteStage stage = AtomicFileWriteStage::Capture;
		std::string reason;
		std::string cleanupError;
	};
	// Different targets may save concurrently. The same normalized target keeps
	// its lock until this object dies, including the caller's cache publication.
	class TERRAENGINE_API AtomicFileWriteTransaction final {
	public:
		explicit AtomicFileWriteTransaction(const std::filesystem::path& target);
		~AtomicFileWriteTransaction();
		AtomicFileWriteTransaction(const AtomicFileWriteTransaction&) = delete;
		AtomicFileWriteTransaction& operator=(const AtomicFileWriteTransaction&) = delete;
		[[nodiscard]] AtomicFileWriteResult Commit(std::string_view bytes, const AtomicFileWriteOptions& options = {});
		[[nodiscard]] const std::filesystem::path& Target() const { return target; }
	private:
		std::filesystem::path target;
		std::shared_ptr<std::mutex> targetMutex;
		std::unique_lock<std::mutex> targetLock;
	};
}
