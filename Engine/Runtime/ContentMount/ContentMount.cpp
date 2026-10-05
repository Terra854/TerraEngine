// ContentMount.cpp - synchronized TEPAK reader publication and checked reads.

#include "ContentMount.h"

#include "Core/Logger/Logger.h"
#include "TEPAK/PackReader.h"
#include "TEPAK/PackTypes.h"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>

struct ContentMount::Impl final
{
	// Readers keep this shared lock through decompression. Mount and Unmount take
	// the unique side, so neither can replace the PackReader during an active read.
	mutable std::shared_mutex archiveMutex;
	tepak::PackReader reader;
	std::filesystem::path archivePath;
	std::unordered_map<std::string, MountedContentEntry> entries;
	bool mounted = false;
};

namespace
{
	MountedContentEntry CopyEntry(const tepak::PackEntry& entry)
	{
		return {entry.normalizedPath, entry.assetClass, entry.logicalSize, entry.entryHash};
	}

	template<typename ImplType>
	MountedContentReadResult ReadCheckedLocked(
		const ImplType& impl,
		std::string_view logicalPath,
		std::uint64_t offset,
		std::optional<std::uint64_t> requestedSize,
		std::uint64_t maximumReadBytes)
	{
		MountedContentReadResult result;
		if(!impl.mounted)
		{
			result.status = ContentReadStatus::NotMounted;
			return result;
		}

		const std::string normalized = ContentMount::NormalizeLogicalPath(logicalPath);
		const auto found = impl.entries.find(normalized);
		if(found == impl.entries.end())
		{
			result.status = ContentReadStatus::NotFound;
			return result;
		}
		result.entry = found->second;

		const std::uint64_t size = requestedSize.value_or(result.entry.logicalSize);
		if(offset > result.entry.logicalSize || size > result.entry.logicalSize - offset)
		{
			result.status = ContentReadStatus::OutOfRange;
			return result;
		}
		if(size > maximumReadBytes)
		{
			result.status = ContentReadStatus::ReadLimitExceeded;
			return result;
		}

		try
		{
			// Full reads verify the complete entry even when its logical size is
			// zero. An explicitly requested empty range intentionally stays lazy.
			result.bytes = requestedSize.has_value()
				? impl.reader.ReadRange(normalized, offset, size)
				: impl.reader.ReadAll(normalized);
			if(result.bytes.size() != size)
			{
				result.bytes.clear();
				result.status = ContentReadStatus::ArchiveError;
				return result;
			}
			result.status = ContentReadStatus::Ok;
		}
		catch(const std::exception& exception)
		{
			TERRAENGINE_LOG(LogLevel::Error, "ContentMount: checked read failed for '"
				<< normalized << "': " << exception.what());
			result.bytes.clear();
			result.status = ContentReadStatus::ArchiveError;
		}
		return result;
	}
}

ContentMount& ContentMount::Instance()
{
	static ContentMount instance;
	return instance;
}

ContentMount::ContentMount()
	: m_impl(std::make_unique<Impl>())
{
}

ContentMount::~ContentMount() = default;
ContentMount::ContentMount(ContentMount&&) noexcept = default;
ContentMount& ContentMount::operator=(ContentMount&&) noexcept = default;

bool ContentMount::Mount(const std::filesystem::path& archivePath)
{
	// Candidate construction intentionally happens without the live-mount lock.
	// Existing worker reads continue while the new archive validates and indexes.
	tepak::PackReader candidateReader;
	std::filesystem::path candidatePath;
	std::unordered_map<std::string, MountedContentEntry> candidateEntries;
	try
	{
		// Path copying may allocate; finish it before publishing any live fields.
		candidatePath = archivePath;
		candidateReader = tepak::PackReader::Open(candidatePath);
		for(const tepak::PackEntry& entry : candidateReader.GetEntries())
		{
			candidateEntries.emplace(entry.normalizedPath, CopyEntry(entry));
		}
	}
	catch(const std::exception& exception)
	{
		TERRAENGINE_LOG(LogLevel::Error, "ContentMount: failed to open TEPAK archive at "
			<< archivePath.string() << ": " << exception.what());
		return false;
	}

	{
		std::unique_lock lock(m_impl->archiveMutex);
		m_impl->reader = std::move(candidateReader);
		m_impl->entries = std::move(candidateEntries);
		m_impl->archivePath.swap(candidatePath);
		m_impl->mounted = true;
	}

	TERRAENGINE_LOG(LogLevel::Info, "ContentMount: mounted TEPAK archive '"
		<< archivePath.filename().string() << "'.");
	return true;
}

bool ContentMount::IsMounted() const
{
	std::shared_lock lock(m_impl->archiveMutex);
	return m_impl->mounted;
}

std::filesystem::path ContentMount::GetArchivePath() const
{
	std::shared_lock lock(m_impl->archiveMutex);
	return m_impl->archivePath;
}

std::string ContentMount::NormalizeLogicalPath(std::string_view logicalPath)
{
	std::string normalized(logicalPath);
	for(char& character : normalized)
	{
		if(character == '\\') character = '/';
		if(character >= 'A' && character <= 'Z')
		{
			character = static_cast<char>(character + ('a' - 'A'));
		}
	}

	std::string_view lookup = normalized;
	while(!lookup.empty() && lookup.front() == '/') lookup.remove_prefix(1U);
	while(lookup.starts_with("./")) lookup.remove_prefix(2U);
	return std::string(lookup);
}

bool ContentMount::Exists(std::string_view logicalPath) const
{
	return FindEntry(logicalPath).has_value();
}

std::optional<MountedContentEntry> ContentMount::FindEntry(std::string_view logicalPath) const
{
	std::shared_lock lock(m_impl->archiveMutex);
	if(!m_impl->mounted) return std::nullopt;
	const auto found = m_impl->entries.find(NormalizeLogicalPath(logicalPath));
	if(found == m_impl->entries.end()) return std::nullopt;
	return found->second;
}

std::vector<MountedContentEntry> ContentMount::SnapshotEntries() const
{
	std::shared_lock lock(m_impl->archiveMutex);
	std::vector<MountedContentEntry> result;
	if(!m_impl->mounted) return result;
	result.reserve(m_impl->entries.size());
	for(const auto& [path, entry] : m_impl->entries)
	{
		(void)path;
		result.push_back(entry);
	}
	std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
		return left.logicalPath < right.logicalPath;
	});
	return result;
}

MountedContentReadResult ContentMount::ReadRangeChecked(
	std::string_view logicalPath,
	std::uint64_t offset,
	std::uint64_t size,
	std::uint64_t maximumReadBytes) const
{
	std::shared_lock lock(m_impl->archiveMutex);
	return ReadCheckedLocked(*m_impl, logicalPath, offset, size, maximumReadBytes);
}

MountedContentReadResult ContentMount::ReadAllChecked(
	std::string_view logicalPath,
	std::uint64_t maximumReadBytes) const
{
	std::shared_lock lock(m_impl->archiveMutex);
	return ReadCheckedLocked(*m_impl, logicalPath, 0U, std::nullopt, maximumReadBytes);
}

std::vector<std::uint8_t> ContentMount::ReadAll(std::string_view logicalPath) const
{
	return ReadAllChecked(logicalPath).bytes;
}

std::vector<std::uint8_t> ContentMount::ReadRange(
	std::string_view logicalPath,
	std::uint64_t offset,
	std::uint64_t size) const
{
	return ReadRangeChecked(logicalPath, offset, size).bytes;
}

void ContentMount::Unmount()
{
	std::unique_lock lock(m_impl->archiveMutex);
	if(!m_impl->mounted) return;
	m_impl->reader = tepak::PackReader{};
	m_impl->entries.clear();
	m_impl->archivePath.clear();
	m_impl->mounted = false;
	TERRAENGINE_LOG(LogLevel::Info, "ContentMount: unmounted TEPAK archive.");
}
