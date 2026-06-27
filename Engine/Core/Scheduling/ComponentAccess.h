// ComponentAccess.h - component/resource access contracts for ECS query jobs.
//
// The scheduler can only run component work on worker threads when every job
// has a clear read/write declaration. These small value types are intentionally
// independent from EnTT so tests, systems, and future editor visualizers can
// reason about safety before any live component storage is touched.

#pragma once

#include "Core/Export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TerraEngine::Scheduling {

	enum class ComponentAccessMode {
		Read,
		Write
	};

	enum class ComponentAccessGrantMode {
		WorkerRead,
		WorkerWriteDeferred,
		ProtectedCommit
	};

	enum class ComponentAccessScope {
		Component,
		Snapshot,
		DeferredWrite,
		ProtectedWrite,
		Render,
		Script,
		Input,
		Window,
		Resource,
		UI,
		Editor
	};

	enum class ComponentAccessConflictKind {
		None,
		ReadRead,
		ReadWrite,
		WriteWrite,
		MissingDeclaration
	};

	enum class ComponentAccessDiagnosticSeverity {
		Info,
		Warning,
		Error
	};

	enum class ComponentAccessDiagnosticCode {
		None,
		EmptyOwner,
		EmptyQuery,
		DuplicateRead,
		DuplicateWrite,
		ReadPromotedToWrite,
		MissingWorkerDeclaration,
		ProtectedFallback,
		AccessConflict,
		AccessScopeConflict,
		StaleGeneration
	};

	struct ComponentAccessDeclaration {
		std::string ownerSystem;
		std::string queryName;
		std::vector<std::string> readComponents;
		std::vector<std::string> writeComponents;
		std::vector<std::string> readResources;
		std::vector<std::string> writeResources;
		std::vector<ComponentAccessScope> scopes;
		std::uint64_t snapshotGeneration = 0;
		std::uint64_t currentGeneration = 0;
		bool workerEligible = false;
		std::string mainThreadReason;
	};

	struct ComponentAccessConflict {
		ComponentAccessConflictKind kind = ComponentAccessConflictKind::None;
		std::string resourceName;
		bool requiresOrdering = false;
	};

	struct ComponentAccessDiagnostic {
		ComponentAccessDiagnosticSeverity severity = ComponentAccessDiagnosticSeverity::Info;
		ComponentAccessDiagnosticCode code = ComponentAccessDiagnosticCode::None;
		std::string ownerSystem;
		std::string queryName;
		std::string resourceName;
		std::string message;
		std::string recommendedAction;
	};

	struct ComponentAccessValidationResult {
		bool valid = true;
		bool protectedFallback = false;
		ComponentAccessDeclaration normalized;
		std::vector<ComponentAccessDiagnostic> diagnostics;
		std::string formatted;
	};

	struct ComponentAccessGrantRequest {
		std::string grantId;
		std::string workloadName;
		std::vector<std::string> readComponents;
		std::vector<std::string> writeComponents;
		std::string scope;
		ComponentAccessScope scopeKind = ComponentAccessScope::Component;
		std::uint64_t snapshotGeneration = 0;
		std::uint64_t currentGeneration = 0;
		ComponentAccessGrantMode grantMode = ComponentAccessGrantMode::WorkerWriteDeferred;
		bool schedulerValidated = false;
	};

	struct ComponentAccessGrantResult {
		bool granted = false;
		ComponentAccessGrantRequest normalized;
		std::vector<ComponentAccessDiagnostic> diagnostics;
		std::string fallbackReason;
	};

	TERRAENGINE_API std::string ToString(ComponentAccessMode mode);
	TERRAENGINE_API std::string ToString(ComponentAccessGrantMode mode);
	TERRAENGINE_API std::string ToString(ComponentAccessScope scope);
	TERRAENGINE_API std::string ToString(ComponentAccessConflictKind kind);
	TERRAENGINE_API std::string ToString(ComponentAccessDiagnosticSeverity severity);
	TERRAENGINE_API std::string ToString(ComponentAccessDiagnosticCode code);
	TERRAENGINE_API ComponentAccessDeclaration NormalizeComponentAccessDeclaration(ComponentAccessDeclaration declaration, std::vector<ComponentAccessDiagnostic>* diagnostics = nullptr);
	TERRAENGINE_API ComponentAccessValidationResult ValidateComponentAccessDeclaration(ComponentAccessDeclaration declaration);
	TERRAENGINE_API ComponentAccessConflict ClassifyComponentAccessConflict(const ComponentAccessDeclaration& left, const ComponentAccessDeclaration& right);
	TERRAENGINE_API std::vector<ComponentAccessDiagnostic> ValidateComponentAccessSet(const std::vector<ComponentAccessDeclaration>& declarations);
	TERRAENGINE_API std::string FormatComponentAccessDiagnostics(const std::vector<ComponentAccessDiagnostic>& diagnostics);
	TERRAENGINE_API ComponentAccessGrantResult ValidateComponentAccessGrant(ComponentAccessGrantRequest request);

}
