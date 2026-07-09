// FatalErrorReport.cpp - small fatal-report sidecar helpers.

#include "Core/FatalErrorReport.h"

#include <fstream>
#include <sstream>
#include <utility>

namespace TerraEngine
{
	namespace
	{
		CrashDumpStatus CrashDumpStatusFromString(std::string_view value)
		{
			if (value == "Written")
			{
				return CrashDumpStatus::Written;
			}
			if (value == "Failed")
			{
				return CrashDumpStatus::Failed;
			}
			return CrashDumpStatus::Unsupported;
		}
	}

	const char* ToString(FatalErrorCategory category)
	{
		switch (category)
		{
		case FatalErrorCategory::RendererRecoveryFailed: return "RendererRecoveryFailed";
		case FatalErrorCategory::GraphicsFatal: return "GraphicsFatal";
		case FatalErrorCategory::StartupFatal: return "StartupFatal";
		case FatalErrorCategory::ExportFatal: return "ExportFatal";
		case FatalErrorCategory::ControlledEngineFatal: return "ControlledEngineFatal";
		case FatalErrorCategory::PreviousHardCrash: return "PreviousHardCrash";
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
		report.canContinue = false;
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

		// The sidecar is intentionally line-based so the previous-crash dialog
		// path can parse it with minimal dependencies during early startup.
		output << "category=" << ToString(report.category) << '\n';
		output << "reason=" << report.shortReason << '\n';
		output << "details=" << report.details << '\n';
		output << "dump=" << report.crashDumpPath.generic_string() << '\n';
		output << "dumpStatus=" << ToString(report.crashDumpStatus) << '\n';
		output << "dumpFailure=" << report.crashDumpFailureReason << '\n';
		output << "log=" << report.logPath.generic_string() << '\n';
		return true;
	}

	bool LoadFatalErrorSummarySidecar(const std::filesystem::path& path, FatalErrorDialogReport& report, std::string* errorMessage)
	{
		std::ifstream input(path, std::ios::binary);
		if (!input)
		{
			if (errorMessage != nullptr)
			{
				*errorMessage = "Could not open fatal error sidecar for reading.";
			}
			return false;
		}

		report = BuildFatalErrorDialogReport(FatalErrorCategory::PreviousHardCrash, "Previous TerraEngine crash detected.");
		report.crashSummaryPath = path;
		report.shownOnNextLaunch = true;

		std::string line;
		while (std::getline(input, line))
		{
			const std::size_t separator = line.find('=');
			if (separator == std::string::npos)
			{
				continue;
			}
			const std::string key = line.substr(0u, separator);
			const std::string value = line.substr(separator + 1u);
			if (key == "reason")
			{
				report.shortReason = value;
			}
			else if (key == "details")
			{
				report.details = value;
			}
			else if (key == "dump")
			{
				report.crashDumpPath = value;
			}
			else if (key == "dumpStatus")
			{
				report.crashDumpStatus = CrashDumpStatusFromString(value);
			}
			else if (key == "dumpFailure")
			{
				report.crashDumpFailureReason = value;
			}
			else if (key == "log")
			{
				report.logPath = value;
			}
		}
		return true;
	}
}
