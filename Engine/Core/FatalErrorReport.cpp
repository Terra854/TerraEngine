// FatalErrorReport.cpp - small fatal-report sidecar helpers.

#include "Core/FatalErrorReport.h"

#include <fstream>
#include <utility>

namespace TerraEngine
{
	const char* ToString(FatalErrorCategory category)
	{
		switch (category)
		{
		case FatalErrorCategory::RendererRecoveryFailed: return "RendererRecoveryFailed";
		case FatalErrorCategory::GraphicsFatal: return "GraphicsFatal";
		case FatalErrorCategory::StartupFatal: return "StartupFatal";
		case FatalErrorCategory::ExportFatal: return "ExportFatal";
		case FatalErrorCategory::ControlledEngineFatal: return "ControlledEngineFatal";
		}
		return "ControlledEngineFatal";
	}

	const char* ToString(CrashDumpStatus status)
	{
		switch (status)
		{
		case CrashDumpStatus::Written: return "Written";
		case CrashDumpStatus::Unsupported: return "Unsupported";
		case CrashDumpStatus::Failed: return "Failed";
		}
		return "Unsupported";
	}

	FatalErrorDialogReport BuildFatalErrorDialogReport(FatalErrorCategory category, std::string shortReason, std::string details)
	{
		FatalErrorDialogReport report;
		report.category = category;
		report.reportId = std::string("fatal-") + ToString(category);
		report.title = "TerraEngine encountered a fatal error";
		report.shortReason = std::move(shortReason);
		report.details = std::move(details);
		return report;
	}

	bool SaveFatalErrorSummarySidecar(const FatalErrorDialogReport& report, const std::filesystem::path& path, std::string* errorMessage)
	{
		std::error_code createError;
		std::filesystem::create_directories(path.parent_path(), createError);
		if (createError)
		{
			if (errorMessage != nullptr)
			{
				*errorMessage = createError.message();
			}
			return false;
		}

		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if (!output)
		{
			if (errorMessage != nullptr)
			{
				*errorMessage = "Could not open fatal error sidecar for writing.";
			}
			return false;
		}

		// Keep the sidecar line-based so it remains human-readable beside the
		// native dump and can be inspected without editor-specific tooling.
		output << "category=" << ToString(report.category) << '\n';
		output << "reason=" << report.shortReason << '\n';
		output << "details=" << report.details << '\n';
		output << "dump=" << report.crashDumpPath.generic_string() << '\n';
		output << "dumpStatus=" << ToString(report.crashDumpStatus) << '\n';
		output << "dumpFailure=" << report.crashDumpFailureReason << '\n';
		output << "log=" << report.logPath.generic_string() << '\n';
		return true;
	}

}
