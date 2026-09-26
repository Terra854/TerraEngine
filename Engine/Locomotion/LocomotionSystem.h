// LocomotionSystem.h - protected character binding and movement coordination.

#pragma once

#include "Core/SystemBase.h"
#include "LocomotionCommands.h"
#include "LocomotionParameterTrace.h"

#include <memory>
#include <optional>
#include <span>

class ObjectManager;

namespace TerraEngine::Locomotion {
	struct PublishedLocomotionBinding {
		std::uint32_t entity = 0U;
		LocomotionHandle handle{};
	};

	// One protected read supplies the exact identity and acquisition preconditions.
	// Interop copies these values; it never keeps a component or runtime address.
	struct PublishedLocomotionResolution {
		LocomotionHandle handle{};
		LocomotionReason bindingReason = LocomotionReason::None;
		std::uint64_t configurationRevision = 0U;
		std::uint64_t authorityEpoch = 0U;
	};

	// Validity and the retained immutable root are captured at one protected
	// instant. A replacement cannot slip between separate identity/root reads.
	struct PublishedLocomotionObservationSnapshot {
		bool handleCurrent = false;
		std::shared_ptr<const LocomotionObservationRoot> root;
	};

	// Diagnostics are copied values. A caller cannot use this publication to
	// retain an ECS address or to bypass the command/physics authority boundary.
	struct LocomotionDiagnostics {
		std::uint64_t publicationEpoch = 0U;
		std::size_t liveSlots = 0U;
		std::size_t pendingCommands = 0U;
		std::size_t preparedCharacters = 0U;
		std::size_t activeShards = 0U;
		std::uint64_t lastPublishMicroseconds = 0U;
		std::uint64_t rejectedTargets = 0U;
		std::string lastTargetFailure;
	};

	class TERRAENGINE_API LocomotionSystem final : public SystemsBase {
	public:
		static LocomotionSystem& Instance();
		void Initialize() override;
		void Update(float deltaTime) override;
		void Shutdown() override;
		std::string SystemName() override;
		Scheduling::SystemDeclaration SchedulerDeclaration() override;
		std::vector<Scheduling::FrameWorkloadDeclaration> DeclareFrameWorkloads(float deltaTime) override;

		// All public gameplay calls use immutable bindings and copied envelopes.
		// Resolve never silently turns an old wrapper into a replacement attachment.
		[[nodiscard]] std::optional<LocomotionHandle> ResolvePublished(std::uint32_t entity) const;
		[[nodiscard]] std::optional<LocomotionHandle> ResolvePublished(UUID objectId) const;
		[[nodiscard]] std::optional<PublishedLocomotionResolution> ResolvePublishedSnapshot(UUID objectId) const;
		[[nodiscard]] CommandBatchAdmission SubmitBatch(std::span<const LocomotionCommand> commands);
		[[nodiscard]] CommandReceipt ReadReceipt(LocomotionHandle handle, std::uint64_t producerId,
			std::uint64_t sequence, std::uint64_t correlationId) const;
		[[nodiscard]] std::optional<CommandReceipt> ReadCurrentReceipt(LocomotionHandle handle, std::uint64_t producerId,
			std::uint64_t sequence, std::uint64_t correlationId) const;
		[[nodiscard]] std::shared_ptr<const LocomotionObservationRoot> CopyObservations() const;
		[[nodiscard]] PublishedLocomotionObservationSnapshot CopyObservationsForHandle(LocomotionHandle handle) const;
		[[nodiscard]] LocomotionDiagnostics ReadDiagnostics() const;
		// Disabled by default. Configure before capture, read latestSequence as the
		// starting cursor, then reject lostRecords in every subsequent read.
		bool ConfigureParameterTrace(bool enabled, std::size_t capacity = 4096U);
		LocomotionParameterTraceSnapshot CopyParameterTrace(std::uint64_t afterSequence) const;

		// MainEngine and scripting invoke this only after a protected publication.
		// Workers never inspect live components, revoke leases or publish transforms.
		void ObserveProtectedBoundary();
		void RevokeAll(LocomotionReason reason = LocomotionReason::Revoked);

	private:
		LocomotionSystem();
		~LocomotionSystem() override;
		LocomotionSystem(const LocomotionSystem&) = delete;
		LocomotionSystem& operator=(const LocomotionSystem&) = delete;
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};
}
