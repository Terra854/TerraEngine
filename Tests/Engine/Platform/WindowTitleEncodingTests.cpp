// Actual production fatal-caption dispatch observed through an owned hidden HWND.
// Prerequisites: Windows user32 desktop; compile the real WindowSystem.cpp with
// TERRAENGINE_WINDOW_NATIVE_TITLE_TESTS and the proposed dispatch seam. No GPU,
// GLFW singleton, engine initialization, modal dialog, or process exit is used.
// Parent invocation: TERRAENGINE_TEST_FILTER=Engine.WindowTitleEncoding.
#include "TestHarness.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "Tests/Support/WindowFatalDialogNativeTestSupport.h"
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		// char8_t literals describe bytes explicitly and work even when the source
		// checkout/compiler locale differs. This copies bytes; it never decodes.
		std::string Utf8Bytes(std::u8string_view value) {
			return {reinterpret_cast<const char*>(value.data()), value.size()};
		}
		class OwnedHiddenCaptionWindow final {
		public:
			OwnedHiddenCaptionWindow() {
				// The OS-owned STATIC class avoids process-wide registration. Omitting
				// WS_VISIBLE keeps this private top-level window hidden throughout.
				handle = CreateWindowExW(0u, L"STATIC", L"owned-caption-probe", WS_OVERLAPPED,
					0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
				if (!handle) throw TestFailure("Hidden native caption window creation failed: " + std::to_string(GetLastError()));
			}
			~OwnedHiddenCaptionWindow() { if (handle) DestroyWindow(handle); }
			OwnedHiddenCaptionWindow(const OwnedHiddenCaptionWindow&) = delete;
			OwnedHiddenCaptionWindow& operator=(const OwnedHiddenCaptionWindow&) = delete;
			HWND Get() const noexcept { return handle; }
		private:
			HWND handle = nullptr;
		};
		struct NativeTitleObservation {
			HWND window = nullptr;
			HWND bodyWindow = nullptr;
			bool invoked = false;
			bool setSucceeded = false;
			bool bodySetSucceeded = false;
			unsigned int flags = 0u;
		};
		int ApplyActualDispatchedCaption(void* context, const wchar_t* message, const wchar_t* title, unsigned int flags) {
			auto& observation = *static_cast<NativeTitleObservation*>(context);
			observation.invoked = true;
			observation.flags = flags;
			// This is a real native title operation with the exact wchar_t caption
			// produced by WindowSystem. No test converter supplies or repairs it.
			observation.setSucceeded = SetWindowTextW(observation.window, title) != FALSE;
			observation.bodySetSucceeded = SetWindowTextW(observation.bodyWindow, message) != FALSE;
			return IDOK;
		}
		std::wstring ReadActualNativeTitle(TestContext& context, HWND window) {
			SetLastError(ERROR_SUCCESS);
			const int length = GetWindowTextLengthW(window);
			context.Expect(length >= 0 && length <= 2048 && (length != 0 || GetLastError() == ERROR_SUCCESS),
				"Native title length must be readable and fit this bounded fixture.");
			std::vector<wchar_t> storage(static_cast<std::size_t>(length) + 1u, L'\0');
			SetLastError(ERROR_SUCCESS);
			const int copied = GetWindowTextW(window, storage.data(), static_cast<int>(storage.size()));
			context.Expect(copied == length && (copied != 0 || GetLastError() == ERROR_SUCCESS),
				"GetWindowTextW must read the whole actual native UTF16 caption.");
			return {storage.data(), static_cast<std::size_t>(copied)};
		}
		void ExpectProductionNativeCaption(TestContext& context, std::u8string_view input, std::wstring_view expected,
			std::u8string_view reason = u8"bounded native caption test", std::wstring_view expectedReason = L"bounded native caption test",
			std::u8string_view details = u8"", std::wstring_view expectedDetails = L"") {
			OwnedHiddenCaptionWindow window;
			OwnedHiddenCaptionWindow bodyWindow;
			context.Expect(IsWindowVisible(window.Get()) == FALSE, "The owned native window must begin hidden.");
			NativeTitleObservation observation{window.Get(), bodyWindow.Get()};
			FatalErrorDialogReport report;
			report.title = Utf8Bytes(input);
			report.shortReason = Utf8Bytes(reason);
			report.details = Utf8Bytes(details);
			const int result = WindowTestSupport::DispatchFatalDialogForTest(report, ApplyActualDispatchedCaption, &observation);
			context.Expect(result == IDOK && observation.invoked && observation.setSucceeded && observation.bodySetSucceeded,
				"Actual production caption dispatch must complete the real native title call.");
			context.Expect(observation.flags == (MB_OK | MB_ICONERROR | MB_TASKMODAL),
				"The seam must retain the production native fatal-dialog flags.");
			context.Expect(IsWindowVisible(window.Get()) == FALSE && IsWindowVisible(bodyWindow.Get()) == FALSE, "Caption application must never expose the hidden fixture window.");
			context.Expect(ReadActualNativeTitle(context, window.Get()) == expected,
				"Actual native UTF16 title must equal the independently authored intended text.");
			std::wstring expectedBody(expectedReason);
			if (!expectedDetails.empty()) { expectedBody += L"\n\n"; expectedBody += expectedDetails; }
			context.Expect(ReadActualNativeTitle(context, bodyWindow.Get()) == expectedBody,
				"Actual production native-wide body arguments must preserve intended UTF16 text too.");
		}
	}
	void RegisterWindowTitleEncodingTests(TestRegistry& registry) {
		registry.Add("Engine.WindowTitleEncoding.AsciiCaptionControl", [](TestContext& context) {
			ExpectProductionNativeCaption(context, u8"TerraEngine fatal report 123", L"TerraEngine fatal report 123");
		});
		registry.Add("Engine.WindowTitleEncoding.EmptyCaptionControl", [](TestContext& context) {
			ExpectProductionNativeCaption(context, u8"", L"", u8"", L"");
		});
		registry.Add("Engine.WindowTitleEncoding.BmpUtf8Caption", [](TestContext& context) {
			ExpectProductionNativeCaption(context, u8"Caf\u00E9 \u4E16\u754C \u03A9", L"Caf\u00E9 \u4E16\u754C \u03A9");
		});
		registry.Add("Engine.WindowTitleEncoding.SupplementaryUtf8Caption", [](TestContext& context) {
			ExpectProductionNativeCaption(context, u8"Terra \U0001F680 \U0001F30D", L"Terra \U0001F680 \U0001F30D");
		});
		registry.Add("Engine.WindowTitleEncoding.UnicodePathAndCombiningCaption", [](TestContext& context) {
			// This path is caption text, not a fabricated on-disk artifact. No ANSI
			// filesystem conversion is permitted to obscure the UTF8 title oracle.
			ExpectProductionNativeCaption(context, u8"C:\\Users\\\u674E\\\u9879\u76EE\\Cafe\u0301.scene",
				L"C:\\Users\\\u674E\\\u9879\u76EE\\Cafe\u0301.scene");
		});
		registry.Add("Engine.WindowTitleEncoding.BmpUtf8BodyAndPath", [](TestContext& context) {
			ExpectProductionNativeCaption(context, u8"ASCII title", L"ASCII title",
				u8"Caf\u00E9 \u4E16\u754C", L"Caf\u00E9 \u4E16\u754C",
				u8"C:\\Users\\\u674E\\\u9879\u76EE\\error.log", L"C:\\Users\\\u674E\\\u9879\u76EE\\error.log");
		});
		registry.Add("Engine.WindowTitleEncoding.SupplementaryUtf8Body", [](TestContext& context) {
			ExpectProductionNativeCaption(context, u8"ASCII title", L"ASCII title",
				u8"Failure \U0001F680", L"Failure \U0001F680");
		});
		registry.Add("Engine.WindowTitleEncoding.InvalidUtf8PolicyQualification", [](TestContext& context) {
			OwnedHiddenCaptionWindow window;
			OwnedHiddenCaptionWindow bodyWindow;
			NativeTitleObservation observation{window.Get(), bodyWindow.Get()};
			FatalErrorDialogReport report;
			// Windows CP_UTF8/flags0 replaces malformed subsequences with U+FFFD.
			// A readable fatal report must still dispatch rather than reject text.
			report.title = std::string("\xC3\x28", 2u);
			report.shortReason = std::string("\xF0\x28\x8C\x28", 4u);
			const int result = WindowTestSupport::DispatchFatalDialogForTest(report, ApplyActualDispatchedCaption, &observation);
			context.Expect(result == IDOK && observation.invoked && observation.setSucceeded && observation.bodySetSucceeded,
				"Malformed UTF8 must still dispatch readable native-wide replacement text.");
			context.Expect(ReadActualNativeTitle(context, window.Get()) == L"\uFFFD("
				&& ReadActualNativeTitle(context, bodyWindow.Get()) == L"\uFFFD(\uFFFD(",
				"Actual native title/body must follow Windows UTF8 replacement, never ANSI byte widening.");
			context.Expect(IsWindowVisible(window.Get()) == FALSE && IsWindowVisible(bodyWindow.Get()) == FALSE,
				"Malformed input cannot make either owned native window visible.");
		});
	}
}
#else
namespace TerraEngine::Tests {
	// CR139's native-wide call is Windows-only. Register no pretend Linux pass;
	// a dedicated filter selects0 and the existing harness rejects an empty run.
	void RegisterWindowTitleEncodingTests(TestRegistry&) {}
}
#endif
