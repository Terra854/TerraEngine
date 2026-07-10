// FatalErrorReport.h - renderer-independent fatal error report data.

#pragma once

#include "Core/Export.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace TerraEngine
{
	enum class FatalErrorCategory
	{
		RendererRecoveryFailed,
		GraphicsFatal,
		StartupFatal,
		ExportFatal,
		ControlledEngineFatal
	};

	enum class CrashDumpStatus
	{
		Written,
		Unsupported,
		Failed
	};

	struct FatalErrorDialogReport
	{
		std::string reportId;
		FatalErrorCategory category = FatalErrorCategory::ControlledEngineFatal;
		std::string title;
		std::string shortReason;
		std::string details;
		std::filesystem::path logPath;
		std::filesystem::path crashSummaryPath;
		std::filesystem::path crashDumpDirectory;
		std::filesystem::path crashDumpPath;
		CrashDumpStatus crashDumpStatus = CrashDumpStatus::Unsupported;
		std::string crashDumpFailureReason;
		std::string incidentId;
		std::string recoverySessionId;
	};

	// Editor failures always use a stable editor-owned diagnostics root. Project
	// display names are reserved for exported applications and must never decide
	// where the editor itself writes crash artifacts.
	inline constexpr std::string_view kEditorFatalErrorProductName = "TerraEngine Editor";

	TERRAENGINE_API const char* ToString(FatalErrorCategory category);
	TERRAENGINE_API const char* ToString(CrashDumpStatus status);
	TERRAENGINE_API FatalErrorDialogReport BuildFatalErrorDialogReport(FatalErrorCategory category, std::string shortReason, std::string details = {});
	TERRAENGINE_API bool SaveFatalErrorSummarySidecar(const FatalErrorDialogReport& report, const std::filesystem::path& path, std::string* errorMessage = nullptr);
}
