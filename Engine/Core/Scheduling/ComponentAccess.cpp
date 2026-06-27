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

		void NormalizeScopes(std::vector<ComponentAccessScope>& scopes)
		{
			if (scopes.empty())
			{
				scopes.push_back(ComponentAccessScope::Component);
				return;
			}
			std::stable_sort(scopes.begin(), scopes.end(), [](ComponentAccessScope left, ComponentAccessScope right) {
				return static_cast<int>(left) < static_cast<int>(right);
			});
			scopes.erase(std::unique(scopes.begin(), scopes.end()), scopes.end());
		}

		bool HasScope(const ComponentAccessDeclaration& declaration, ComponentAccessScope scope)
		{
			return std::find(declaration.scopes.begin(), declaration.scopes.end(), scope) != declaration.scopes.end();
		}

		bool HasAnyWrite(const ComponentAccessDeclaration& declaration)
		{
			return !declaration.writeComponents.empty() || !declaration.writeResources.empty();
		}

		bool HasProtectedOnlyScope(const ComponentAccessDeclaration& declaration)
		{
			return HasScope(declaration, ComponentAccessScope::ProtectedWrite)
				|| HasScope(declaration, ComponentAccessScope::Editor);
		}

		bool HasReadOnlySnapshotScope(const ComponentAccessDeclaration& declaration)
		{
			return HasScope(declaration, ComponentAccessScope::Snapshot)
				|| HasScope(declaration, ComponentAccessScope::Input)
				|| HasScope(declaration, ComponentAccessScope::Window);
		}

		std::string FormatScopes(const std::vector<ComponentAccessScope>& scopes)
		{
			std::ostringstream stream;
			for (std::size_t index = 0; index < scopes.size(); ++index)
			{
				if (index != 0)
				{
					stream << ",";
				}
				stream << ToString(scopes[index]);
			}
			return stream.str();
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

	std::string ToString(ComponentAccessGrantMode mode)
	{
		switch (mode)
		{
		case ComponentAccessGrantMode::WorkerRead:
			return "worker-read";
		case ComponentAccessGrantMode::WorkerWriteDeferred:
			return "worker-write-deferred";
		case ComponentAccessGrantMode::ProtectedCommit:
		default:
			return "protected-commit";
		}
	}

	std::string ToString(ComponentAccessScope scope)
	{
		switch (scope)
		{
		case ComponentAccessScope::Snapshot:
			return "snapshot";
		case ComponentAccessScope::DeferredWrite:
			return "deferred-write";
		case ComponentAccessScope::ProtectedWrite:
			return "protected-write";
		case ComponentAccessScope::Render:
			return "render";
		case ComponentAccessScope::Script:
			return "script";
		case ComponentAccessScope::Input:
			return "input";
		case ComponentAccessScope::Window:
			return "window";
		case ComponentAccessScope::Resource:
			return "resource";
		case ComponentAccessScope::UI:
			return "ui";
		case ComponentAccessScope::Editor:
			return "editor";
		case ComponentAccessScope::Component:
		default:
			return "component";
		}
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
		case ComponentAccessDiagnosticCode::AccessScopeConflict:
			return "access-scope-conflict";
		case ComponentAccessDiagnosticCode::StaleGeneration:
			return "stale-generation";
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
		NormalizeScopes(declaration.scopes);
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
		if (result.normalized.workerEligible && HasProtectedOnlyScope(result.normalized))
		{
			result.protectedFallback = true;
			AddDiagnostic(result.diagnostics, ComponentAccessDiagnosticSeverity::Warning, ComponentAccessDiagnosticCode::ProtectedFallback, result.normalized, FormatScopes(result.normalized.scopes), "Worker-eligible declaration includes a protected-only scope.", "Split worker-safe preparation from editor/protected commits, then run the commit on the protected path.");
		}
		if (HasReadOnlySnapshotScope(result.normalized) && HasAnyWrite(result.normalized))
		{
			AddDiagnostic(result.diagnostics, ComponentAccessDiagnosticSeverity::Error, ComponentAccessDiagnosticCode::AccessScopeConflict, result.normalized, FormatScopes(result.normalized.scopes), "Read-only snapshot/input/window scopes cannot declare writes.", "Move writes into deferred-write or protected-write scopes after the immutable snapshot is consumed.");
		}
		if (result.normalized.snapshotGeneration != 0
			&& result.normalized.currentGeneration != 0
			&& result.normalized.snapshotGeneration != result.normalized.currentGeneration)
		{
			AddDiagnostic(result.diagnostics, ComponentAccessDiagnosticSeverity::Error, ComponentAccessDiagnosticCode::StaleGeneration, result.normalized, "snapshot-generation", "Component access snapshot generation is stale.", "Discard this workload and rebuild it from the latest scene/component generation.");
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

	ComponentAccessGrantResult ValidateComponentAccessGrant(ComponentAccessGrantRequest request)
	{
		ComponentAccessGrantResult result;
		ComponentAccessDeclaration declaration;
		declaration.ownerSystem = request.workloadName;
		declaration.queryName = request.grantId.empty() ? request.workloadName : request.grantId;
		declaration.readComponents = std::move(request.readComponents);
		declaration.writeComponents = std::move(request.writeComponents);
		declaration.scopes = { request.scopeKind };
		declaration.snapshotGeneration = request.snapshotGeneration;
		declaration.currentGeneration = request.currentGeneration;
		declaration.workerEligible = request.schedulerValidated;
		declaration.mainThreadReason = request.schedulerValidated ? std::string{} : "Scheduler did not validate this hot-loop access grant.";
		std::vector<ComponentAccessDiagnostic> normalizationDiagnostics;
		declaration = NormalizeComponentAccessDeclaration(std::move(declaration), &normalizationDiagnostics);
		result.diagnostics = std::move(normalizationDiagnostics);

		result.normalized.grantId = declaration.queryName;
		result.normalized.workloadName = declaration.ownerSystem;
		result.normalized.readComponents = declaration.readComponents;
		result.normalized.writeComponents = declaration.writeComponents;
		result.normalized.scope = request.scope.empty()
			? ToString(request.grantMode)
			: request.scope + ":" + ToString(request.scopeKind) + ":" + ToString(request.grantMode);
		result.normalized.scopeKind = request.scopeKind;
		result.normalized.snapshotGeneration = request.snapshotGeneration;
		result.normalized.currentGeneration = request.currentGeneration;
		result.normalized.schedulerValidated = request.schedulerValidated;

		if (HasReadOnlySnapshotScope(declaration) && HasAnyWrite(declaration))
		{
			result.fallbackReason = "Read-only snapshot/input/window scopes cannot grant component writes.";
			AddDiagnostic(result.diagnostics, ComponentAccessDiagnosticSeverity::Error, ComponentAccessDiagnosticCode::AccessScopeConflict, declaration, FormatScopes(declaration.scopes), result.fallbackReason, "Use a deferred write grant after snapshot preparation, or keep the write on the protected path.");
			SortDiagnostics(result.diagnostics);
			return result;
		}
		if (declaration.snapshotGeneration != 0
			&& declaration.currentGeneration != 0
			&& declaration.snapshotGeneration != declaration.currentGeneration)
		{
			result.fallbackReason = "Component access grant references a stale snapshot generation.";
			AddDiagnostic(result.diagnostics, ComponentAccessDiagnosticSeverity::Error, ComponentAccessDiagnosticCode::StaleGeneration, declaration, "snapshot-generation", result.fallbackReason, "Rebuild the worker snapshot before granting component access.");
			SortDiagnostics(result.diagnostics);
			return result;
		}

		if (request.grantMode == ComponentAccessGrantMode::ProtectedCommit)
		{
			result.granted = true;
			result.fallbackReason = "Protected commit grants run on the serialized commit path.";
			AddDiagnostic(result.diagnostics, ComponentAccessDiagnosticSeverity::Info, ComponentAccessDiagnosticCode::ProtectedFallback, declaration, "", result.fallbackReason, "No worker access is granted; apply side effects at the protected commit point.");
			SortDiagnostics(result.diagnostics);
			return result;
		}
		if (!request.schedulerValidated)
		{
			result.fallbackReason = "Scheduler validation is required before hot-loop component access can bypass protected locks.";
			AddDiagnostic(result.diagnostics, ComponentAccessDiagnosticSeverity::Warning, ComponentAccessDiagnosticCode::ProtectedFallback, declaration, "", result.fallbackReason, "Run the workload through scheduler/query validation or use reflected protected access.");
			return result;
		}
		if (request.grantMode == ComponentAccessGrantMode::WorkerRead && !declaration.writeComponents.empty())
		{
			result.fallbackReason = "Worker-read grants cannot include component writes.";
			AddDiagnostic(result.diagnostics, ComponentAccessDiagnosticSeverity::Error, ComponentAccessDiagnosticCode::AccessConflict, declaration, "", result.fallbackReason, "Use a deferred worker-write grant or a protected commit grant.");
			SortDiagnostics(result.diagnostics);
			return result;
		}
		if (declaration.readComponents.empty() && declaration.writeComponents.empty())
		{
			result.fallbackReason = "Component access grant has no component reads or writes.";
			AddDiagnostic(result.diagnostics, ComponentAccessDiagnosticSeverity::Error, ComponentAccessDiagnosticCode::MissingWorkerDeclaration, declaration, "", result.fallbackReason, "Declare the component set before requesting a hot-loop access grant.");
			return result;
		}
		result.granted = std::none_of(result.diagnostics.begin(), result.diagnostics.end(), [](const ComponentAccessDiagnostic& diagnostic) {
			return diagnostic.severity == ComponentAccessDiagnosticSeverity::Error;
		});
		if (!result.granted && result.fallbackReason.empty())
		{
			result.fallbackReason = "Component access grant was denied by diagnostics.";
		}
		SortDiagnostics(result.diagnostics);
		return result;
	}
}
