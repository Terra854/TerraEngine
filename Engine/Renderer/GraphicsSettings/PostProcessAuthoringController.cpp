// Small protected transactions over retained immutable authoring roots.
#include "PostProcessAuthoringController.h"
#include "Resource/PostProcessEffectAsset.h"
#include "Scripting/ScriptGlue.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <set>

namespace TerraEngine::Renderer::GraphicsSettings
{
	namespace
	{
		using Wire = AuthoringWire::Snapshot;
		using Reason = GraphicsRequestReason;
		// Only the allocator is process-wide: controller state remains owned by
		// RenderSystem. CAS prevents wraparound from recycling an old request ID.
		std::atomic<std::uint64_t> nextRequestId{1};
		std::uint64_t AllocateId()
		{
			auto value = nextRequestId.load(std::memory_order_relaxed);
			while (value && value != UINT64_MAX) {
				if (nextRequestId.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return value;
			} return 0;
		}
		bool Exact(const GraphicsTarget& target)
		{
			if (target.scope != GraphicsScope::Camera || !target.camera || !target.view) return false;
			const auto& c = *target.camera; const auto& v = *target.view;
			return c.cameraId != EMPTY_UUID && c.sceneGeneration && c.objectLifetimeGeneration && c.componentGeneration && c.bindingGeneration
				&& v.lifetimeGeneration && static_cast<std::uint32_t>(v.viewType) <= 3 && static_cast<std::uint32_t>(v.outputKind) <= 4;
		}
		bool Authority(const GraphicsLifecycleStamp& l) { return l.projectGeneration && l.rendererSessionGeneration && l.backendResourceGeneration; }
		bool Matches(const GraphicsTarget& query, const GraphicsTarget& actual)
		{
			return query.scope == GraphicsScope::Camera && query.camera && query.camera == actual.camera && (!query.view || query.view == actual.view);
		}
		bool Finite(const CustomPostProcessValue& value)
		{
			return std::visit([](const auto& v) {
				using T = std::decay_t<decltype(v)>;
				if constexpr (std::is_same_v<T, float>) return std::isfinite(v);
				else if constexpr (requires { v.begin(); }) return std::all_of(v.begin(), v.end(), [](float x) { return std::isfinite(x); });
				else return true;
			}, value);
		}
		bool LessEqual(const CustomPostProcessValue& a, const CustomPostProcessValue& b)
		{
			if (a.index() != b.index()) return false;
			return std::visit([&](const auto& value) {
				const auto& other = std::get<std::decay_t<decltype(value)>>(b);
				if constexpr (requires { value.begin(); }) { for (std::size_t i = 0; i < value.size(); ++i) if (value[i] > other[i]) return false; return true; }
				else return value <= other;
			}, a);
		}
		bool Valid(const CustomPostProcessParameter& schema, const CustomPostProcessValue& value)
		{
			return value.index() == static_cast<std::size_t>(schema.layout.kind) && Finite(value)
				&& (!schema.minimum || LessEqual(*schema.minimum, value)) && (!schema.maximum || LessEqual(value, *schema.maximum));
		}
		bool ValuesEqual(const std::vector<CustomPostProcessParameterValue>& a, const std::vector<CustomPostProcessParameterValue>& b)
		{
			if (a.size() != b.size()) return false;
			for (std::size_t i = 0; i < a.size(); ++i) if (a[i].name != b[i].name || a[i].value != b[i].value) return false;
			return true;
		}
		bool EffectsEqual(const std::vector<AuthoringWire::EffectState>& a, const std::vector<AuthoringWire::EffectState>& b)
		{
			if (a.size() != b.size()) return false;
			for (std::size_t i = 0; i < a.size(); ++i) if (a[i].assetId != b[i].assetId || a[i].enabled != b[i].enabled || !ValuesEqual(a[i].parameters, b[i].parameters)) return false;
			return true;
		}
		bool SchemaEqual(const Wire& a, const Wire& b)
		{
			if (a.authoredProfileAssetId != b.authoredProfileAssetId || a.descriptors.size() != b.descriptors.size()) return false;
			for (std::size_t i = 0; i < a.descriptors.size(); ++i) {
				const auto& x = a.descriptors[i]; const auto& y = b.descriptors[i];
				if (x.effect != y.effect || x.available != y.available || x.reason != y.reason || x.detail != y.detail) return false;
			} return true;
		}
		bool Failure(AuthoringWire::Diagnostic& d, Reason reason, const char* text)
		{
			d = {NativeGraphicsSettingsStatus::Rejected, reason, text}; return false;
		}
	}

	PostProcessAuthoringController::View* PostProcessAuthoringController::Find(const GraphicsTarget& target)
	{
		View* result = nullptr;
		for (auto& view : views_) if (Matches(target, view.root->target)) { if (result) return nullptr; result = &view; }
		return result;
	}
	const PostProcessAuthoringController::View* PostProcessAuthoringController::Find(const GraphicsTarget& target) const
	{
		const View* result = nullptr;
		for (const auto& view : views_) if (Matches(target, view.root->target)) { if (result) return nullptr; result = &view; }
		return result;
	}

	bool PostProcessAuthoringController::ResolveEffects(const Wire& schema, const std::vector<AuthoringWire::EffectState>& baseline,
		const std::vector<Override>& overrides, std::vector<AuthoringWire::EffectState>& output, std::string& detail)
	{
		auto fail = [&](const char* message) { detail = message; return false; };
		if (schema.descriptors.size() > 64 || baseline.size() > 64 || overrides.size() > 64) return fail("Effect capacity exceeded.");
		std::vector<AuthoringWire::EffectState> result; std::set<UUID> descriptorIds, baseIds, overrideIds;
		for (const auto& d : schema.descriptors) {
			if (!d.effect || !descriptorIds.insert(d.effect->descriptor.assetId).second) return fail("Missing or duplicate retained effect descriptor.");
			const auto& descriptor = d.effect->descriptor;
			// Reuse the asset codec's complete structural/default/bounds checks.
			// This validates metadata only, never shader readiness or GPU support.
			std::vector<std::byte> encoded; std::string error;
			if (!Resource::SerializePostProcessEffectAsset(*d.effect, encoded, error)) return fail("Malformed retained effect descriptor.");
			AuthoringWire::EffectState state; state.assetId = descriptor.assetId;
			for (const auto& parameter : descriptor.parameters) state.parameters.push_back({parameter.layout.name, parameter.defaultValue});
			const auto base = std::find_if(baseline.begin(), baseline.end(), [&](const auto& b) { return b.assetId == state.assetId; });
			const auto patch = std::find_if(overrides.begin(), overrides.end(), [&](const auto& b) { return b.assetId == state.assetId; });
			auto apply = [&](const std::vector<CustomPostProcessParameterValue>& values) {
				if (values.size() > 32) return false; std::set<std::string> names;
				for (const auto& v : values) {
					if (!names.insert(v.name).second) return false;
					const auto p = std::find_if(descriptor.parameters.begin(), descriptor.parameters.end(), [&](const auto& s) { return s.layout.name == v.name; });
					if (p == descriptor.parameters.end() || !Valid(*p, v.value)) return false;
					state.parameters[static_cast<std::size_t>(p - descriptor.parameters.begin())].value = v.value;
				} return true;
			};
			if (base != baseline.end()) { state.enabled = base->enabled; if (!apply(base->parameters)) return fail("Invalid authored baseline parameter."); }
			if (patch != overrides.end()) { if (patch->enabled) state.enabled = *patch->enabled; if (!apply(patch->parameters)) return fail("Unknown parameter, wrong type, nonfinite value or out-of-range patch."); }
			if (state.enabled && !d.available) return fail("Enabled effect has no certified native availability.");
			result.push_back(std::move(state));
		}
		for (const auto& b : baseline) if (!descriptorIds.contains(b.assetId) || !baseIds.insert(b.assetId).second) return fail("Baseline refers to missing or duplicate effect.");
		for (const auto& o : overrides) if (!descriptorIds.contains(o.assetId) || !overrideIds.insert(o.assetId).second) return fail("Override refers to missing or duplicate effect.");
		output = std::move(result); detail.clear(); return true;
	}

	bool PostProcessAuthoringController::PublishView(const Wire& baseline, AuthoringWire::Diagnostic& diagnostic)
	{
		if (!Exact(baseline.target) || !Authority(baseline.lifecycle) || !baseline.revision)
			return Failure(diagnostic, Reason::StaleLifetime, "Publication requires exact current camera, view, lifecycle and nonzero local revision.");
		if (!baseline.effectiveFrameId && !baseline.effectiveEffects.empty())
			return Failure(diagnostic, Reason::PreparationFailed, "Effective effect payload requires covering native frame proof.");
		std::lock_guard lock(mutex_);
		auto* existing = Find(baseline.target);
		if (existing && existing->baselineReady && !existing->pendingRequestId && existing->root->lifecycle == baseline.lifecycle && SchemaEqual(*existing->root, baseline) && EffectsEqual(existing->baselineEffects, baseline.requestedEffects)) { diagnostic = {}; return true; }
		Pending* resolvingScene = nullptr;
		if (existing && existing->pendingRequestId) {
			auto found = receipts_.find(existing->pendingRequestId);
			if (found != receipts_.end() && found->second.sceneAccepted && !found->second.sceneResolved
				&& found->second.lifecycle == baseline.lifecycle && found->second.profileAssetId == baseline.authoredProfileAssetId) resolvingScene = &found->second;
			else return Failure(diagnostic, Reason::ConflictingRequest, "A candidate is awaiting protected scene/native completion.");
		}
		if (!existing && views_.size() >= kMaximumViews) return Failure(diagnostic, Reason::RequestCapacityExceeded, "Authoring view capacity exceeded.");
		std::vector<AuthoringWire::EffectState> requested; std::string detail;
		const bool sameLifetime = existing && existing->root->lifecycle == baseline.lifecycle;
		const std::vector<Override> noOverrides;
		if (!ResolveEffects(baseline, baseline.requestedEffects, sameLifetime ? existing->overrides : noOverrides, requested, detail))
			return Failure(diagnostic, Reason::InvalidValue, detail.c_str());
		auto root = std::make_shared<Wire>(baseline); root->requestedEffects = std::move(requested);
		auto baselineEffects = baseline.requestedEffects;
		if (sameLifetime) {
			if (!resolvingScene && existing->nextRevision == UINT64_MAX) return Failure(diagnostic, Reason::RequestCapacityExceeded, "Authoring revision exhausted.");
			root->revision = resolvingScene ? resolvingScene->candidateRevision : existing->nextRevision + 1;
			// A baseline refresh cannot replace old accepted output with requested
			// effects. Only ReportRecordedView certifies the new revision.
			root->effectiveEffects = existing->root->effectiveEffects; root->effectiveFrameId = existing->root->effectiveFrameId;
			root->sceneVisibleFrameId = std::max(root->sceneVisibleFrameId, existing->root->sceneVisibleFrameId);
		} else if (baseline.effectiveFrameId) {
			std::vector<AuthoringWire::EffectState> accepted;
			if (!ResolveEffects(baseline, baseline.effectiveEffects, noOverrides, accepted, detail)) return Failure(diagnostic, Reason::InvalidValue, detail.c_str());
			root->effectiveEffects = std::move(accepted);
		}
		std::shared_ptr<const Wire> initialAccepted;
		if (!sameLifetime && baseline.effectiveFrameId) {
			auto accepted = std::make_shared<Wire>(*root); accepted->requestedEffects = accepted->effectiveEffects;
			initialAccepted = std::move(accepted);
		}
		if (!existing) { views_.push_back({}); existing = &views_.back(); }
		if (!sameLifetime) {
			existing->overrides.clear(); existing->acceptedRevision = baseline.effectiveFrameId ? baseline.revision : 0;
			existing->acceptedRoot = std::move(initialAccepted);
		}
		if (resolvingScene) resolvingScene->sceneResolved = true;
		existing->baselineEffects = std::move(baselineEffects); existing->baselineReady = true; existing->nextRevision = root->revision; existing->root = std::move(root);
		diagnostic = {}; return true;
	}

	PostProcessAuthoringReadResult PostProcessAuthoringController::Read(const GraphicsTarget& target) const
	{
		std::lock_guard lock(mutex_); const auto* view = Find(target);
		if (view) return {view->root};
		const auto count = std::count_if(views_.begin(), views_.end(), [&](const auto& v) { return Matches(target, v.root->target); });
		return {nullptr, count > 1 ? Reason::AmbiguousView : Reason::TargetUnavailable, count > 1 ? "Select a concrete camera output view." : "Camera view lifetime is not published."};
	}

	PostProcessAuthoringReadResult PostProcessAuthoringController::ReadAcceptedView(const GraphicsTarget& target) const
	{
		std::lock_guard lock(mutex_); const auto* view = Find(target);
		if (view && view->acceptedRoot) return {view->acceptedRoot};
		const auto count = std::count_if(views_.begin(), views_.end(), [&](const auto& v) { return Matches(target, v.root->target); });
		return {nullptr, count > 1 ? Reason::AmbiguousView : Reason::TargetUnavailable,
			count > 1 ? "Select a concrete camera output view." : "No exact accepted native output has been reported."};
	}

	PostProcessAuthoringAdmission PostProcessAuthoringController::Submit(const AuthoringWire::Request& request)
	{
		std::lock_guard lock(mutex_); PostProcessAuthoringAdmission result;
		auto& receipt = result.receipt.request; receipt.requestId = AllocateId(); receipt.sequence = receipt.requestId;
		receipt.state = GraphicsRequestState::Rejected;
		auto reject = [&](Reason reason, const char* text) {
			receipt.reason = reason; receipt.detail = text;
			if (receipt.requestId) {
				if (receipts_.size() >= kMaximumReceipts) {
					auto oldest = receipts_.end();
					for (auto it = receipts_.begin(); it != receipts_.end(); ++it) if (!it->second.before && (oldest == receipts_.end() || it->first < oldest->first)) oldest = it;
					if (oldest != receipts_.end()) receipts_.erase(oldest);
				}
				if (receipts_.size() < kMaximumReceipts) { Pending p; p.receipt = result.receipt; receipts_.emplace(receipt.requestId, std::move(p)); }
			} return result;
		};
		if (!receipt.requestId) return reject(Reason::RequestCapacityExceeded, "Process authoring request ID space exhausted.");
		auto* view = Find(request.target);
		if (!view) {
			const auto count = std::count_if(views_.begin(), views_.end(), [&](const auto& v) { return Matches(request.target, v.root->target); });
			return reject(count > 1 ? Reason::AmbiguousView : Reason::StaleLifetime, "Exact camera/view authority is unavailable or ambiguous.");
		}
		const auto& before = *view->root; receipt.effectiveRevision = view->acceptedRevision;
		if (request.lifecycle != before.lifecycle) return reject(Reason::StaleLifetime, "Authoring lifecycle changed.");
		if (request.baseRevision != before.revision) return reject(Reason::StaleRevision, "Authoring baseline revision changed.");
		if (!view->baselineReady) return reject(Reason::PreparationFailed, "Authored profile baseline has not been resolved after scene visibility.");
		if (view->pendingRequestId) return reject(Reason::ConflictingRequest, "This view already has an incomplete authoring transaction.");
		if (view->nextRevision == UINT64_MAX) return reject(Reason::RequestCapacityExceeded, "Authoring revision exhausted.");
		auto overrides = view->overrides;
		if (request.operation == AuthoringWire::Operation::PatchCustomEffects) {
			if (request.effects.empty() || request.effects.size() > 64 || !request.resetEffectAssetIds.empty() || request.clearAll || request.profileAssetId) return reject(Reason::InvalidValue, "Invalid custom patch shape.");
			std::set<UUID> ids;
			for (const auto& patch : request.effects) {
				if (patch.assetId == EMPTY_UUID || !ids.insert(patch.assetId).second || patch.parameters.size() > 32 || patch.resetParameters.size() > 32 || (!patch.enabled && patch.parameters.empty() && patch.resetParameters.empty())) return reject(Reason::InvalidValue, "Empty or duplicate effect patch.");
				const auto descriptor = std::find_if(before.descriptors.begin(), before.descriptors.end(), [&](const auto& d) { return d.effect && d.effect->descriptor.assetId == patch.assetId; });
				if (descriptor == before.descriptors.end()) return reject(Reason::InvalidValue, "Unknown retained effect descriptor.");
				if (patch.enabled && *patch.enabled && !descriptor->available) return reject(Reason::Unsupported, "Effect is unavailable for this exact view.");
				auto current = std::find_if(overrides.begin(), overrides.end(), [&](const auto& o) { return o.assetId == patch.assetId; });
				if (current == overrides.end()) { overrides.push_back({patch.assetId}); current = std::prev(overrides.end()); }
				if (patch.enabled) current->enabled = patch.enabled;
				std::set<std::string> names;
				for (const auto& value : patch.parameters) {
					if (!names.insert(value.name).second) return reject(Reason::InvalidValue, "Duplicate parameter patch.");
					auto old = std::find_if(current->parameters.begin(), current->parameters.end(), [&](const auto& p) { return p.name == value.name; });
					if (old == current->parameters.end()) current->parameters.push_back(value); else *old = value;
				}
				for (const auto& name : patch.resetParameters) {
					if (!names.insert(name).second || std::none_of(descriptor->effect->descriptor.parameters.begin(), descriptor->effect->descriptor.parameters.end(), [&](const auto& p) { return p.layout.name == name; })) return reject(Reason::InvalidValue, "Unknown or conflicting parameter reset.");
					std::erase_if(current->parameters, [&](const auto& p) { return p.name == name; });
				}
			}
		} else if (request.operation == AuthoringWire::Operation::ResetCustomEffects) {
			if (!request.effects.empty() || request.profileAssetId || request.resetEffectAssetIds.size() > 64 || (request.clearAll ? !request.resetEffectAssetIds.empty() : request.resetEffectAssetIds.empty())) return reject(Reason::InvalidValue, "Reset requires clearAll or explicit effect IDs.");
			if (request.clearAll) overrides.clear();
			else { std::set<UUID> ids; for (const auto& id : request.resetEffectAssetIds) {
				if (id == EMPTY_UUID || !ids.insert(id).second || std::none_of(before.descriptors.begin(), before.descriptors.end(), [&](const auto& d) { return d.effect && d.effect->descriptor.assetId == id; })) return reject(Reason::InvalidValue, "Unknown or duplicate reset effect.");
				std::erase_if(overrides, [&](const auto& o) { return o.assetId == id; });
			} }
		} else if (request.operation == AuthoringWire::Operation::SetCameraProfile) {
			if (!request.effects.empty() || !request.resetEffectAssetIds.empty() || request.clearAll || (request.profileAssetId && *request.profileAssetId == EMPTY_UUID)) return reject(Reason::InvalidValue, "Invalid authored profile operation.");
		} else return reject(Reason::InvalidValue, "Unknown authoring operation.");
		std::vector<AuthoringWire::EffectState> requested; std::string detail;
		if (!ResolveEffects(before, view->baselineEffects, overrides, requested, detail)) return reject(Reason::InvalidValue, detail.c_str());
		// Reserve receipt storage before publishing any candidate root. The small
		// terminal cache is bounded; IDs are never recycled after eviction.
		if (receipts_.size() >= kMaximumReceipts) {
			auto oldest = receipts_.end(); for (auto it = receipts_.begin(); it != receipts_.end(); ++it) if (!it->second.before && (oldest == receipts_.end() || it->first < oldest->first)) oldest = it;
			if (oldest == receipts_.end()) return reject(Reason::RequestCapacityExceeded, "All retained receipt slots are pending."); receipts_.erase(oldest);
		}
		Pending pending; pending.target = before.target; pending.lifecycle = before.lifecycle; pending.before = view->root; pending.beforeOverrides = view->overrides;
		pending.candidateRevision = view->nextRevision + 1; pending.operation = request.operation; pending.profileAssetId = request.profileAssetId.value_or(EMPTY_UUID);
		receipt.state = GraphicsRequestState::Accepted; receipt.requestedRevision = pending.candidateRevision;
		pending.receipt = result.receipt;
		std::shared_ptr<const Wire> nextRoot;
		if (request.operation != AuthoringWire::Operation::SetCameraProfile) { auto root = std::make_shared<Wire>(before); root->revision = pending.candidateRevision; root->requestedEffects = std::move(requested); nextRoot = std::move(root); }
		else result.sceneIntent = PostProcessSceneProfileIntent{receipt.requestId, before.target, before.lifecycle, before.revision, pending.profileAssetId};
		receipts_.emplace(receipt.requestId, std::move(pending)); view->pendingRequestId = receipt.requestId; ++view->nextRevision;
		if (nextRoot) { view->root = std::move(nextRoot); view->overrides = std::move(overrides); }
		return result;
	}

	std::optional<AuthoringWire::Receipt> PostProcessAuthoringController::ReadReceipt(GraphicsRequestId id) const
	{
		std::lock_guard lock(mutex_); const auto found = receipts_.find(id);
		return found == receipts_.end() ? std::nullopt : std::optional(found->second.receipt);
	}
	void PostProcessAuthoringController::RejectLocked(Pending& pending, Reason reason, std::string detail)
	{
		auto* view = Find(pending.target);
		if (view && view->pendingRequestId == pending.receipt.request.requestId && view->root->lifecycle == pending.lifecycle) {
			// Scene acceptance cannot be undone by a renderer failure. Keep the
			// real authored property/visible frame, but preserve old native effects.
			if (!pending.sceneAccepted) { view->root = pending.before; view->overrides = std::move(pending.beforeOverrides); }
			else {
				auto root = std::make_shared<Wire>(*view->root);
				// A late record of the failed candidate cannot become effective.
				// Exhaustion retires this view rather than recycling its revision.
				if (view->nextRevision != UINT64_MAX) root->revision = ++view->nextRevision;
				else root->revision = 0;
				view->root = std::move(root);
			}
			view->pendingRequestId = 0;
		}
		pending.receipt.request.state = GraphicsRequestState::Rejected; pending.receipt.request.reason = reason;
		if (detail.size() > 4096) detail.resize(4096);
		pending.receipt.request.detail = std::move(detail); pending.receipt.request.appliedFrameId = 0;
		pending.receipt.effectiveFrameId = 0; pending.before.reset(); pending.beforeOverrides.clear();
	}
	bool PostProcessAuthoringController::RejectRequest(GraphicsRequestId id, Reason reason, std::string detail)
	{
		std::lock_guard lock(mutex_); auto found = receipts_.find(id);
		if (found == receipts_.end() || !found->second.before || reason == Reason::None) return false;
		RejectLocked(found->second, reason, std::move(detail)); return true;
	}
	bool PostProcessAuthoringController::RejectRequestedView(const GraphicsTarget& target,
		const GraphicsLifecycleStamp& lifecycle, GraphicsRevision revision, Reason reason, std::string detail)
	{
		if (!revision || reason == Reason::None || !Exact(target)) return false;
		std::lock_guard lock(mutex_); auto* view = Find(target);
		if (!view || view->root->target != target || view->root->lifecycle != lifecycle
			|| view->root->revision != revision || !view->pendingRequestId) return false;
		const auto found = receipts_.find(view->pendingRequestId);
		if (found == receipts_.end()) return false;
		auto& pending = found->second;
		if (!pending.before || pending.target != target || pending.lifecycle != lifecycle
			|| pending.candidateRevision != revision) return false;
		RejectLocked(pending, reason, std::move(detail)); return true;
	}
	bool PostProcessAuthoringController::ReportSceneTransaction(GraphicsRequestId id, const GraphicsTarget& target,
		const GraphicsLifecycleStamp& lifecycle, bool accepted, std::uint64_t frame)
	{
		std::lock_guard lock(mutex_); auto found = receipts_.find(id);
		if (found == receipts_.end()) return false; auto& p = found->second; auto* view = Find(target);
		if (!view || !p.before || p.operation != AuthoringWire::Operation::SetCameraProfile || p.sceneAccepted || p.target != target || p.lifecycle != lifecycle || view->root->lifecycle != lifecycle || view->pendingRequestId != id) return false;
		if (!accepted) { RejectLocked(p, Reason::PreparationFailed, "Typed scene transaction was rejected before protected visibility."); return true; }
		if (!frame || frame < view->root->sceneVisibleFrameId) return false;
		auto root = std::make_shared<Wire>(*view->root); root->revision = p.candidateRevision; root->authoredProfileAssetId = p.profileAssetId; root->sceneVisibleFrameId = frame;
		view->root = std::move(root); view->baselineReady = false; p.sceneAccepted = true; p.receipt.sceneVisibleFrameId = frame; return true;
	}
	bool PostProcessAuthoringController::ReportRecordedView(const GraphicsTarget& target, const GraphicsLifecycleStamp& lifecycle, GraphicsRevision revision, std::uint64_t frame)
	{
		if (!frame || !revision || !Exact(target)) return false;
		std::lock_guard lock(mutex_); auto* view = Find(target);
		if (!view || !view->baselineReady || view->root->target != target || view->root->lifecycle != lifecycle || view->root->revision != revision || frame <= view->root->effectiveFrameId) return false;
		Pending* pending = nullptr;
		if (view->pendingRequestId) {
			auto found = receipts_.find(view->pendingRequestId); if (found == receipts_.end()) return false; pending = &found->second;
			if (pending->candidateRevision != revision || (pending->operation == AuthoringWire::Operation::SetCameraProfile && (!pending->sceneAccepted || !pending->sceneResolved))) return false;
			if (pending->sceneAccepted && frame < pending->receipt.sceneVisibleFrameId) return false;
		}
		auto root = std::make_shared<Wire>(*view->root); root->effectiveEffects = root->requestedEffects; root->effectiveFrameId = frame; view->root = std::move(root); view->acceptedRoot = view->root; view->acceptedRevision = revision;
		if (pending) {
			auto& r = pending->receipt; r.request.state = GraphicsRequestState::Applied; r.request.effectiveRevision = revision; r.request.appliedFrameId = frame; r.effectiveFrameId = frame;
			pending->before.reset(); pending->beforeOverrides.clear(); view->pendingRequestId = 0;
		} return true;
	}
	void PostProcessAuthoringController::RetireView(const GraphicsTarget& target, const GraphicsLifecycleStamp& lifecycle)
	{
		std::lock_guard lock(mutex_); auto* view = Find(target);
		if (!view || view->root->target != target || view->root->lifecycle != lifecycle) return;
		if (view->pendingRequestId) { auto found = receipts_.find(view->pendingRequestId); if (found != receipts_.end()) RejectLocked(found->second, Reason::StaleLifetime, "Camera view authority retired."); }
		std::erase_if(views_, [&](const auto& v) { return v.root->target == target && v.root->lifecycle == lifecycle; });
	}
}
