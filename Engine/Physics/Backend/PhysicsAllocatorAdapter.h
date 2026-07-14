// PhysicsAllocatorAdapter.h - Provider-private Jolt memory and diagnostics bridge.
//
// Jolt requires process-global callback pointers for allocation and issue
// reporting. This RAII adapter owns those pointers only while a physics backend
// lifetime is active, so gameplay-facing physics contracts never name Jolt.

#pragma once

#include "Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace TerraEngine::Physics::Backend {
	// Provider failures are translated before they leave the backend boundary.
	// Callers can preserve this compact value in diagnostics without storing Jolt
	// enums, callback pointers, or provider-owned strings.
	enum class PhysicsProviderErrorCode : std::uint8_t {
		None,
		AllocationFailure,
		AssertionFailure,
		TraceMessage
	};

	struct PhysicsProviderError {
		PhysicsProviderErrorCode code = PhysicsProviderErrorCode::None;
		std::string message;
		std::string sourceFile;
		std::uint32_t sourceLine = 0U;
	};

	// Jolt's free callbacks do not supply an allocation size. The adapter therefore
	// reports exact request/failure counts and an exact outstanding allocation count,
	// but intentionally does not claim an unavailable live-byte total.
	struct PhysicsAllocationDiagnostics {
		std::uint64_t allocationRequests = 0U;
		std::uint64_t reallocationRequests = 0U;
		std::uint64_t alignedAllocationRequests = 0U;
		std::uint64_t freeRequests = 0U;
		std::uint64_t allocationFailures = 0U;
		std::uint64_t outstandingAllocations = 0U;
		std::uint64_t highWaterOutstandingAllocations = 0U;
		std::uint64_t traceMessages = 0U;
		std::uint64_t assertionFailures = 0U;
		std::uint64_t suppressedLogMessages = 0U;
	};

	// PhysicsBackendProfileScope maps a named backend operation onto TerraEngine's
	// profiler. It replaces provider profiling macros, which remain disabled in the
	// production Jolt profile, while preserving useful named CPU timing scopes.
	class TERRAENGINE_API PhysicsBackendProfileScope final {
	public:
		explicit PhysicsBackendProfileScope(const char* name) noexcept;
		~PhysicsBackendProfileScope();

		PhysicsBackendProfileScope(const PhysicsBackendProfileScope&) = delete;
		PhysicsBackendProfileScope& operator=(const PhysicsBackendProfileScope&) = delete;

	private:
		bool active = false;
	};

	// PhysicsAllocatorAdapter installs Jolt's global custom allocator, trace, and
	// assertion callbacks. Instances may be nested, but the first installation
	// captures prior pointers and the final destruction restores exactly those
	// pointers. The backend must destroy all Jolt objects before final teardown.
	class TERRAENGINE_API PhysicsAllocatorAdapter final {
	public:
		PhysicsAllocatorAdapter() noexcept;
		~PhysicsAllocatorAdapter();

		PhysicsAllocatorAdapter(const PhysicsAllocatorAdapter&) = delete;
		PhysicsAllocatorAdapter& operator=(const PhysicsAllocatorAdapter&) = delete;

		[[nodiscard]] bool IsInstalled() const noexcept;
		[[nodiscard]] PhysicsAllocationDiagnostics Diagnostics() const noexcept;
		[[nodiscard]] PhysicsProviderError LastError() const;
		// Acceptance tests use the real translated provider-health path without
		// forcing the process allocator to fail unpredictably. This records the same
		// bounded failure counter and report consumed by JoltPhysicsBackend.
		void ReportAllocationFailureForValidation(std::size_t requestedBytes) noexcept;

	private:
		bool installed = false;
	};
}
