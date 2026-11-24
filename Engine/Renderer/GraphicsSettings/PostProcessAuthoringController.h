// Protected copied authoring state. No scene storage, GPU handles or file access.
#pragma once
#include "Scripting/PostProcessAuthoringWire.h"
#include <mutex>
#include <unordered_map>

namespace TerraEngine::Renderer::GraphicsSettings
{
	namespace AuthoringWire = TerraEngine::Scripting::PostProcessAuthoringWire;
	struct PostProcessSceneProfileIntent
	{
		GraphicsRequestId requestId = 0;
		GraphicsTarget target;
		GraphicsLifecycleStamp lifecycle;
		GraphicsRevision baseRevision = 0;
		UUID profileAssetId = EMPTY_UUID;
	};
	struct PostProcessAuthoringAdmission
	{
		AuthoringWire::Receipt receipt;
		std::optional<PostProcessSceneProfileIntent> sceneIntent;
	};
	struct PostProcessAuthoringReadResult
	{
		std::shared_ptr<const AuthoringWire::Snapshot> snapshot;
		GraphicsRequestReason reason = GraphicsRequestReason::None;
		std::string detail;
	};
	class PostProcessAuthoringController
	{
	public:
		static constexpr std::size_t kMaximumViews = 128;
		static constexpr std::size_t kMaximumReceipts = 4096;
		// Called at the existing protected publication boundary with real retained
		// descriptor owners and actual authority. Initial effective/frame facts
		// must already be covered by native receipts; this method invents none.
		// Same-lifetime refresh replaces the authored baseline atomically and
		// reapplies accepted transient overrides. Pending work blocks refresh except
		// the real post-flush baseline resolving an accepted profile scene intent.
		bool PublishView(const AuthoringWire::Snapshot& baseline, AuthoringWire::Diagnostic& diagnostic);
		PostProcessAuthoringReadResult Read(const GraphicsTarget& target) const;
		// Returns the exact retained descriptor/value root last acknowledged by
		// native output, independent of later requested descriptors or revisions.
		PostProcessAuthoringReadResult ReadAcceptedView(const GraphicsTarget& target) const;
		PostProcessAuthoringAdmission Submit(const AuthoringWire::Request& request);
		std::optional<AuthoringWire::Receipt> ReadReceipt(GraphicsRequestId requestId) const;
		// A typed scene transaction's accepted protected flush is distinct from
		// enqueue admission. Rejection preserves the old requested/effective roots.
		bool ReportSceneTransaction(GraphicsRequestId requestId, const GraphicsTarget& target,
			const GraphicsLifecycleStamp& lifecycle, bool accepted, std::uint64_t visibleFrameId);
		// Native output must identify this exact view, lifecycle, candidate local
		// revision and real frame. A different view/revision cannot apply a request.
		bool ReportRecordedView(const GraphicsTarget& target, const GraphicsLifecycleStamp& lifecycle,
			GraphicsRevision authoringRevision, std::uint64_t frameId);
		// Failed preparation must become terminal so a caller never waits forever.
		// The retained pre-candidate root is restored without reusing its reserved
		// candidate revision; late callbacks cannot apply a later request by alias.
		bool RejectRequest(GraphicsRequestId requestId, GraphicsRequestReason reason, std::string detail);
		// Native preparation carries a copied candidate identity, never a mutable
		// request lookup. Compare and reject under the same lock so a late failure
		// cannot roll back a newer request for the same physical output.
		bool RejectRequestedView(const GraphicsTarget& target, const GraphicsLifecycleStamp& lifecycle,
			GraphicsRevision candidateRevision, GraphicsRequestReason reason, std::string detail);
		void RetireView(const GraphicsTarget& target, const GraphicsLifecycleStamp& lifecycle);
	private:
		struct Override
		{
			UUID assetId = EMPTY_UUID;
			std::optional<bool> enabled;
			std::vector<CustomPostProcessParameterValue> parameters;
		};
		struct View
		{
			std::shared_ptr<const AuthoringWire::Snapshot> root;
			std::shared_ptr<const AuthoringWire::Snapshot> acceptedRoot;
			std::vector<AuthoringWire::EffectState> baselineEffects;
			std::vector<Override> overrides;
			GraphicsRevision nextRevision = 0;
			GraphicsRevision acceptedRevision = 0;
			GraphicsRequestId pendingRequestId = 0;
			bool baselineReady = true;
		};
		struct Pending
		{
			AuthoringWire::Receipt receipt;
			GraphicsTarget target;
			GraphicsLifecycleStamp lifecycle;
			GraphicsRevision candidateRevision = 0;
			AuthoringWire::Operation operation = AuthoringWire::Operation::PatchCustomEffects;
			UUID profileAssetId = EMPTY_UUID;
			bool sceneAccepted = false;
			bool sceneResolved = false;
			std::shared_ptr<const AuthoringWire::Snapshot> before;
			std::vector<Override> beforeOverrides;
		};
		// Small copied state is serialized under this private mutex. No scheduler
		// work, resource loading, ECS mutation or native command recording runs here.
		mutable std::mutex mutex_;
		std::vector<View> views_;
		std::unordered_map<GraphicsRequestId, Pending> receipts_;
		View* Find(const GraphicsTarget& target);
		const View* Find(const GraphicsTarget& target) const;
		static bool ResolveEffects(const AuthoringWire::Snapshot& schema,
			const std::vector<AuthoringWire::EffectState>& baseline, const std::vector<Override>& overrides,
			std::vector<AuthoringWire::EffectState>& output, std::string& detail);
		void RejectLocked(Pending& pending, GraphicsRequestReason reason, std::string detail);
	};
}
