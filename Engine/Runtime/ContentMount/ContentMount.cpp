// ContentMount.cpp — Contains the Content Mount file-specific types, helper functions, and module integration code implementation for mounted content registration, lookup, and byte-loading services.

// ContentMount.cpp — Implementation of the TEPAK runtime content mount.
//
// This file integrates TEPAK's PackReader API into the engine runtime. The TEPAK
// first-party source code is copied verbatim into Engine/Runtime/ContentMount/ so the
// engine build is self-contained. TEPAK's third-party dependencies (zstd, xxhash)
// remain sourced from TEPAK/External/ as submodules.

#include "ContentMount.h"

#include "Core/Logger/Logger.h"

#include "TEPAK/PackReader.h"
#include "TEPAK/PackTypes.h"

#include <mutex>
#include <stdexcept>
#include <unordered_set>

// Hidden implementation — holds the TEPAK PackReader and associated state.
struct ContentMount::Impl {
	// The underlying TEPAK archive reader, opened via PackReader::Open().
	tepak::PackReader reader;

	// Path to the currently mounted .tepak archive file on disk.
	std::filesystem::path archivePath;

	// Whether a valid archive is currently mounted and ready for reads.
	bool mounted = false;

	// Quick-lookup set of normalised file paths inside the archive so
	// Exists() calls are O(1) without rescanning metadata tables.
	std::unordered_set<std::string> filePaths;

	// Guard concurrent mount/unmount operations from different engine threads.
	std::mutex mountMutex;
};

// --- Singleton ---

ContentMount& ContentMount::Instance() {
	static ContentMount instance;
	return instance;
}

// --- Constructors / assignment ---

ContentMount::ContentMount() : m_impl(std::make_unique<Impl>()) {}
ContentMount::~ContentMount() = default;
ContentMount::ContentMount(ContentMount&&) noexcept = default;
ContentMount& ContentMount::operator=(ContentMount&&) noexcept = default;

// --- Public API ---

bool ContentMount::Mount(const std::filesystem::path& archivePath) {
	std::scoped_lock lock(m_impl->mountMutex);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_impl->mounted) {
		TERRAENGINE_LOG(LogLevel::Warning, "ContentMount: Unmounting existing archive before mounting a new one.");
		Unmount();
	}

	try {
		m_impl->reader = tepak::PackReader::Open(archivePath);
	}
	catch (const std::exception& e) {
		TERRAENGINE_LOG(LogLevel::Error, "ContentMount: Failed to open TEPAK archive at "
			<< archivePath.string() << " — " << e.what());
		return false;
	}

	// Build the quick-lookup set from all entries in the archive.
	m_impl->filePaths.clear();
	for (const auto& entry : m_impl->reader.GetEntries()) {
		m_impl->filePaths.insert(entry.normalizedPath);
	}

	m_impl->archivePath = archivePath;
	m_impl->mounted = true;

	TERRAENGINE_LOG(LogLevel::Info, "ContentMount: Mounted TEPAK archive '"
		<< archivePath.filename().string() << "' with "
		<< m_impl->filePaths.size() << " entries.");

	return true;
}

// ContentMount::IsMounted is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ContentMount::IsMounted() const {
	return m_impl->mounted;
}

// ContentMount::GetArchivePath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const std::filesystem::path& ContentMount::GetArchivePath() const {
	return m_impl->archivePath;
}

// ContentMount::NormalizeLogicalPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ContentMount::NormalizeLogicalPath(std::string_view logicalPath) {
	std::string normalized(logicalPath);
	for (char& c : normalized) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (c == '\\') c = '/';
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
	}

	std::string_view lookup = normalized;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (!lookup.empty() && lookup.front() == '/') {
		lookup.remove_prefix(1);
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (lookup.starts_with("./")) {
		lookup.remove_prefix(2);
	}

	return std::string(lookup);
}

// ContentMount::Exists is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ContentMount::Exists(std::string_view logicalPath) const {
	if (!m_impl->mounted) {
		return false;
	}

	return m_impl->filePaths.count(NormalizeLogicalPath(logicalPath)) > 0;
}

// ContentMount::ReadAll is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<uint8_t> ContentMount::ReadAll(std::string_view logicalPath) const {
	if (!m_impl->mounted) {
		TERRAENGINE_LOG(LogLevel::Error, "ContentMount: Cannot read — no archive is mounted.");
		return {};
	}

	try {
		// TEPAK's ReadAll returns a ByteBuffer (std::vector<uint8_t>) which is
		// exactly what the engine resource managers expect.
		return m_impl->reader.ReadAll(NormalizeLogicalPath(logicalPath));
	}
	catch (const std::exception& e) {
		TERRAENGINE_LOG(LogLevel::Error, "ContentMount: Failed to read '"
			<< std::string(logicalPath) << "' — " << e.what());
		return {};
	}
}

// ContentMount::ReadRange is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<uint8_t> ContentMount::ReadRange(std::string_view logicalPath, uint64_t offset, uint64_t size) const {
	if (!m_impl->mounted) {
		TERRAENGINE_LOG(LogLevel::Error, "ContentMount: Cannot read range — no archive is mounted.");
		return {};
	}

	try {
		return m_impl->reader.ReadRange(NormalizeLogicalPath(logicalPath), offset, size);
	}
	catch (const std::exception& e) {
		TERRAENGINE_LOG(LogLevel::Error, "ContentMount: Failed to read range from '"
			<< std::string(logicalPath) << "' — " << e.what());
		return {};
	}
}

// ContentMount::Unmount is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ContentMount::Unmount() {
	std::scoped_lock lock(m_impl->mountMutex);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!m_impl->mounted) {
		return;
	}

	// Release the TEPAK reader by assigning a default-constructed instance.
	// shared_ptr inside PackReader will drop the implementation and all memory.
	m_impl->reader = tepak::PackReader{};
	m_impl->filePaths.clear();
	m_impl->archivePath.clear();
	m_impl->mounted = false;

	TERRAENGINE_LOG(LogLevel::Info, "ContentMount: Unmounted TEPAK archive.");
}
