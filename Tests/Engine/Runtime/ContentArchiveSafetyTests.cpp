#include "TestHarness.h"
#include "Tests/Support/ArchiveSafetyFixtures.h"
#include "TEPAK/PackReader.h"
#include "Runtime/ContentMount/ContentMount.h"
#include <limits>
#include "codec/CodecProvider.h"
#include "io/PathNormalizer.h"
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#undef min
#undef max
#endif

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Tests::ArchiveSafety;
		template<class Action> bool Rejects(Action action) {
			try { action(); } catch(const std::exception&) { return true; }
			return false;
		}
		template<class Mutation> void RejectMutation(TestContext& context, Mutation mutation, bool rehash = true) {
			Fixture fixture; (void)fixture.Build(); Image image(fixture.PackPath());
			mutation(image); if(rehash) image.RehashMetadata(); image.Save(fixture.MutationPath());
			context.Expect(Rejects([&] { (void)tepak::PackReader::Open(fixture.MutationPath()); }), "Malformed persisted metadata must be rejected before exposing a reader.");
		}
		bool MakeDirectoryRedirect(const std::filesystem::path& link, const std::filesystem::path& target, bool junction) {
#ifdef _WIN32
			if(!junction) return CreateSymbolicLinkW(link.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE;
			// Junctions require no symbolic-link privilege and exercise reparse
			// protection independently from the optional symlink capability.
			std::filesystem::create_directory(link);
			HANDLE handle=CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
			if(handle==INVALID_HANDLE_VALUE) return false;
			struct JunctionData { DWORD tag; WORD length,reserved,subOffset,subLength,printOffset,printLength; wchar_t paths[2048]; } data{};
			const auto substitute=L"\\??\\"+std::filesystem::absolute(target).native(); const auto print=target.native();
			if(substitute.size()+print.size()+2>=2048){ CloseHandle(handle); return false; }
			data.tag=IO_REPARSE_TAG_MOUNT_POINT; data.subLength=static_cast<WORD>(substitute.size()*sizeof(wchar_t)); data.printOffset=data.subLength+sizeof(wchar_t); data.printLength=static_cast<WORD>(print.size()*sizeof(wchar_t));
			std::copy(substitute.begin(),substitute.end(),data.paths); std::copy(print.begin(),print.end(),data.paths+substitute.size()+1);
			data.length=static_cast<WORD>(8+(substitute.size()+print.size()+2)*sizeof(wchar_t)); DWORD returned=0;
			const bool ok=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,&data,8+data.length,nullptr,0,&returned,nullptr)!=FALSE; CloseHandle(handle); return ok;
#else
			(void)junction; std::error_code error; std::filesystem::create_directory_symlink(target,link,error); return !error;
#endif
		}

	}
	void RegisterContentArchiveSafetyTests(TestRegistry& registry) {
		registry.Add("Engine.ContentArchiveSafety.FailedReplacementPreservesMountedBytes", [](TestContext& context) {
			// A candidate rejection must retain the last complete public mount.
			Fixture fixture; (void)fixture.Build(); ContentMount mount;
			context.Expect(mount.Mount(fixture.PackPath()), "Writer archive must mount.");
			Image image(fixture.PackPath()); const auto e = image.Entry(image.FindEntry("nested/safe-content.bin"));
			auto x = image.Extent(e.firstExtentIndex); --x.storedSize; image.SetExtent(e.firstExtentIndex, x);
			image.RehashMetadata(); image.Save(fixture.MutationPath());
			context.Expect(!mount.Mount(fixture.MutationPath()), "Invalid candidate must not replace prior mount.");
			context.Expect(mount.GetArchivePath() == fixture.PackPath(), "Rejected replacement must retain prior archive identity.");
			context.Expect(mount.ReadAll("nested/safe-content.bin") == fixture.authored.at("nested/safe-content.bin"), "Rejected replacement must retain prior logical bytes.");
		});
		// Every mode uses the actual writer and compares authored logical bytes.
		registry.Add("Engine.ContentArchiveSafety.WriterVariantsReadExactBytes", [](TestContext& context) {
			for(const auto variant : {Variant::Raw, Variant::Compressed, Variant::Dictionary, Variant::Paged, Variant::Empty, Variant::EmptyTree}) {
				std::cout << "[CORPUS] archive writer variant " << static_cast<int>(variant) << std::endl;
				Fixture fixture; (void)fixture.Build(variant); const auto reader = tepak::PackReader::Open(fixture.PackPath());
				context.Expect(reader.GetEntries().size() == fixture.authored.size(), "Writer corpus must preserve every file identity.");
				for(const auto& [path, bytes] : fixture.authored) context.Expect(reader.ReadAll(path) == bytes, "Every writer mode must reconstruct exact authored bytes.");
			}
		});
		registry.Add("Engine.ContentArchiveSafety.ExactCrossExtentAndZeroRanges", [](TestContext& context) {
			Fixture fixture; (void)fixture.Build(); const auto reader = tepak::PackReader::Open(fixture.PackPath());
			const auto& bytes = fixture.authored.at("nested/safe-content.bin");
			context.Expect(reader.ReadRange("nested/safe-content.bin", 4000, 300) == Bytes(bytes.begin() + 4000, bytes.begin() + 4300), "Cross-extent range must be placed at exact logical offsets.");
			context.Expect(reader.ReadRange("nested/safe-content.bin", bytes.size(), 0).empty(), "End-of-file zero range succeeds.");
			context.Expect(reader.ReadAll("empty.bin").empty(), "Writer one-zero-extent file reconstructs empty bytes.");
		});
		registry.Add("Engine.ContentArchiveSafety.RangeOverflowRejected", [](TestContext& context) {
			Fixture fixture; (void)fixture.Build(); const auto reader = tepak::PackReader::Open(fixture.PackPath());
			context.Expect(Rejects([&] { (void)reader.ReadRange("nested/safe-content.bin", 1, std::numeric_limits<std::uint64_t>::max()); }), "Wrapping range size must be rejected before allocation or read.");
		});
		registry.Add("Engine.ContentArchiveSafety.TableCountCrossesNextSection", [](TestContext& context) {
			RejectMutation(context, [](Image& image) { auto h = image.Header(); ++h.directoryCount; image.SetHeader(h); });
		});
		registry.Add("Engine.ContentArchiveSafety.PathSliceWrapRejected", [](TestContext& context) {
			RejectMutation(context, [](Image& image) { auto e = image.Entry(0); e.pathOffset = std::numeric_limits<std::uint32_t>::max(); e.pathLength = 2; image.SetEntry(0, e); });
		});
		registry.Add("Engine.ContentArchiveSafety.ExtentReferenceWrapRejected", [](TestContext& context) {
			RejectMutation(context, [](Image& image) { auto e = image.Entry(0); e.firstExtentIndex = std::numeric_limits<std::uint32_t>::max(); e.entryExtentCount = 2; image.SetEntry(0, e); });
		});
		registry.Add("Engine.ContentArchiveSafety.ExtentGapRejected", [](TestContext& context) {
			RejectMutation(context, [](Image& image) { const auto e = image.Entry(image.FindEntry("nested/safe-content.bin")); auto x = image.Extent(e.firstExtentIndex + 1); ++x.fileOffset; image.SetExtent(e.firstExtentIndex + 1, x); });
		});
		registry.Add("Engine.ContentArchiveSafety.DuplicateExtentOwnershipRejected", [](TestContext& context) {
			RejectMutation(context, [](Image& image) { auto e = image.Entry(image.FindEntry("branch/other.bin")); const auto other = image.Entry(image.FindEntry("nested/safe-content.bin")); e.firstExtentIndex = other.firstExtentIndex; image.SetEntry(image.FindEntry("branch/other.bin"), e); });
		});
		registry.Add("Engine.ContentArchiveSafety.RawStoredSizeMismatchRejected", [](TestContext& context) {
			RejectMutation(context, [](Image& image) { const auto e = image.Entry(image.FindEntry("nested/safe-content.bin")); auto x = image.Extent(e.firstExtentIndex); --x.storedSize; image.SetExtent(e.firstExtentIndex, x); });
		});
		registry.Add("Engine.ContentArchiveSafety.UnknownCodecRejected", [](TestContext& context) {
			RejectMutation(context, [](Image& image) { auto x = image.Extent(0); x.codec = 255; image.SetExtent(0, x); });
		});
		registry.Add("Engine.ContentArchiveSafety.CompressedZeroStoredRejected", [](TestContext& context) {
			RejectMutation(context, [](Image& image) { const auto id = image.FindEntry("nested/safe-content.bin"); auto e = image.Entry(id); auto x = image.Extent(e.firstExtentIndex); e.storedSize -= x.storedSize; x.storedSize = 0; x.codec = 1; image.SetEntry(id, e); image.SetExtent(e.firstExtentIndex, x); });
		});
		registry.Add("Engine.ContentArchiveSafety.PhysicalOverrunAndTruncatedSectionsRejected", [](TestContext& context) {
			RejectMutation(context, [](Image& image) { auto x = image.Extent(0); x.storedOffset = image.Header().archiveSize; x.storedSize = 1; x.uncompressedSize = 1; image.SetExtent(0, x); });
			Fixture fixture; (void)fixture.Build(); Image image(fixture.PackPath()); image.bytes.resize(static_cast<std::size_t>(image.Header().pathTableOffset)); image.Save(fixture.MutationPath());
			context.Expect(Rejects([&] { (void)tepak::PackReader::Open(fixture.MutationPath()); }), "Truncated metadata is rejected before allocation.");
		});
		registry.Add("Engine.ContentArchiveSafety.OverlapUndershootAndUnsupportedSizesRejected", [](TestContext& context) {
			RejectMutation(context, [](Image& image) { const auto e = image.Entry(image.FindEntry("nested/safe-content.bin")); auto x = image.Extent(e.firstExtentIndex + 1); --x.fileOffset; image.SetExtent(e.firstExtentIndex + 1, x); });
			RejectMutation(context, [](Image& image) { const auto id = image.FindEntry("nested/safe-content.bin"); auto e = image.Entry(id); ++e.logicalSize; image.SetEntry(id, e); });
			RejectMutation(context, [](Image& image) { auto e = image.Entry(0); e.logicalSize = std::numeric_limits<std::uint64_t>::max(); image.SetEntry(0, e); });
			tepak::codec::CodecProvider codec;
			context.Expect(Rejects([&] { (void)codec.Decompress(Bytes{1,2}, tepak::StorageMode::Raw, 3, {}); }), "Direct Raw codec calls must reject length disagreement.");
		});
		registry.Add("Engine.ContentArchiveSafety.UnsafeStoredPathsRejected", [](TestContext& context) {
			// Longer Unicode words are not reserved COM/LPT device basenames.
			context.Expect(!Rejects([] { tepak::io::ValidateStoredPath("company\xc2\xb2.bin",false); }), "Legitimate Unicode basename must remain accepted.");

			// Rehash each structural mutation so path validation is the failing layer.
			for(const auto& path : std::vector<std::string>{"../outside.bin", "/root.bin", "c:escape.bin", "c:/escape.bin", "//host/share.bin", "\\\\?\\c:\\escape.bin", "nested\\alias.bin", "nested/./x.bin", "nested/../x.bin", "stream:payload", "nested/x. ", "con.bin", std::string("nul\0tail",8)}) {
				RejectMutation(context, [&](Image& image) { image.SetStoredPath(image.FindEntry("nested/safe-content.bin"), path); });
			}
		});
		registry.Add("Engine.ContentArchiveSafety.AliasCollisionRejected", [](TestContext& context) {
			RejectMutation(context, [](Image& image) { image.SetStoredPath(image.FindEntry("nested/safe-content.bin"), "branch/other.bin"); });
		});
		registry.Add("Engine.ContentArchiveSafety.ValidSubtreeExactOutputSet", [](TestContext& context) {
			Fixture fixture; (void)fixture.Build(); const auto reader = tepak::PackReader::Open(fixture.PackPath()); reader.Extract("nested", fixture.ExtractionRoot(), 2);
			std::size_t count = 0; for(const auto& item : std::filesystem::recursive_directory_iterator(fixture.ExtractionRoot())) if(item.is_regular_file()) ++count;
			context.Expect(count == 1 && ReadFile(fixture.ExtractionRoot()/"nested/safe-content.bin") == fixture.authored.at("nested/safe-content.bin"), "Subtree extraction visits exactly its one selected file.");
		});
		registry.Add("Engine.ContentArchiveSafety.TreeSelfCycleRejected", [](TestContext& context) {
			RejectMutation(context, [](Image& image) { auto d = image.Directory(1); d.firstChildDirectoryIndex = 1; d.childDirectoryCount = 1; image.SetDirectory(1,d); });
		});
		registry.Add("Engine.ContentArchiveSafety.TreeLongCycleRejected", [](TestContext& context) {
			RejectMutation(context, [](Image& image) { auto a = image.Directory(1), b = image.Directory(2); a.firstChildDirectoryIndex = 2; a.childDirectoryCount = 1; b.firstChildDirectoryIndex = 1; b.childDirectoryCount = 1; image.SetDirectory(1,a); image.SetDirectory(2,b); });
		});
		registry.Add("Engine.ContentArchiveSafety.TreeDisconnectedCycleRejected", [](TestContext& context) {
			RejectMutation(context, [](Image& image) { auto r = image.Directory(0); r.childDirectoryCount = 0; image.SetDirectory(0,r); auto a=image.Directory(1), b=image.Directory(2); a.firstChildDirectoryIndex=2; a.childDirectoryCount=1; b.firstChildDirectoryIndex=1; b.childDirectoryCount=1; image.SetDirectory(1,a); image.SetDirectory(2,b); });
		});
		registry.Add("Engine.ContentArchiveSafety.TreeInvalidRootAndParentRejected", [](TestContext& context) {
			RejectMutation(context, [](Image& image) { auto r=image.Directory(0); r.parentDirectoryId=0; image.SetDirectory(0,r); });
			RejectMutation(context, [](Image& image) { auto d=image.Directory(1); d.parentDirectoryId=std::numeric_limits<std::uint32_t>::max(); image.SetDirectory(1,d); });
		});
		registry.Add("Engine.ContentArchiveSafety.TreeDuplicateAndOrphanOwnershipRejected", [](TestContext& context) {
			RejectMutation(context, [](Image& image) { auto r=image.Directory(0); --r.childDirectoryCount; image.SetDirectory(0,r); });
			RejectMutation(context, [](Image& image) { auto a=image.Directory(1), b=image.Directory(2); b.firstChildFileIndex=a.firstChildFileIndex; b.childFileCount=a.childFileCount; image.SetDirectory(2,b); });
		});
		registry.Add("Engine.ContentArchiveSafety.TreeDeepValidExtractsOnce", [](TestContext& context) {
			// Synthesized writer-layout metadata avoids OS long-path source creation.
			// Its only file is at root; deep directory paths are never OS destinations.
			Fixture fixture; auto image = DeepTree(2048); image.Save(fixture.PackPath()); const auto reader=tepak::PackReader::Open(fixture.PackPath());
			context.Expect(reader.GetDirectories().size()==2048, "Every deep node must be accepted iteratively."); reader.Extract("",fixture.ExtractionRoot(),2);
			std::size_t count=0; for(const auto& item:std::filesystem::recursive_directory_iterator(fixture.ExtractionRoot())) if(item.is_regular_file()) ++count;
			context.Expect(count==1 && ReadFile(fixture.ExtractionRoot()/"root.bin").empty(), "Deep traversal must publish the root file exactly once.");
		});
		registry.Add("Engine.ContentArchiveSafety.MetadataAndDictionaryCorruptionRejected", [](TestContext& context) {
			Fixture fixture; (void)fixture.Build(); Image image(fixture.PackPath()); auto h=image.Header(); h.metadataHash^=1; image.SetHeader(h); image.Save(fixture.MutationPath());
			context.Expect(Rejects([&]{ (void)tepak::PackReader::Open(fixture.MutationPath()); }), "Metadata hash must be verified on mount.");
			image=Image(fixture.PackPath()); h=image.Header(); h.dictionaryHash=0; image.SetHeader(h); image.Save(fixture.MutationPath());
			context.Expect(Rejects([&]{ (void)tepak::PackReader::Open(fixture.MutationPath()); }), "Absent dictionary must use real XXH3 empty hash.");
			(void)fixture.Build(Variant::Dictionary); image=Image(fixture.PackPath()); image.Flip(image.Header().dictionaryOffset); image.Save(fixture.MutationPath());
			context.Expect(Rejects([&]{ (void)tepak::PackReader::Open(fixture.MutationPath()); }), "Dictionary byte corruption must reject mount.");
		});
		registry.Add("Engine.ContentArchiveSafety.TouchedPayloadAndEntryHashesVerified", [](TestContext& context) {
			for(const auto variant : {Variant::Raw, Variant::Compressed}) {
				Fixture fixture; (void)fixture.Build(variant); Image image(fixture.PackPath()); const auto e=image.Entry(image.FindEntry("nested/safe-content.bin")); image.CorruptExtentPayload(e.firstExtentIndex); image.Save(fixture.MutationPath()); const auto reader=tepak::PackReader::Open(fixture.MutationPath());
				context.Expect(Rejects([&]{ (void)reader.ReadAll("nested/safe-content.bin"); }), "Corrupt decoded bytes must fail full reads.");
			}
			Fixture fixture; (void)fixture.Build(); Image image(fixture.PackPath()); const auto id=image.FindEntry("nested/safe-content.bin"); auto e=image.Entry(id); e.entryHash^=1; image.SetEntry(id,e); image.RehashMetadata(); image.Save(fixture.MutationPath()); const auto reader=tepak::PackReader::Open(fixture.MutationPath());
			context.Expect(reader.ReadRange("nested/safe-content.bin",1,3)==Bytes(fixture.authored.at("nested/safe-content.bin").begin()+1,fixture.authored.at("nested/safe-content.bin").begin()+4), "Partial read verifies touched extents without demanding whole-file hash.");
			context.Expect(Rejects([&]{ (void)reader.ReadAll("nested/safe-content.bin"); }), "Full read verifies whole entry hash.");
		});
		registry.Add("Engine.ContentArchiveSafety.RangeIgnoresUnrelatedPoisonedExtents", [](TestContext& context) {
			Fixture fixture; (void)fixture.Build(); Image image(fixture.PackPath()); const auto e=image.Entry(image.FindEntry("nested/safe-content.bin")); image.CorruptExtentPayload(e.firstExtentIndex+1); image.Save(fixture.MutationPath()); const auto reader=tepak::PackReader::Open(fixture.MutationPath());
			const auto& bytes=fixture.authored.at("nested/safe-content.bin"); context.Expect(reader.ReadRange("nested/safe-content.bin",0,32)==Bytes(bytes.begin(),bytes.begin()+32), "Unrelated poisoned payload must stay untouched by a partial range.");
			context.Expect(Rejects([&]{ (void)reader.ReadRange("nested/safe-content.bin",4096,32); }), "The same poison must be rejected when actually touched.");
		});
		registry.Add("Engine.ContentArchiveSafety.EmptyExtentAndEntryHashesVerified", [](TestContext& context) {
			for(const bool extentHash : {false,true}) {
				Fixture fixture; (void)fixture.Build(Variant::Empty); Image image(fixture.PackPath());
				if(extentHash){ auto x=image.Extent(0); x.extentHash=0; image.SetExtent(0,x); } else { auto e=image.Entry(0); e.entryHash=0; image.SetEntry(0,e); }
				image.RehashMetadata(); image.Save(fixture.MutationPath()); const auto reader=tepak::PackReader::Open(fixture.MutationPath());
				context.Expect(reader.ReadRange("empty.bin",0,0).empty(), "Zero range is successful even for empty files.");
				context.Expect(Rejects([&]{ (void)reader.ReadAll("empty.bin"); }), "Full empty file must validate empty hashes.");
				// Exercise the runtime wrapper too: a full empty read must retain
				// full-entry verification, while an explicitly empty range stays lazy.
				ContentMount mounted;
				context.Expect(mounted.Mount(fixture.MutationPath()), "Payload hashes remain lazy during mounting.");
				context.Expect(mounted.ReadRangeChecked("empty.bin",0,0).Succeeded(), "An explicit empty mounted range remains valid.");
				context.Expect(mounted.ReadAllChecked("empty.bin").status == ContentReadStatus::ArchiveError,
					"The runtime full-read wrapper must reject corrupt empty extent and entry hashes.");
			}
		});
		registry.Add("Engine.ContentArchiveSafety.CorruptExtractionPreservesPriorFile", [](TestContext& context) {
			Fixture fixture; (void)fixture.Build(); Image image(fixture.PackPath()); const auto e=image.Entry(image.FindEntry("nested/safe-content.bin")); image.CorruptExtentPayload(e.firstExtentIndex); image.Save(fixture.MutationPath()); const auto reader=tepak::PackReader::Open(fixture.MutationPath()); const auto prior=Pattern(27); WriteFile(fixture.ExtractionRoot()/"nested/safe-content.bin",prior);
			context.Expect(Rejects([&]{ reader.Extract("nested",fixture.ExtractionRoot(),2); }), "Corrupt extraction must fail before publication."); context.Expect(ReadFile(fixture.ExtractionRoot()/"nested/safe-content.bin")==prior, "Prior destination must remain byte-identical after decode failure.");
		});

		registry.Add("Engine.ContentArchiveSafety.ParentSymlinkAndJunctionPreserveOutsideSentinel", [](TestContext& context) {
			std::size_t exercised=0;
			for(const bool junction : {false,true}) {
				Fixture fixture; (void)fixture.Build(); const auto before=fixture.OutsideSentinel("safe-content.bin"); std::filesystem::create_directories(fixture.ExtractionRoot());
				if(!MakeDirectoryRedirect(fixture.ExtractionRoot()/"nested",fixture.OutsideRoot(),junction)) { std::cout << "[UNAVAILABLE] " << (junction ? "junction" : "directory symlink") << " creation unavailable; containment was not exercised." << std::endl; continue; }
				++exercised; std::cout << "[LINK] parent " << (junction ? "junction" : "symlink") << " exercised" << std::endl;
				const auto reader=tepak::PackReader::Open(fixture.PackPath()); const bool rejected=Rejects([&]{ reader.Extract("nested",fixture.ExtractionRoot(),2); });
				context.Expect(ReadFile(fixture.OutsideRoot()/"safe-content.bin")==before,"Parent redirection must never alter outside bytes."); context.Expect(rejected,"Extraction must reject a redirected parent.");
			}
			context.Expect(exercised>0,"At least one real parent redirection must exercise containment.");
		});
		registry.Add("Engine.ContentArchiveSafety.FinalLinkPreservesOutsideSentinel", [](TestContext& context) {
			std::size_t exercised=0;
			for(const bool hardLink : {false,true}) {
				Fixture fixture; (void)fixture.Build(); const auto before=fixture.OutsideSentinel(); std::filesystem::create_directories(fixture.ExtractionRoot()/"nested"); std::error_code error;
				if(hardLink) std::filesystem::create_hard_link(fixture.OutsideRoot()/"sentinel.bin",fixture.ExtractionRoot()/"nested/safe-content.bin",error);
				else std::filesystem::create_symlink(fixture.OutsideRoot()/"sentinel.bin",fixture.ExtractionRoot()/"nested/safe-content.bin",error);
				if(error) { std::cout << "[UNAVAILABLE] " << (hardLink ? "hard link" : "file symlink") << " creation unavailable; containment was not exercised." << std::endl; continue; }
				++exercised; std::cout << "[LINK] final " << (hardLink ? "hardlink" : "symlink") << " exercised" << std::endl;
				const auto reader=tepak::PackReader::Open(fixture.PackPath()); const bool rejected=Rejects([&]{ reader.Extract("nested",fixture.ExtractionRoot(),2); });
				context.Expect(ReadFile(fixture.OutsideRoot()/"sentinel.bin")==before,"Final links must never truncate outside bytes."); context.Expect(rejected,"Extraction must reject an existing final link.");
			}
			context.Expect(exercised>0,"At least one real final link must exercise containment.");
		});

	}
}

