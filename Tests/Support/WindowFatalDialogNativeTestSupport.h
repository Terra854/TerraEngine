#pragma once
// Test-only synchronous dispatch seam. The production fatal entry uses
// the same caption conversion/dispatch helper with the real MessageBoxW sink.
// This wrapper is defined only in a window-test compilation of WindowSystem.cpp;
// it does not expose a runtime/global hook or change WindowSystem class layout.
#include "Core/FatalErrorReport.h"

#ifdef _WIN32
namespace TerraEngine::WindowTestSupport {
	using NativeFatalDialogSink = int (*)(void* context, const wchar_t* message,
		const wchar_t* title, unsigned int flags);
	int DispatchFatalDialogForTest(const FatalErrorDialogReport& report,
		NativeFatalDialogSink sink, void* context);
}
#endif
