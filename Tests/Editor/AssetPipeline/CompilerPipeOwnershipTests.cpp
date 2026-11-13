// CompilerPipeOwnershipTests.cpp - exercise the real AssetDatabase launcher.
#include "TestHarness.h"
#include "AssetDatabase.h"
#include "ThreadPool/ThreadPool.h"
#include "Utilities/PlatformUtils.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <iostream>

#if defined(_WIN32)
#include <windows.h>

// Required parent-owned native-only friend declaration in AssetDatabase.h:
// friend struct AssetDatabaseCompilerProcessTestAccess;
// This invokes the real production method; no compilation-substitution hook is used.
struct AssetDatabaseCompilerProcessTestAccess {
	static bool Run(const std::filesystem::path& exe, const std::wstring& args, const std::filesystem::path& receipt) {
		return AssetDatabase::Instance().RunCompilerProcess(exe, args, nullptr, "Compiler ownership fixture", receipt);
	}
};
#endif

namespace TerraEngine::EditorTests {
#if defined(_WIN32)
namespace {
	struct Handle {
		HANDLE value = nullptr;
		explicit Handle(HANDLE handle) : value(handle) { if(!value) throw std::runtime_error("Cannot create fixture event."); }
		~Handle() { CloseHandle(value); }
		Handle(const Handle&) = delete;
		Handle& operator=(const Handle&) = delete;
	};
	struct FixtureRoot {
		std::filesystem::path path;
		FixtureRoot() {
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			path = std::filesystem::temp_directory_path() / ("TerraCompilerOwnership-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(stamp));
			if(!std::filesystem::create_directory(path)) throw std::runtime_error("Cannot reserve compiler ownership fixture.");
		}
		~FixtureRoot() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
	};
	std::filesystem::path FixtureExecutable(TestContext& context) {
		// Parent CMake puts the child beside both built and installed runners.
		// Keep only its configured name: a missing installed fixture must fail
		// instead of accidentally launching a surviving build-tree executable.
		const auto name = std::filesystem::path(TERRAENGINE_COMPILER_PROCESS_FIXTURE_PATH).filename();
		const auto executable = TerraEngine::PlatformUtils::GetExecutablePath().parent_path() / name;
		context.Expect(!name.empty() && std::filesystem::is_regular_file(executable),
			"The native compiler fixture must exist beside the running review test executable.");
		return executable;
	}
	std::wstring Quote(const std::filesystem::path& path) { return L"\"" + path.wstring() + L"\""; }
	std::wstring Command(const std::filesystem::path& exe, HANDLE sentinel, const std::filesystem::path& receipt,
		const std::wstring& ready, const std::wstring& release, const std::wstring& marker) {
		// Names contain no quotes or trailing directory separator. The tested
		// defect is native handle ownership, not this fixture's command encoding.
		return Quote(exe) + L" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(sentinel))
			+ L" " + Quote(receipt) + L" \"" + ready + L"\" \"" + release + L"\" " + marker;
	}
	nlohmann::json ReadReceipt(const std::filesystem::path& path) {
		std::ifstream in(path); return nlohmann::json::parse(in);
	}
	struct ReleaseEvents {
		HANDLE a, b;
		~ReleaseEvents() { SetEvent(a); SetEvent(b); }
	};
}
#endif

void RegisterCompilerPipeOwnershipTests(TestRegistry& registry) {
	registry.Add("Editor.AssetPipeline.CompilerOwnership.UnrelatedInheritableEventIsExcluded", [](TestContext& context) {
#if defined(_WIN32)
		FixtureRoot fixture;
		SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
		Handle sentinel(CreateEventW(&inheritable, TRUE, FALSE, nullptr));
		const auto exe = FixtureExecutable(context);
		const auto receipt = fixture.path / "sentinel.json";
		context.Expect(AssetDatabaseCompilerProcessTestAccess::Run(exe, Command(exe, sentinel.value, receipt, L"-", L"-", L"A"), receipt),
			"The real compiler launcher must complete the controlled child.");
		const auto observed = ReadReceipt(receipt);
		context.Expect(WaitForSingleObject(sentinel.value, 0) == WAIT_TIMEOUT,
			"A compiler child must not inherit and signal an unrelated parent event.");
		context.Expect(!observed.at("inheritedSentinel").get<bool>(), "The native child must report its lack of the parent-only handle.");
#else
		context.Skip("Real Windows handle inheritance requires the Windows compiler fixture.");
#endif
	});
	registry.Add("Editor.AssetPipeline.CompilerOwnership.OverlappingChildrenKeepSeparateReceipts", [](TestContext& context) {
#if defined(_WIN32)
		FixtureRoot fixture;
		const auto stem = L"Local\\TerraCompilerOwner-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count());
		const auto ar = stem + L"-A-ready", br = stem + L"-B-ready", ax = stem + L"-A-release", bx = stem + L"-B-release";
		Handle readyA(CreateEventW(nullptr, TRUE, FALSE, ar.c_str())), readyB(CreateEventW(nullptr, TRUE, FALSE, br.c_str()));
		Handle releaseA(CreateEventW(nullptr, TRUE, FALSE, ax.c_str())), releaseB(CreateEventW(nullptr, TRUE, FALSE, bx.c_str()));
		const auto exe = FixtureExecutable(context);
		const auto receiptA = fixture.path / "A.json", receiptB = fixture.path / "B.json";
		const auto ownerThread = std::this_thread::get_id();
		std::thread::id producerA, producerB, settledA, settledB;
		auto pool = ThreadPool::CreateOwned(2);
		// Record both ends of the actual blocking compiler launch/capture operation.
		// The ready/release events below prove these two operations really overlap.
		auto a = pool->createTask([&] {
			producerA = std::this_thread::get_id();
			const bool result = AssetDatabaseCompilerProcessTestAccess::Run(exe, Command(exe, nullptr, receiptA, ar, ax, L"A"), receiptA);
			settledA = std::this_thread::get_id(); return result;
		});
		auto b = pool->createTask([&] {
			producerB = std::this_thread::get_id();
			const bool result = AssetDatabaseCompilerProcessTestAccess::Run(exe, Command(exe, nullptr, receiptB, br, bx, L"B"), receiptB);
			settledB = std::this_thread::get_id(); return result;
		});
		auto af = a->getFuture(), bf = b->getFuture();
		// Guard destructs before pool: any owner exception releases both child waits.
		ReleaseEvents release{releaseA.value, releaseB.value};
		pool->enqueue(a); pool->enqueue(b);
		const bool bothReady = WaitForSingleObject(readyA.value, 20000) == WAIT_OBJECT_0
			&& WaitForSingleObject(readyB.value, 20000) == WAIT_OBJECT_0;
		SetEvent(releaseA.value);
		const bool aCompletedWhileBHeld = af.wait_for(std::chrono::seconds(20)) == std::future_status::ready;
		SetEvent(releaseB.value);
		const bool aOk = af.get(), bOk = bf.get();
		std::cout << "Compiler capture identities: owner=" << ownerThread << " A=" << producerA << ':' << settledA
			<< " B=" << producerB << ':' << settledB << " A-settled-while-B-held=" << aCompletedWhileBHeld << '\n';
		context.Expect(producerA != std::thread::id{} && producerB != std::thread::id{}
			&& producerA != ownerThread && producerB != ownerThread && producerA != producerB
			&& settledA == producerA && settledB == producerB && std::this_thread::get_id() == ownerThread,
			"Overlapping real compiler operations must retain distinct observed workers through native child settlement.");
		context.Expect(bothReady && aCompletedWhileBHeld && aOk && bOk,
			"A must settle independently while the other actual child remains held.");
		context.Expect(ReadReceipt(receiptA).at("marker") == "A" && ReadReceipt(receiptB).at("marker") == "B",
			"Each real child must publish only its own receipt.");
#else
		context.Skip("Real concurrent compiler pipe ownership requires Windows.");
#endif
	});
}
}
