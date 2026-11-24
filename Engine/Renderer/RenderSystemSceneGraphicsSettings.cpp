// Scene View settings use copied worker inputs and the existing protected frame
// boundary. They never resolve scene objects or mutate the managed Game owner.
#include "Frame/RenderFrameState.h"
#include "GraphicsSettings/PostProcessColorSettings.h"
#include "RHI/Vulkan/VulkanContext.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <chrono>

namespace
{
	using namespace TerraEngine::Renderer::GraphicsSettings;
	using SceneState = RenderGraphicsSettingsState::SceneGraphics;
	constexpr std::array<GraphicsTarget, 1> SceneTargets{};

	bool SceneColorField(GraphicsSettingId id)
	{
		return id == GraphicsSettingId::FixedExposureCompensation || id == GraphicsSettingId::ExposureMode
			|| id == GraphicsSettingId::ToneMappingMode;
	}

	bool SceneField(GraphicsSettingId id)
	{
		return IsGraphicsAASetting(id) || SceneColorField(id);
	}

	const GraphicsValue* FindSceneValue(const GraphicsSettingsSnapshot& snapshot, GraphicsSettingId id)
	{
		const auto field = std::find_if(snapshot.values.begin(), snapshot.values.end(), [id](const auto& value) { return value.id == id; });
		return field == snapshot.values.end() ? nullptr : &field->value;
	}

	void SetSceneValue(GraphicsSettingsSnapshot& snapshot, GraphicsSettingId id, GraphicsValue value)
	{
		const auto field = std::find_if(snapshot.values.begin(), snapshot.values.end(), [id](const auto& item) { return item.id == id; });
		if(field == snapshot.values.end()) snapshot.values.push_back({id, std::move(value)});
		else field->value = std::move(value);
	}

	std::vector<GraphicsSettingId> ProjectedOverrides(const SceneState& scene, std::uint64_t requestId)
	{
		auto overrides = scene.colorOverrides;
		const auto request = std::find_if(scene.requestFields.begin(), scene.requestFields.end(), [requestId](const auto& row) { return row.first == requestId; });
		if(request != scene.requestFields.end())
			for(const auto id : request->second)
				if(SceneColorField(id) && std::find(overrides.begin(), overrides.end(), id) == overrides.end()) overrides.push_back(id);
		return overrides;
	}

	std::shared_ptr<const GraphicsSettingsSnapshot> MergeScene(SceneState& scene, const GraphicsSettingsSnapshot& game,
		const GraphicsSettingsSnapshot& local, std::span<const GraphicsSettingId> overrides)
	{
		const auto certificate = local.postProcessResourceGeneration == game.lifecycle.backendResourceGeneration
			? local.postProcessResourceGeneration : game.postProcessResourceGeneration;
		const auto publishedFrameId = std::max(game.publishedFrameId, local.publishedFrameId);
		// Immutable owner revisions identify values. Reusing their projection
		// avoids allocating copied settings every unchanged, disabled Scene frame.
		const auto cached = std::find_if(scene.projections.begin(), scene.projections.end(), [&](const auto& entry) {
			return entry.gameRevision == game.revision && entry.sceneRevision == local.revision
				&& entry.snapshot->lifecycle == game.lifecycle && entry.snapshot->postProcessResourceGeneration == certificate
				&& entry.snapshot->publishedFrameId == publishedFrameId
				&& entry.snapshot->outputWidth == scene.extent.x && entry.snapshot->outputHeight == scene.extent.y;
		});
		if(cached != scene.projections.end()) return cached->snapshot;
		// Inherit complete Game color. The viewport owns AA and scaling, so a
		// Game request cannot change the Scene attachment extent or history.
		auto merged = game;
		const auto setLocal = [&](GraphicsSettingId id, GraphicsValue fallback) {
			const auto* value = FindSceneValue(local, id);
			SetSceneValue(merged, id, value ? *value : std::move(fallback));
		};
		setLocal(GraphicsSettingId::AntiAliasingMode, std::uint32_t{0});
		setLocal(GraphicsSettingId::GeometryRasterMode, std::uint32_t{0});
		setLocal(GraphicsSettingId::UpscalingMode, std::uint32_t{0});
		setLocal(GraphicsSettingId::InternalResolutionScale, 1.0f);
		setLocal(GraphicsSettingId::UpscalingSharpness, 0.0f);
		for(const auto id : overrides)
			if(const auto* value = FindSceneValue(local, id)) SetSceneValue(merged, id, *value);
		merged.target = {};
		GraphicsAAConfiguration aa;
		ResolveGraphicsAAConfiguration(merged.values, aa);
		merged.outputWidth = scene.extent.x;
		merged.outputHeight = scene.extent.y;
		merged.internalWidth = ResolveGraphicsAAInternalDimension(merged.outputWidth, aa);
		merged.internalHeight = ResolveGraphicsAAInternalDimension(merged.outputHeight, aa);
		merged.sampleCount = local.sampleCount;
		// This is a settings certificate, not evidence that the Scene image was
		// submitted. The exact destination is still qualified by the native owner.
		merged.postProcessResourceGeneration = certificate;
		// A successful local request can publish after the Game profile. Its
		// receipt must correspond to the Scene state even before Game changes.
		merged.publishedFrameId = publishedFrameId;
		merged.revision = scene.nextMergedRevision++;
		auto snapshot = std::make_shared<const GraphicsSettingsSnapshot>(std::move(merged));
		if(scene.projections.size() == 4u) scene.projections.erase(scene.projections.begin());
		scene.projections.push_back({game.revision, local.revision, snapshot});
		return snapshot;
	}

	GraphicsPreparationContext SceneContext(const SceneState& scene, std::uint64_t frameId)
	{
		return {scene.lifecycle, *scene.catalog, SceneTargets, frameId};
	}

	void DiscardSceneNativeCandidate(RenderGraphicsSettingsState& owner, RHIContext* backend)
	{
		// Scene and Game preparation are serialized under the owner mutex. Only
		// a caller that held a pending Scene rebuild uses this cleanup; the extra
		// guard protects unrelated Game candidates if that invariant is violated.
		if(owner.pendingRebuild || owner.preparation.valid()) return;
		if(auto* native = dynamic_cast<VulkanContext*>(backend)) native->DiscardPostProcessCandidates();
	}

	void UpdateSceneContext(RenderGraphicsSettingsState& owner, std::uint64_t frameId)
	{
		auto& scene = owner.scene;
		auto lifecycle = owner.lifecycle;
		lifecycle.playSessionGeneration = 0;
		if(scene.lifecycle != lifecycle)
		{
			const bool retiredOwner = scene.lifecycle.projectGeneration != lifecycle.projectGeneration
				|| scene.lifecycle.rendererSessionGeneration != lifecycle.rendererSessionGeneration;
			++scene.epoch;
			scene.pendingRebuild.reset();
			scene.completions.clear();
			scene.projections.clear();
			if(retiredOwner)
			{
				scene.colorOverrides.clear();
				if(scene.registered) scene.view.lifetimeGeneration = scene.nextViewLifetime++;
			}
			scene.lifecycle = lifecycle;
		}
		if(scene.sourceCatalog != owner.catalog || scene.catalogModeMask != scene.spatialModeMask)
		{
			auto catalog = std::make_shared<std::vector<GraphicsSettingDescriptor>>();
			for(auto descriptor : *owner.catalog)
			{
				if(!SceneField(descriptor.id)) continue;
				descriptor.gameScope = true;
				descriptor.cameraScope = false;
				if(descriptor.id == GraphicsSettingId::AntiAliasingMode)
				{
					const auto coverage = GraphicsValue{static_cast<std::uint32_t>(GraphicsAntiAliasingMode::MSAA)};
					const bool hasCoverage = std::find(descriptor.options.begin(), descriptor.options.end(), coverage)
						!= descriptor.options.end();
					const auto temporal = GraphicsValue{static_cast<std::uint32_t>(GraphicsAntiAliasingMode::TAA)};
					const bool hasTemporal = std::find(descriptor.options.begin(), descriptor.options.end(), temporal)
						!= descriptor.options.end();
					descriptor.defaultValue = std::uint32_t{0};
					descriptor.options = {std::uint32_t{0}};
					for(std::uint32_t mode = 1; mode <= 3; ++mode)
						if((scene.spatialModeMask & (1u << mode)) != 0) descriptor.options.emplace_back(mode);
					if(hasCoverage) descriptor.options.push_back(coverage);
					if(hasTemporal) descriptor.options.push_back(temporal);
				}
				catalog->push_back(std::move(descriptor));
			}
			scene.catalog = std::move(catalog);
			scene.sourceCatalog = owner.catalog;
			scene.catalogModeMask = scene.spatialModeMask;
		}
		scene.controller.Invalidate(SceneContext(scene, frameId));
		scene.controller.UpdateViewExtent({}, scene.extent.x, scene.extent.y);
	}

	void RegisterSceneOutput(RenderGraphicsSettingsState& owner, glm::uvec2 extent, std::uint32_t spatialModeMask,
		std::uint64_t frameId)
	{
		auto& scene = owner.scene;
		const bool registered = extent.x != 0 && extent.y != 0;
		if(scene.extent != extent || scene.registered != registered)
		{
			// Exact view tokens expire on resize or genuine closure. Value ownership
			// and terminal receipts survive; accepted image ownership stays native.
			scene.controller.CancelPending(GraphicsRequestReason::StaleLifetime);
			++scene.epoch;
			scene.pendingRebuild.reset();
			scene.completions.clear();
			scene.projections.clear();
			scene.view = {RenderViewType::Scene, 1u, registered ? scene.nextViewLifetime++ : 0u, RenderOutputKind::SceneView};
		}
		scene.extent = extent;
		scene.registered = registered;
		scene.spatialModeMask = (spatialModeMask & 0xFu) | 1u;
		UpdateSceneContext(owner, frameId);
	}
}

void RenderSystem::RegisterSceneViewGraphicsOutput(glm::uvec2 extent, std::uint32_t spatialModeMask)
{
	std::lock_guard lock(graphicsSettingsState->mutex);
	auto& scene = graphicsSettingsState->scene;
	// The registered extent and owner's context are checked in protected
	// capture. An unchanged native output needs no second controller walk.
	if(scene.extent == extent && scene.spatialModeMask == ((spatialModeMask & 0xFu) | 1u)) return;
	const bool hadPending = scene.pendingRebuild.has_value();
	const auto epoch = scene.epoch;
	try
	{
		RegisterSceneOutput(*graphicsSettingsState, extent, spatialModeMask, nextRenderFrameId);
	}
	catch(...)
	{
		// Catalog allocation can fail after retiring a resized view. Its native
		// candidate still needs settlement even when no replacement was admitted.
		if(hadPending && epoch != graphicsSettingsState->scene.epoch) DiscardSceneNativeCandidate(*graphicsSettingsState, rhi.get());
		throw;
	}
	if(hadPending && epoch != graphicsSettingsState->scene.epoch) DiscardSceneNativeCandidate(*graphicsSettingsState, rhi.get());
}

std::optional<RenderSystem::SceneViewGraphicsSettingsState> RenderSystem::ReadSceneViewGraphicsSettings() const
{
	std::lock_guard lock(graphicsSettingsState->mutex);
	auto& owner = *graphicsSettingsState;
	auto& scene = owner.scene;
	if(!scene.registered) return std::nullopt;
	SceneViewGraphicsSettingsState result;
	result.lifecycle = scene.lifecycle;
	result.view = scene.view;
	const auto local = scene.controller.Snapshot();
	result.revision = local->revision;
	result.spatialModeMask = scene.spatialModeMask;
	result.effective = MergeScene(scene, *owner.controller.Snapshot(), *local, scene.colorOverrides);
	result.requested = scene.pendingRebuild && scene.preparationGame
		? MergeScene(scene, *scene.preparationGame, scene.pendingRebuild->snapshot, ProjectedOverrides(scene, scene.pendingRebuild->requestId))
		: result.effective;
	return result;
}

GraphicsRequestReceipt RenderSystem::QueueSceneViewGraphicsSettings(const GraphicsLifecycleStamp& lifecycle,
	const GraphicsViewIdentity& view, GraphicsRevision baseRevision, std::span<const GraphicsSettingValue> values)
{
	std::lock_guard lock(graphicsSettingsState->mutex);
	auto& scene = graphicsSettingsState->scene;
	GraphicsSettingsRequest request;
	request.lifecycle = lifecycle;
	request.baseRevision = baseRevision;
	request.patches.assign(values.begin(), values.end());
	if(!scene.registered) return scene.controller.Reject(std::move(request), GraphicsRequestReason::TargetUnavailable);
	if(lifecycle != scene.lifecycle || view != scene.view) return scene.controller.Reject(std::move(request), GraphicsRequestReason::StaleLifetime);
	if(std::any_of(values.begin(), values.end(), [](const auto& value) { return !SceneField(value.id); }))
		return scene.controller.Reject(std::move(request), GraphicsRequestReason::InvalidScope);
	std::vector<GraphicsSettingId> fields;
	for(const auto& value : values) fields.push_back(value.id);
	// Allocate ingress bookkeeping before admission. A late allocation failure
	// must not leave an accepted color override without its authored-field ledger.
	scene.requestFields.reserve(scene.requestFields.size() + 1u);
	const auto receipt = scene.controller.Enqueue(std::move(request));
	scene.requestFields.emplace_back(receipt.requestId, std::move(fields));
	return receipt;
}

std::optional<GraphicsRequestReceipt> RenderSystem::ReadSceneViewGraphicsSettingsReceipt(GraphicsRequestId requestId) const
{
	std::lock_guard lock(graphicsSettingsState->mutex);
	return graphicsSettingsState->scene.controller.GetReceipt(requestId);
}

bool RenderSystem::HasSceneViewGraphicsSettingsPreparation() const
{
	std::lock_guard lock(graphicsSettingsState->mutex);
	return graphicsSettingsState->scene.HasPreparation();
}

std::shared_ptr<const GraphicsSettingsSnapshot> RenderSystem::GetMergedSceneViewGraphicsSettingsSnapshot(
	std::uint64_t* sceneRevision, std::optional<GraphicsViewIdentity>* view, glm::uvec2 plannedExtent) const
{
	std::lock_guard lock(graphicsSettingsState->mutex);
	auto& owner = *graphicsSettingsState;
	const auto game = owner.controller.Snapshot();
	const auto scene = owner.scene.controller.Snapshot();
	if(sceneRevision) *sceneRevision = owner.scene.registered ? scene->revision : 0u;
	if(view) *view = owner.scene.registered ? std::optional(owner.scene.view) : std::nullopt;
	auto merged = MergeScene(owner.scene, *game, *scene, owner.scene.colorOverrides);
	if(!plannedExtent.x || !plannedExtent.y || plannedExtent == owner.scene.extent) return merged;
	// A resize prepares a replacement before the new image can be accepted.
	// Borrow the prospective lifetime internally. RegisterSceneOutput
	// publishes it to API callers only after the native result is accepted.
	if(view) *view = GraphicsViewIdentity{RenderViewType::Scene, 1u, owner.scene.nextViewLifetime, RenderOutputKind::SceneView};
	auto planned = std::make_shared<GraphicsSettingsSnapshot>(*merged);
	GraphicsAAConfiguration aa;
	ResolveGraphicsAAConfiguration(planned->values, aa);
	planned->outputWidth = plannedExtent.x;
	planned->outputHeight = plannedExtent.y;
	planned->internalWidth = ResolveGraphicsAAInternalDimension(planned->outputWidth, aa);
	planned->internalHeight = ResolveGraphicsAAInternalDimension(planned->outputHeight, aa);
	return planned;
}

GraphicsSettingsSnapshot RenderSystem::ProjectSceneViewGraphicsSettings(const GraphicsSettingsSnapshot& game) const
{
	std::lock_guard lock(graphicsSettingsState->mutex);
	auto& scene = graphicsSettingsState->scene;
	return *MergeScene(scene, game, *scene.controller.Snapshot(), scene.colorOverrides);
}

std::optional<GraphicsPreparedCandidate> RenderSystem::GetPendingSceneViewGraphicsSettingsResourcePreparation() const
{
	std::lock_guard lock(graphicsSettingsState->mutex);
	auto& scene = graphicsSettingsState->scene;
	if(!scene.registered || !scene.pendingRebuild || !scene.preparationGame) return std::nullopt;
	auto candidate = *scene.pendingRebuild;
	candidate.snapshot = *MergeScene(scene, *scene.preparationGame, candidate.snapshot, ProjectedOverrides(scene, candidate.requestId));
	return candidate;
}

void RenderSystem::CompleteSceneViewGraphicsSettingsResourcePreparation(GraphicsRequestId requestId,
	GraphicsLifecycleStamp lifecycle, bool succeeded, bool postProcessPrepared, GraphicsRequestReason failureReason)
{
	std::lock_guard lock(graphicsSettingsState->mutex);
	auto& scene = graphicsSettingsState->scene;
	if(scene.pendingRebuild && scene.pendingRebuild->requestId == requestId && scene.lifecycle == lifecycle)
		scene.completions.push_back({requestId, lifecycle, succeeded, postProcessPrepared, failureReason});
}

void RenderSystem::CaptureSceneViewGraphicsSettingsAtProtectedBoundary()
{
	auto state = graphicsSettingsState;
	bool heldNativeSceneCandidate = false;
	try
	{
		std::lock_guard lock(state->mutex);
		auto& scene = state->scene;
		heldNativeSceneCandidate = scene.pendingRebuild.has_value();
		const auto epochBeforeContext = scene.epoch;
		UpdateSceneContext(*state, nextRenderFrameId);
		if(heldNativeSceneCandidate && scene.epoch != epochBeforeContext) DiscardSceneNativeCandidate(*state, rhi.get());
		if(!scene.registered) return;
		const auto context = SceneContext(scene, nextRenderFrameId);
		if(scene.preparation.valid() && scene.preparation.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			auto candidate = scene.preparation.get();
			const auto receipt = candidate ? scene.controller.GetReceipt(candidate->requestId) : std::nullopt;
			if(candidate && receipt && receipt->state == GraphicsRequestState::PendingRebuild) scene.pendingRebuild = std::move(candidate);
		}
		for(const auto& completion : scene.completions)
		{
			if(!scene.pendingRebuild || scene.pendingRebuild->requestId != completion.requestId || completion.lifecycle != scene.lifecycle) continue;
			const auto candidateId = scene.pendingRebuild->requestId;
			auto proposedOverrides = ProjectedOverrides(scene, candidateId);
			bool ready = completion.succeeded;
			const auto game = state->controller.Snapshot();
			// Native preparation covered the complete inherited Game state. A
			// competing publication cannot silently change that candidate underneath it.
			ready = ready && scene.preparationGame && game->revision == scene.preparationGame->revision
				&& game->lifecycle == scene.preparationGame->lifecycle && game->values == scene.preparationGame->values;
			const auto receipt = scene.controller.Publish(std::move(*scene.pendingRebuild), context, ready,
				completion.postProcessPrepared ? context.lifecycle.backendResourceGeneration : 0u, completion.failureReason);
			scene.pendingRebuild.reset();
			if(receipt.state == GraphicsRequestState::Applied) scene.colorOverrides = std::move(proposedOverrides);
			else DiscardSceneNativeCandidate(*state, rhi.get());
			heldNativeSceneCandidate = false;
		}
		scene.completions.clear();
		std::erase_if(scene.requestFields, [&](const auto& row) {
			const auto receipt = scene.controller.GetReceipt(row.first);
			return !receipt || receipt->state == GraphicsRequestState::Rejected || receipt->state == GraphicsRequestState::Cancelled
				|| receipt->state == GraphicsRequestState::Applied;
		});
		if(scene.HasPreparation() || state->preparation.valid() || state->pendingRebuild || !scene.controller.HasPendingWork()) return;
		const auto epoch = scene.epoch;
		const auto catalog = scene.catalog;
		const auto lifecycle = scene.lifecycle;
		const auto frameId = nextRenderFrameId;
		scene.preparationGame = state->controller.Snapshot();
		auto task = ThreadPool::instance().createTask([state, epoch, catalog, lifecycle, frameId]() -> std::optional<GraphicsPreparedCandidate> {
			std::lock_guard workerLock(state->mutex);
			if(epoch != state->scene.epoch || !state->scene.registered) return std::nullopt;
			return state->scene.controller.Prepare({lifecycle, *catalog, SceneTargets, frameId});
		});
		auto future = task->getFuture();
		ThreadPool::instance().enqueue(task);
		scene.preparation = std::move(future);
	}
	catch(...)
	{
		// Infrastructure failures leave immutable effective settings untouched.
		// No partially prepared native candidate becomes an effective publication.
		std::lock_guard lock(state->mutex);
		if(heldNativeSceneCandidate || state->scene.pendingRebuild) DiscardSceneNativeCandidate(*state, rhi.get());
		state->scene.controller.CancelPending(GraphicsRequestReason::PreparationFailed);
		state->scene.pendingRebuild.reset();
		state->scene.completions.clear();
	}
}
