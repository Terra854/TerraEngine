// ProceduralGeometryPublication.h - Transactional generated-geometry ingress.
//
// Worker-produced geometry candidates never mutate live scene or renderer
// storage. Source edits become visible at an explicit protected flush, while a
// second protected boundary atomically swaps a fully validated generation.

#pragma once

#include "Core/Export.h"
#include "Renderer/VirtualGeometry/VirtualGeometryTypes.h"

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace TerraEngine::Renderer
{
	enum class ProceduralGeometryEdit : std::uint8_t
	{
		ControlPointChanged = 0u,
		MaterialChanged = 1u,
		DisplacementChanged = 2u,
		TopologyChanged = 3u
	};

	struct ProceduralGeometryTransaction
	{
		std::uint64_t transactionIdentity = 0u;
		std::uint64_t objectIdentity = 0u;
	};

	struct ProceduralGeometryCandidate
	{
		std::uint64_t candidateIdentity = 0u;
		std::uint64_t objectIdentity = 0u;
		std::uint64_t sourceGeneration = 0u;
		std::uint64_t targetGeneration = 0u;
		std::uint64_t validationIdentity = 0u;
	};

	class TERRAENGINE_API ProceduralGeometryPublication
	{
	public:
		[[nodiscard]] ProceduralGeometryTransaction BeginTransaction(
			std::uint64_t objectIdentity);

		[[nodiscard]] bool SubmitSourceEdit(
			const ProceduralGeometryTransaction &transaction,
			ProceduralGeometryEdit edit);

		[[nodiscard]] bool IsVisible(std::uint64_t objectIdentity) const;

		// Must run at the declared protected scene-mutation boundary.
		void FlushSceneMutations();

		[[nodiscard]] std::uint64_t CurrentGeneration(
			std::uint64_t objectIdentity) const;

		[[nodiscard]] ProceduralGeometryCandidate PrepareCandidate(
			std::uint64_t objectIdentity,
			std::uint64_t sourceGeneration);

		// Must run at GeometryAssetGenerationPublish. Rejection is atomic.
		[[nodiscard]] bool Publish(
			const ProceduralGeometryCandidate &candidate);

		void Cancel(const ProceduralGeometryCandidate &candidate);

		[[nodiscard]] bool RejectConflictingTransaction(
			std::uint64_t objectIdentity);

		[[nodiscard]] bool RetireAfterFence(
			std::uint64_t objectIdentity,
			std::uint64_t generation);

		[[nodiscard]] std::uint64_t ViewGeneration(
			std::uint64_t objectIdentity,
			GeometryViewKind view) const;

	private:
		struct PendingEdit
		{
			ProceduralGeometryTransaction transaction{};
			ProceduralGeometryEdit edit = ProceduralGeometryEdit::ControlPointChanged;
		};

		[[nodiscard]] std::uint64_t CurrentGenerationLocked(
			std::uint64_t objectIdentity) const;

		mutable std::mutex m_mutex;
		std::uint64_t m_nextTransactionIdentity = 1u;
		std::uint64_t m_nextCandidateIdentity = 1u;
		std::vector<PendingEdit> m_pendingEdits;
		std::unordered_set<std::uint64_t> m_visibleObjects;
		std::unordered_set<std::uint64_t> m_conflictedObjects;
		std::unordered_set<std::uint64_t> m_cancelledCandidates;
		std::unordered_map<std::uint64_t, std::uint64_t> m_currentGenerations;
		std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> m_retiringGenerations;
	};
} // namespace TerraEngine::Renderer
