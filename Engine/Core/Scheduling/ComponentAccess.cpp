// ComponentAccess.cpp - deterministic normalization and validation helpers.

#include "ComponentAccess.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_set>

namespace TerraEngine::Scheduling {
	namespace {
		std::string ResourceKey(std::string prefix, const std::string& name)
		{
			return prefix + ":" + name;
		}

		void AddDiagnostic(std::vector<ComponentAccessDiagnostic>& diagnostics, ComponentAccessDiagnosticSeverity severity, ComponentAccessDiagnosticCode code, const ComponentAccessDeclaration& declaration, std::string resourceName, std::string message, std::string action)
		{
			diagnostics.push_back({
				severity,
				code,
				declaration.ownerSystem,
				declaration.queryName,
				std::move(resourceName),
				std::move(message),
				std::move(action)
			});
		}

		int SeverityRank(ComponentAccessDiagnosticSeverity severity)
		{
			switch (severity)
			{
			case ComponentAccessDiagnosticSeverity::Error:
				return 0;
			case ComponentAccessDiagnosticSeverity::Warning:
				return 1;
			case ComponentAccessDiagnosticSeverity::Info:
			default:
				return 2;
			}
		}

		void SortDiagnostics(std::vector<ComponentAccessDiagnostic>& diagnostics)
		{
			std::stable_sort(diagnostics.begin(), diagnostics.end(), [](const ComponentAccessDiagnostic& left, const ComponentAccessDiagnostic& right) {
				return std::make_tuple(SeverityRank(left.severity), static_cast<int>(left.code), left.ownerSystem, left.queryName, left.resourceName, left.message)
					< std::make_tuple(SeverityRank(right.severity), static_cast<int>(right.code), right.ownerSystem, right.queryName, right.resourceName, right.message);
			});
		}

		void NormalizeList(std::vector<std::string>& values, const ComponentAccessDeclaration& declaration, ComponentAccessDiagnosticCode duplicateCode, std::vector<ComponentAccessDiagnostic>* diagnostics)
		{
			values.erase(std::remove_if(values.begin(), values.end(), [](const std::string& value) {
				return value.empty();
			}), values.end());
			std::stable_sort(values.begin(), values.end());
			std::vector<std::string> unique;
			for (const std::string& value : values)
			{
				if (!unique.empty() && unique.back() == value)
				{
					if (diagnostics != nullptr)
					{
						AddDiagnostic(*diagnostics, ComponentAccessDiagnosticSeverity::Info, duplicateCode, declaration, value, "Duplicate access entry was collapsed.", "Keep one access declaration per component or resource.");
					}
					continue;
				}
				unique.push_back(value);
			}
			values = std::move(unique);
		}

		void RemoveWritesFromReads(std::vector<std::string>& reads, const std::vector<std::string>& writes, const ComponentAccessDeclaration& declaration, const std::string& prefix, std::vector<ComponentAccessDiagnostic>* diagnostics)
		{
			std::vector<std::string> filtered;
			for (const std::string& read : reads)
			{
				if (std::find(writes.begin(), writes.end(), read) != writes.end())
				{
					if (diagnostics != nullptr)
					{
						AddDiagnostic(*diagnostics, ComponentAccessDiagnosticSeverity::Info, ComponentAccessDiagnosticCode::ReadPromotedToWrite, declaration, ResourceKey(prefix, read), "Read access was promoted to write access.", "A write declaration already includes read access for safety validation.");
					}
					continue;
				}
				filtered.push_back(read);
			}
			reads = std::move(filtered);
		}

		std::set<std::string> ReadSet(const ComponentAccessDeclaration& declaration)
		{
			std::set<std::string> result;
			for (const std::string& value : declaration.readComponents)
			{
				result.insert(ResourceKey("component", value));
			}
			for (const std::string& value : declaration.readResources)
			{
				result.insert(ResourceKey("resource", value));
			}
			return result;
		}

		std::set<std::string> WriteSet(const ComponentAccessDeclaration& declaration)
		{
			std::set<std::string> result;
			for (const std::string& value : declaration.writeComponents)
			{
				result.insert(ResourceKey("component", value));
			}
			for (const std::string& value : declaration.writeResources)
			{
				result.insert(ResourceKey("resource", value));
			}
			return result;
		}
	}

	std::string ToString(ComponentAccessMode mode)
	{
		return mode == ComponentAccessMode::Write ? "write" : "read";
	}

	std::string ToString(ComponentAccessConflictKind kind)
	{
		switch (kind)
		{
		case ComponentAccessConflictKind::ReadRead:
			return "read-read";
		case ComponentAccessConflictKind::ReadWrite:
			return "read-write";
		case ComponentAccessConflictKind::WriteWrite:
			return "write-write";
		case ComponentAccessConflictKind::MissingDeclaration:
			return "missing-declaration";
		case ComponentAccessConflictKind::None:
		default:
			return "none";
		}
	}

	std::string ToString(ComponentAccessDiagnosticSeverity severity)
	{
		switch (severity)
		{
		case ComponentAccessDiagnosticSeverity::Error:
			return "error";
		case ComponentAccessDiagnosticSeverity::Warning:
			return "warning";
		case ComponentAccessDiagnosticSeverity::Info:
		default:
			return "info";
		}
	}

	std::string ToString(ComponentAccessDiagnosticCode code)
	{
		switch (code)
		{
		case ComponentAccessDiagnosticCode::EmptyOwner:
			return "empty-owner";
		case ComponentAccessDiagnosticCode::EmptyQuery:
			return "empty-query";
		case ComponentAccessDiagnosticCode::DuplicateRead:
			return "duplicate-read";
		case ComponentAccessDiagnosticCode::DuplicateWrite:
			return "duplicate-write";
		case ComponentAccessDiagnosticCode::ReadPromotedToWrite:
			return "read-promoted-to-write";
		case ComponentAccessDiagnosticCode::MissingWorkerDeclaration:
			return "missing-worker-declaration";
		case ComponentAccessDiagnosticCode::ProtectedFallback:
			return "protected-fallback";
		case ComponentAccessDiagnosticCode::AccessConflict:
			return "access-conflict";
		case ComponentAccessDiagnosticCode::None:
		default:
			return "none";
		}
	}

	ComponentAccessDeclaration NormalizeComponentAccessDeclaration(ComponentAccessDeclaration declaration, std::vector<ComponentAccessDiagnostic>* diagnostics)
	{
		NormalizeList(declaration.readComponents, declaration, ComponentAccessDiagnosticCode::DuplicateRead, diagnostics);
		NormalizeList(declaration.writeComponents, declaration, ComponentAccessDiagnosticCode::DuplicateWrite, diagnostics);
		NormalizeList(declaration.readResources, declaration, ComponentAccessDiagnosticCode::DuplicateRead, diagnostics);
		NormalizeList(declaration.writeResources, declaration, ComponentAccessDiagnosticCode::DuplicateWrite, diagnostics);
		RemoveWritesFromReads(declaration.readComponents, declaration.writeComponents, declaration, "component", diagnostics);
		RemoveWritesFromReads(declaration.readResources, declaration.writeResources, declaration, "resource", diagnostics);
		return declaration;
	}

	ComponentAccessValidationResult ValidateComponentAccessDeclaration(ComponentAccessDeclaration declaration)
	{
		ComponentAccessValidationResult result;
		std::vector<ComponentAccessDiagnostic> normalizationDiagnostics;
		result.normalized = NormalizeComponentAccessDeclaration(std::move(declaration), &normalizationDiagnostics);
		result.diagnostics = std::move(normalizationDiagnostics);

		if (result.normalized.ownerSystem.empty())
		{
			AddDiagnostic(result.diagnostics, ComponentAccessDiagnosticSeverity::Error, ComponentAccessDiagnosticCode::EmptyOwner, result.normalized, "", "Component access declaration has no owning system.", "Set ownerSystem to the stable SystemsBase name.");
		}
		if (result.normalized.queryName.empty())
		{
			AddDiagnostic(result.diagnostics, ComponentAccessDiagnosticSeverity::Error, ComponentAccessDiagnosticCode::EmptyQuery, result.normalized, "", "Component access declaration has no query name.", "Set queryName so diagnostics remain deterministic.");
		}

		const bool hasAccess = !result.normalized.readComponents.empty()
			|| !result.normalized.writeComponents.empty()
			|| !result.normalized.readResources.empty()
			|| !result.normalized.writeResources.empty();
		if (result.normalized.workerEligible && !hasAccess)
		{
			result.protectedFallback = true;
			AddDiagnostic(result.diagnostics, ComponentAccessDiagnosticSeverity::Warning, ComponentAccessDiagnosticCode::MissingWorkerDeclaration, result.normalized, "", "Worker-eligible query has no component or resource access declaration.", "Declare all reads/writes or keep this work protected.");
		}
		if (!result.normalized.workerEligible && result.normalized.mainThreadReason.empty())
		{
			AddDiagnostic(result.diagnostics, ComponentAccessDiagnosticSeverity::Info, ComponentAccessDiagnosticCode::ProtectedFallback, result.normalized, "", "Work is protected because it is not marked worker eligible.", "No action required unless the workload has a safe worker contract.");
		}

		SortDiagnostics(result.diagnostics);
		result.valid = std::none_of(result.diagnostics.begin(), result.diagnostics.end(), [](const ComponentAccessDiagnostic& diagnostic) {
			return diagnostic.severity == ComponentAccessDiagnosticSeverity::Error;
		});
		result.formatted = FormatComponentAccessDiagnostics(result.diagnostics);
		return result;
	}

	ComponentAccessConflict ClassifyComponentAccessConflict(const ComponentAccessDeclaration& leftInput, const ComponentAccessDeclaration& rightInput)
	{
		ComponentAccessDeclaration left = NormalizeComponentAccessDeclaration(leftInput);
		ComponentAccessDeclaration right = NormalizeComponentAccessDeclaration(rightInput);
		const bool leftMissing = left.workerEligible && ReadSet(left).empty() && WriteSet(left).empty();
		const bool rightMissing = right.workerEligible && ReadSet(right).empty() && WriteSet(right).empty();
		if (leftMissing || rightMissing)
		{
			return { ComponentAccessConflictKind::MissingDeclaration, "", true };
		}

		const std::set<std::string> leftReads = ReadSet(left);
		const std::set<std::string> rightReads = ReadSet(right);
		const std::set<std::string> leftWrites = WriteSet(left);
		const std::set<std::string> rightWrites = WriteSet(right);
		for (const std::string& write : leftWrites)
		{
			if (rightWrites.contains(write))
			{
				return { ComponentAccessConflictKind::WriteWrite, write, true };
			}
			if (rightReads.contains(write))
			{
				return { ComponentAccessConflictKind::ReadWrite, write, true };
			}
		}
		for (const std::string& write : rightWrites)
		{
			if (leftReads.contains(write))
			{
				return { ComponentAccessConflictKind::ReadWrite, write, true };
			}
		}
		for (const std::string& read : leftReads)
		{
			if (rightReads.contains(read))
			{
				return { ComponentAccessConflictKind::ReadRead, read, false };
			}
		}
		return {};
	}

	std::vector<ComponentAccessDiagnostic> ValidateComponentAccessSet(const std::vector<ComponentAccessDeclaration>& declarations)
	{
		std::vector<ComponentAccessDiagnostic> diagnostics;
		std::vector<ComponentAccessDeclaration> normalized;
		for (const ComponentAccessDeclaration& declaration : declarations)
		{
			ComponentAccessValidationResult result = ValidateComponentAccessDeclaration(declaration);
			normalized.push_back(result.normalized);
			diagnostics.insert(diagnostics.end(), result.diagnostics.begin(), result.diagnostics.end());
		}

		for (std::size_t leftIndex = 0; leftIndex < normalized.size(); ++leftIndex)
		{
			for (std::size_t rightIndex = leftIndex + 1; rightIndex < normalized.size(); ++rightIndex)
			{
				const ComponentAccessConflict conflict = ClassifyComponentAccessConflict(normalized[leftIndex], normalized[rightIndex]);
				if (!conflict.requiresOrdering)
				{
					continue;
				}
				ComponentAccessDeclaration owner;
				owner.ownerSystem = normalized[leftIndex].ownerSystem + " <-> " + normalized[rightIndex].ownerSystem;
				owner.queryName = normalized[leftIndex].queryName + " <-> " + normalized[rightIndex].queryName;
				AddDiagnostic(diagnostics, ComponentAccessDiagnosticSeverity::Warning, ComponentAccessDiagnosticCode::AccessConflict, owner, conflict.resourceName, "Component access conflict requires deterministic ordering: " + ToString(conflict.kind) + ".", "Serialize the jobs with graph dependencies or keep one workload protected.");
			}
		}

		SortDiagnostics(diagnostics);
		return diagnostics;
	}

	std::string FormatComponentAccessDiagnostics(const std::vector<ComponentAccessDiagnostic>& diagnostics)
	{
		if (diagnostics.empty())
		{
			return "Component access diagnostics: none";
		}
		std::ostringstream stream;
		stream << "Component access diagnostics:";
		for (const ComponentAccessDiagnostic& diagnostic : diagnostics)
		{
			stream << "\n- " << ToString(diagnostic.severity)
				<< "/" << ToString(diagnostic.code)
				<< " system='" << diagnostic.ownerSystem
				<< "' query='" << diagnostic.queryName
				<< "' resource='" << diagnostic.resourceName
				<< "': " << diagnostic.message
				<< " Action: " << diagnostic.recommendedAction;
		}
		return stream.str();
	}
}
