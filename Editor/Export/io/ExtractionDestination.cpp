#include "io/ExtractionDestination.h"
#include "io/PathNormalizer.h"
#include "Utilities/UUID.h"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <utility>
#include <vector>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <tuple>
#include <stdexcept>
#include <unordered_set>
#ifdef _WIN32
#include <windows.h>
#undef min
#undef max
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace tepak::io {
	struct ExtractionDestination::Impl {
#ifdef _WIN32
		struct Anchor {
			HANDLE handle = INVALID_HANDLE_VALUE;
			explicit Anchor(HANDLE value) : handle(value) {}
			Anchor(Anchor&& other) noexcept : handle(std::exchange(other.handle,INVALID_HANDLE_VALUE)) {}
			Anchor(const Anchor&)=delete;
			~Anchor() { if(handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
		};
		using Identity=std::tuple<DWORD,DWORD,DWORD>;
		static Identity FileIdentity(const BY_HANDLE_FILE_INFORMATION& info) { return {info.dwVolumeSerialNumber,info.nFileIndexHigh,info.nFileIndexLow}; }
		std::map<Identity,std::filesystem::path> directoryIdentities;
		mutable std::map<Identity,std::string> fileIdentities;
		mutable std::mutex publicationMutex;
		std::map<std::filesystem::path, Anchor> anchors;
		std::map<std::string, std::filesystem::path> files;
		void HoldDirectory(const std::filesystem::path& path) {
			if(anchors.contains(path)) return;
			// Deny write as well as delete sharing: a writable handle could turn
			// an empty directory into a reparse point without renaming it.
			auto open = [&] { return CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr); };
			HANDLE handle = open();
			if(handle == INVALID_HANDLE_VALUE && (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)) {
				if(!CreateDirectoryW(path.c_str(),nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) throw std::runtime_error("Could not create guarded extraction directory.");
				handle = open();
			}
			if(handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not anchor extraction directory.");
			Anchor owned(handle); BY_HANDLE_FILE_INFORMATION info{};
			if(!GetFileInformationByHandle(handle,&info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Extraction directory is redirected or unavailable.");
			if(!directoryIdentities.emplace(FileIdentity(info),path).second) throw std::runtime_error("Extraction directory paths alias the same filesystem object.");
			anchors.emplace(path,std::move(owned));
		}
		static std::optional<Identity> CheckFinal(const std::filesystem::path& path) {
			HANDLE handle=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
			if(handle == INVALID_HANDLE_VALUE) {
				if(GetLastError()==ERROR_FILE_NOT_FOUND) return std::nullopt;
				throw std::runtime_error("Could not inspect extraction destination.");
			}
			Anchor owned(handle); BY_HANDLE_FILE_INFORMATION info{};
			if(!GetFileInformationByHandle(handle,&info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) || info.nNumberOfLinks != 1) throw std::runtime_error("Extraction destination is a link or nonregular file.");
			return FileIdentity(info);
		}
#else
		struct Anchor { int fd=-1; explicit Anchor(int value):fd(value){} Anchor(Anchor&& other) noexcept:fd(std::exchange(other.fd,-1)){} Anchor(const Anchor&)=delete; ~Anchor(){ if(fd>=0) close(fd); } };
		std::map<std::string,Anchor> anchors;
		std::map<std::string,std::pair<int,std::string>> files;
		int HoldChild(int parent,const std::string& name,const std::string& key) {
			if(const auto found=anchors.find(key); found!=anchors.end()) return found->second.fd;
			int fd=openat(parent,name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
			if(fd<0 && errno==ENOENT) { if(mkdirat(parent,name.c_str(),0700)<0 && errno!=EEXIST) throw std::runtime_error("Could not create extraction directory."); fd=openat(parent,name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC); }
			if(fd<0) throw std::runtime_error("Extraction directory is redirected or unavailable.");
			Anchor owned(fd); anchors.emplace(key,std::move(owned)); return fd;
		}
		static void CheckFinal(int parent,const std::string& name) {
			struct stat info{}; if(fstatat(parent,name.c_str(),&info,AT_SYMLINK_NOFOLLOW)<0) { if(errno==ENOENT) return; throw std::runtime_error("Could not inspect extraction destination."); }
			if(!S_ISREG(info.st_mode) || info.st_nlink!=1) throw std::runtime_error("Extraction destination is a link or nonregular file.");
		}
#endif
	};
	ExtractionDestination::ExtractionDestination(const std::filesystem::path& root,std::span<const std::string> paths) : impl(std::make_unique<Impl>()) {
		std::unordered_set<std::string> identities;
		// This whole pass precedes directory creation, including cross-file aliases.
		for(const auto& path:paths) { ValidateStoredPath(path,false); if(!identities.insert(StoredPathIdentity(path)).second) throw std::runtime_error("Extraction paths collide."); }
#ifdef _WIN32
		const auto absolute=std::filesystem::absolute(root).lexically_normal(); auto current=absolute.root_path(); impl->HoldDirectory(current);
		for(const auto& part:absolute.relative_path()) { current/=part; impl->HoldDirectory(current); }
		for(const auto& path:paths) {
			const auto relative=std::filesystem::u8path(path); auto parent=absolute;
			for(const auto& part:relative.parent_path()) { parent/=part; impl->HoldDirectory(parent); }
			const auto target=parent/relative.filename();
			if(const auto identity=Impl::CheckFinal(target); identity && !impl->fileIdentities.emplace(*identity,path).second) throw std::runtime_error("Extraction file paths alias the same filesystem object.");
			impl->files.emplace(path,target);
		}
#else
		const auto absolute=std::filesystem::absolute(root).lexically_normal(); int parent=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC); if(parent<0) throw std::runtime_error("Could not anchor filesystem root.");
		Impl::Anchor rootAnchor(parent); impl->anchors.emplace("/",std::move(rootAnchor)); std::string key;
		for(const auto& part:absolute.relative_path()) { key+="/"+part.string(); parent=impl->HoldChild(parent,part.string(),key); }
		for(const auto& path:paths) {
			const auto relative=std::filesystem::u8path(path); int directory=parent; std::string childKey=key;
			for(const auto& part:relative.parent_path()) { childKey+="/"+part.string(); directory=impl->HoldChild(directory,part.string(),childKey); }
			const auto name=relative.filename().string(); Impl::CheckFinal(directory,name); impl->files.emplace(path,std::pair{directory,name});
		}
#endif
	}
	ExtractionDestination::~ExtractionDestination() = default;
	void ExtractionDestination::Publish(std::string_view relativePath,std::span<const std::uint8_t> bytes) const {
		const auto found=impl->files.find(std::string(relativePath)); if(found==impl->files.end()) throw std::runtime_error("Extraction path was not preflighted.");
		const std::string temporary=".tepak-"+TerraEngine::ToHexString(TerraEngine::GenerateUUID())+".tmp";
#ifdef _WIN32
		const auto& target=found->second; Impl::CheckFinal(target); const auto staged=target.parent_path()/temporary;
		HANDLE handle=CreateFileW(staged.c_str(),GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
		if(handle==INVALID_HANDLE_VALUE) throw std::runtime_error("Could not create private extraction file.");
		// Keep the exact staging object exclusive through rename. Cleanup uses
		// its handle too, never a pathname that another writer could replace.
		struct Stage { HANDLE handle; bool published=false; ~Stage(){ if(!published){ FILE_DISPOSITION_INFO disposition{TRUE}; SetFileInformationByHandle(handle,FileDispositionInfo,&disposition,sizeof(disposition)); } CloseHandle(handle); } } stage{handle};
		std::size_t offset=0; while(offset<bytes.size()) { const DWORD count=static_cast<DWORD>(std::min<std::size_t>(bytes.size()-offset,std::numeric_limits<DWORD>::max())); DWORD written=0; if(!WriteFile(handle,bytes.data()+offset,count,&written,nullptr) || written!=count) throw std::runtime_error("Could not write verified extraction bytes."); offset+=written; }
		if(!FlushFileBuffers(handle)) throw std::runtime_error("Could not flush verified extraction bytes.");
		const auto& name=target.native();
		if(name.size()>(std::numeric_limits<DWORD>::max()-sizeof(FILE_RENAME_INFO))/sizeof(wchar_t)-1) throw std::runtime_error("Extraction destination exceeds rename limits.");
		std::vector<std::uint8_t> storage(sizeof(FILE_RENAME_INFO)+(name.size()+1)*sizeof(wchar_t));
		auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(storage.data()); rename->ReplaceIfExists=TRUE; rename->RootDirectory=nullptr; rename->FileNameLength=static_cast<DWORD>(name.size()*sizeof(wchar_t));
		std::memcpy(rename->FileName,name.c_str(),(name.size()+1)*sizeof(wchar_t));
		// Decoding and staging remain parallel; serialize only final replacement
		// so a newly generated short filename cannot alias a sibling publication.
		std::scoped_lock publication(impl->publicationMutex);
		if(const auto identity=Impl::CheckFinal(target)) {
			const auto prior=impl->fileIdentities.find(*identity);
			if(prior!=impl->fileIdentities.end() && prior->second!=relativePath) throw std::runtime_error("Extraction file aliases another selected path.");
		}
		BY_HANDLE_FILE_INFORMATION stagedInfo{};
		if(!GetFileInformationByHandle(handle,&stagedInfo)) throw std::runtime_error("Could not inspect verified staging identity.");
		// Reserve the identity bookkeeping before the no-fail post-rename flag.
		const auto [identity,inserted]=impl->fileIdentities.emplace(Impl::FileIdentity(stagedInfo),std::string(relativePath));
		if(!inserted) throw std::runtime_error("Verified staging identity is already owned.");
		struct Reservation {
			std::map<Impl::Identity,std::string>& identities;
			std::map<Impl::Identity,std::string>::iterator entry;
			bool committed=false;
			~Reservation(){ if(!committed) identities.erase(entry); }
		} reservation{impl->fileIdentities,identity};
		if(!SetFileInformationByHandle(handle,FileRenameInfo,rename,static_cast<DWORD>(storage.size()))) throw std::runtime_error("Could not publish verified extraction file by handle.");
		stage.published=true; reservation.committed=true;
#else
		// POSIX directory-relative no-follow operations protect pre-existing links.
		// Same-user concurrent replacement of a staging directory entry remains an
		// unverified platform limitation until dedicated Linux race validation.
		const auto& [parent,name]=found->second; Impl::CheckFinal(parent,name); int fd=openat(parent,temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600); if(fd<0) throw std::runtime_error("Could not create private extraction file.");
		struct Stage { int fd,parent; const std::string& name; bool published=false; ~Stage(){ if(fd>=0) close(fd); if(!published) unlinkat(parent,name.c_str(),0); } } stage{fd,parent,temporary};
		std::size_t offset=0; while(offset<bytes.size()) { const auto written=write(fd,bytes.data()+offset,std::min<std::size_t>(bytes.size()-offset,std::numeric_limits<ssize_t>::max())); if(written<0 && errno==EINTR) continue; if(written<=0) throw std::runtime_error("Could not write verified extraction bytes."); offset+=static_cast<std::size_t>(written); }
		if(fsync(fd)<0) throw std::runtime_error("Could not flush verified extraction bytes."); close(fd); stage.fd=-1; Impl::CheckFinal(parent,name); if(renameat(parent,temporary.c_str(),parent,name.c_str())<0) throw std::runtime_error("Could not publish verified extraction file."); stage.published=true;
#endif
	}
}
