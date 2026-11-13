// WindowSystem.cpp — Contains the Window System system state, startup/shutdown hooks, and per-frame work implementation for window configuration, backend contracts, and high-level window system state.

#include "WindowSystem.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <limits>
#include <stdexcept>

#include "Debug.h"
#include "Core/Logger/Logger.h"
#include "Profiling/ProfilerScope.h"

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <Windows.h>
#endif
#if defined(_WIN32) && defined(TERRAENGINE_WINDOW_NATIVE_TITLE_TESTS)
#	include "Tests/Support/WindowFatalDialogNativeTestSupport.h"
#endif

namespace
{
	constexpr const char* kWindowPollingClassification = "protected";
	constexpr const char* kWindowPollingProtectedReason = "Platform event polling, close/resize callbacks, cursor state, and swap-buffer calls remain owned by the coordinator thread.";
	constexpr const char* kWindowPollingWorkerSafeSubwork = "Workers may consume immutable input/window snapshots after polling.";

	// Native filesystem paths must join the report's explicitly UTF8 text.
	std::string Utf8Path(const std::filesystem::path& path)
	{
		const auto bytes = path.u8string();
		return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
	}

	std::string FormatFatalDialogMessage(const TerraEngine::FatalErrorDialogReport& report)
	{
		std::ostringstream stream;
		stream << report.shortReason;
		if (!report.details.empty())
		{
			stream << "\n\n" << report.details;
		}
		if (!report.crashDumpPath.empty())
		{
			stream << "\n\nCrash dump: " << Utf8Path(report.crashDumpPath);
		}
		if (!report.crashSummaryPath.empty())
		{
			stream << "\nReport: " << Utf8Path(report.crashSummaryPath);
		}
		if (!report.logPath.empty())
		{
			stream << "\nLog: " << Utf8Path(report.logPath);
		}
		if (!report.crashDumpFailureReason.empty())
		{
			stream << "\nDump status: " << TerraEngine::ToString(report.crashDumpStatus)
				<< " (" << report.crashDumpFailureReason << ")";
		}
		return stream.str();
	}
#ifdef _WIN32
	using NativeFatalDialogSink = int (*)(void*, const wchar_t*, const wchar_t*, unsigned int);
	struct NativeFatalText
	{
		std::wstring converted;
		bool fallback = false;
		const wchar_t* Data() const noexcept
		{
			// A bounded literal needs no allocation while reporting conversion
			// failure, including failure to allocate the UTF16 output buffer.
			return fallback ? L"Fatal error text unavailable (UTF-8 conversion failed)." : converted.c_str();
		}
	};
	NativeFatalText ConvertFatalUtf8(const std::string& input) noexcept
	{
		NativeFatalText result;
		if(input.empty()) return result;
		if(input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
		{
			result.fallback = true;
			return result;
		}
		const int byteCount = static_cast<int>(input.size());
		// Flags0 applies Windows' U+FFFD replacement policy for malformed UTF8.
		// ANSI conversion or byte widening would corrupt valid multilingual text.
		const int count = MultiByteToWideChar(CP_UTF8, 0, input.data(), byteCount, nullptr, 0);
		if(count <= 0) { result.fallback = true; return result; }
		try
		{
			result.converted.resize(static_cast<std::size_t>(count));
			if(MultiByteToWideChar(CP_UTF8, 0, input.data(), byteCount, result.converted.data(), count) != count)
				result.fallback = true;
		}
		catch(...) { result.fallback = true; }
		return result;
	}
	// Production and owned-window tests share conversion and dispatch arguments.
	int DispatchNativeFatalDialog(const std::string& message, const std::string& title,
		NativeFatalDialogSink sink, void* context)
	{
		const NativeFatalText wideTitle = ConvertFatalUtf8(title);
		const NativeFatalText wideMessage = ConvertFatalUtf8(message);
		return sink(context, wideMessage.Data(), wideTitle.Data(), MB_OK | MB_ICONERROR | MB_TASKMODAL);
	}
	int PresentNativeFatalMessageBox(void*, const wchar_t* message, const wchar_t* title, unsigned int flags)
	{
		return MessageBoxW(nullptr, message, title, flags);
	}
#endif
}

#if defined(_WIN32) && defined(TERRAENGINE_WINDOW_NATIVE_TITLE_TESTS)
namespace TerraEngine::WindowTestSupport {
	int DispatchFatalDialogForTest(const FatalErrorDialogReport& report,
		NativeFatalDialogSink sink, void* context)
	{
		return DispatchNativeFatalDialog(FormatFatalDialogMessage(report), report.title, sink, context);
	}
}
#endif

WindowSystem& WindowSystem::Instance()
{
	// Provide a single shared instance so the engine stack can reference the
	// window system without heap allocations or duplicate ownership.
	static WindowSystem instance;
	return instance;
}


void WindowSystem::Configure(std::unique_ptr<IWindowBackend> backendInstance, WindowConfig config, std::function<void()> shutdownCallback)
{
	// Store the dependencies up front so Initialize can safely boot the backend
	// and the engine can reuse the singleton across runs without stale state.
	backend = std::move(backendInstance);
	windowConfig = std::move(config);
	onShutdownRequested = std::move(shutdownCallback);
	configured = true;
	initialized = false;
	pollingDiagnostics = {};
	pollingDiagnostics.classification = kWindowPollingClassification;
	pollingDiagnostics.protectedReason = kWindowPollingProtectedReason;
	pollingDiagnostics.workerSafeSubwork = kWindowPollingWorkerSafeSubwork;
	latestWorkerSnapshot = {};
	latestWorkerSnapshot.width = windowConfig.width;
	latestWorkerSnapshot.height = windowConfig.height;
	latestWorkerSnapshot.resizable = windowConfig.resizable;
	TERRAENGINE_LOG(LogLevel::Info, "Window presentation configuration: resizable="
		<< (windowConfig.resizable ? "true" : "false"));
	latestWorkerSnapshot.title = windowConfig.title;
	latestWorkerSnapshot.boundarySummary = pollingDiagnostics.workerSafeSubwork;
	// Debug log makes backend selection visible before initialization runs.
	TERRAENGINE_LOG(LogLevel::Debug, "WindowSystem configured with backend " << (backend ? backend->BackendName() : "null"));
}


void* WindowSystem::GetNativeHandle() const
{
	if (!backend) {
		return nullptr;
	}
	
	return backend->GetNativeHandle();
}


void WindowSystem::SetResizeCallback(std::function<void(int, int)> callback)
{
	if (!backend) return;
	backend->SetResizeCallback(std::move(callback));
}


void WindowSystem::Initialize()
{
	if (!configured) {
		throw std::runtime_error("WindowSystem must be configured before initialization.");
	}
	

	if (!backend) {
		throw std::runtime_error("WindowSystem requires a valid backend.");
	}
	
	// Route the backend close request back to the engine so the main loop can
	// exit cleanly.
	backend->SetCloseCallback([this]() {
		if (onShutdownRequested) {
			onShutdownRequested();
		}
	});
	
	backend->Initialize(windowConfig);
	initialized = true;

	// Let consumers know that window creation succeeded and which backend drove it.
	TERRAENGINE_LOG(LogLevel::Info, "WindowSystem initialized using backend: " << backend->BackendName());
}


void WindowSystem::Update(float deltaTime)
{
	if (!initialized) {
		return;
	}

	{
		TERRAENGINE_PROFILE_SCOPE("WindowSystem: Poll Events", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		backend->PollEvents();
	}
	bool closeRequested = false;
	{
		TERRAENGINE_PROFILE_SCOPE("WindowSystem: Publish Worker Snapshot", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Platform polling and buffer swaps remain protected because they execute
		// backend callbacks and OS-facing side effects. The descriptive strings
		// are initialized during Configure so steady frames do not allocate.
		pollingDiagnostics.pollRevision += 1;
		pollingDiagnostics.lastPollRan = true;
		closeRequested = backend->ShouldClose();
		latestWorkerSnapshot.frameRevision = pollingDiagnostics.pollRevision;
		latestWorkerSnapshot.width = windowConfig.width;
		latestWorkerSnapshot.height = windowConfig.height;
		latestWorkerSnapshot.resizable = windowConfig.resizable;
		latestWorkerSnapshot.shouldClose = closeRequested;
		latestWorkerSnapshot.workerReadable = true;
	}
	

	if (closeRequested) {
		// Respect the request from the OS/user to close the application.
		TERRAENGINE_LOG(LogLevel::Debug, "Window backend " << backend->BackendName() << " requested application shutdown");

		if (onShutdownRequested) {
			onShutdownRequested();
		}
	}
	
	{
		TERRAENGINE_PROFILE_SCOPE("WindowSystem: Swap Buffers", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		backend->SwapBuffers();
	}
	pollingDiagnostics.lastSwapRan = true;
}


void WindowSystem::Shutdown()
{
	if (!backend) {
		return;
	}
	
	backend->Shutdown();
	initialized = false;
	latestWorkerSnapshot.workerReadable = false;

	TERRAENGINE_LOG(LogLevel::Info, "WindowSystem shutdown complete (backend " << backend->BackendName() << ").");
}


std::string WindowSystem::SystemName()
{
	return "WindowSystem";
}

WindowSystem::WindowPollingBoundaryDiagnostics WindowSystem::GetPollingBoundaryDiagnostics() const
{
	return pollingDiagnostics;
}

WindowSystem::WindowWorkerSnapshot WindowSystem::GetLatestWorkerSnapshot() const
{
	return latestWorkerSnapshot;
}

[[noreturn]] void WindowSystem::ShowFatalErrorDialog(const TerraEngine::FatalErrorDialogReport& report) const
{
	const std::string message = FormatFatalDialogMessage(report);
	TERRAENGINE_LOG(LogLevel::Critical, "Fatal error dialog: " << report.title << " - " << message);
#ifdef _WIN32
	// MessageBoxW is intentionally independent of the engine renderer. The
	// owner handle may be null when the graphics backend failed during startup,
	// which still gives Windows a process-modal crash reason dialog.
	(void)DispatchNativeFatalDialog(message, report.title, PresentNativeFatalMessageBox, nullptr);
#else
	(void)report;
#endif
	// Fatal graphics state cannot be cleaned up through the ordinary shutdown
	// path because that path may wait on the lost Vulkan device. Flush standard
	// streams, then terminate immediately with a portable non-zero exit code.
	std::fflush(nullptr);
	std::_Exit(EXIT_FAILURE);
}
