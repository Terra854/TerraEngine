// GraphicsSettingsController.cpp - transactional validation without live reads.
#include "Renderer/GraphicsSettings/GraphicsSettingsController.h"
#include "Renderer/GraphicsSettings/PostProcessColorSettings.h"
#include "Renderer/GraphicsSettings/PostProcessEffectSettings.h"
#include "Renderer/GraphicsSettings/GraphicsSettingsCatalog.h"
#include "Renderer/RenderTypes.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

namespace TerraEngine::Renderer::GraphicsSettings
{
	namespace
	{
		bool SameOwnerLifetime(const GraphicsLifecycleStamp& first, const GraphicsLifecycleStamp& second)
		{
			// Resource replacement invalidates prepared backend work, while settings
			// remain owned by the same project, Play Mode, and renderer session.
			return first.projectGeneration == second.projectGeneration
				&& first.playSessionGeneration == second.playSessionGeneration
				&& first.rendererSessionGeneration == second.rendererSessionGeneration;
		}

		bool HasSetting(std::span<const GraphicsSettingId> settings, GraphicsSettingId id)
		{
			return std::find(settings.begin(), settings.end(), id) != settings.end();
		}

		const GraphicsSettingDescriptor* FindDescriptor(const GraphicsPreparationContext& context, GraphicsSettingId id)
		{
			const GraphicsSettingDescriptor* result = nullptr;
			for(const auto& descriptor : context.descriptors)
			{
				if(descriptor.id != id) continue;
				// A duplicated catalog ID is ambiguous, even when both rows happen
				// to have the same default. Reject instead of selecting by ordering.
				if(result != nullptr) return nullptr;
				result = &descriptor;
			}
			return result;
		}

		bool MatchesKind(const GraphicsValue& value, GraphicsValueKind kind)
		{
			return value.index() == static_cast<std::size_t>(kind);
		}

		bool InRange(const GraphicsValue& value, const std::optional<GraphicsValue>& bound, bool minimum)
		{
			if(!bound) return true;
			if(value.index() != bound->index()) return false;
			return std::visit([&](const auto& item) {
				using Value = std::decay_t<decltype(item)>;
				if constexpr(std::is_arithmetic_v<Value>)
				{
					const Value other = std::get<Value>(*bound);
					return minimum ? item >= other : item <= other;
				}
				else return false;
			}, value);
		}

		bool ValidValue(const GraphicsSettingDescriptor& descriptor, const GraphicsValue& value)
		{
			if(!MatchesKind(value, descriptor.valueKind)) return false;
			if(const auto scalar = std::get_if<float>(&value); scalar != nullptr && !std::isfinite(*scalar)) return false;
			if(!InRange(value, descriptor.minimum, true) || !InRange(value, descriptor.maximum, false)) return false;
			return descriptor.options.empty()
				|| std::find(descriptor.options.begin(), descriptor.options.end(), value) != descriptor.options.end();
		}

		bool SupportsScope(const GraphicsSettingDescriptor& descriptor, GraphicsScope scope)
		{
			return scope == GraphicsScope::Game ? descriptor.gameScope : descriptor.cameraScope;
		}

		GraphicsRequestReason ResolveTarget(const GraphicsTarget& requested,
			const GraphicsPreparationContext& context, GraphicsTarget& resolved)
		{
			if(requested.scope == GraphicsScope::Game)
			{
				if(requested.camera || requested.view) return GraphicsRequestReason::InvalidScope;
				resolved = requested;
				return GraphicsRequestReason::None;
			}
			if(requested.scope != GraphicsScope::Camera || !requested.camera
				|| requested.camera->cameraId == TerraEngine::EMPTY_UUID) return GraphicsRequestReason::InvalidScope;
			const GraphicsTarget* match = nullptr;
			for(const auto& target : context.validTargets)
			{
				if(target.scope != GraphicsScope::Camera || target.camera != requested.camera) continue;
				if(requested.view && target.view != requested.view) continue;
				match = &target;
			}
			if(match == nullptr) return GraphicsRequestReason::TargetUnavailable;
			// A mutation belongs to the camera even when several views render it.
			// Per-view identities select effective query metadata, not separate
			// overrides that could drift apart for the same camera component.
			resolved = requested;
			resolved.view.reset();
			return GraphicsRequestReason::None;
		}

		const GraphicsValue* FindValue(std::span<const GraphicsSettingValue> values, GraphicsSettingId id)
		{
			const auto found = std::find_if(values.begin(), values.end(), [id](const auto& value) { return value.id == id; });
			return found == values.end() ? nullptr : &found->value;
		}

		void SetValue(std::vector<GraphicsSettingValue>& values, GraphicsSettingId id, const GraphicsValue& value)
		{
			const auto found = std::find_if(values.begin(), values.end(), [id](const auto& field) { return field.id == id; });
			if(found == values.end()) values.push_back({id, value});
			else found->value = value;
		}
	}

	struct GraphicsSettingsController::Impl
	{
		struct Publication
		{
			std::shared_ptr<const GraphicsSettingsSnapshot> snapshot;
			std::vector<GraphicsSettingId> overrides;
			std::vector<std::shared_ptr<const GraphicsSettingsSnapshot>> views;
		};
		struct Work
		{
			GraphicsSettingsRequest request;
			GraphicsRequestId id = 0;
		};
		struct Prepared
		{
			GraphicsPreparedCandidate candidate;
			std::vector<GraphicsSettingId> overrides;
			GraphicsTarget validationTarget;
		};

		// Serialize copied validation and pointer publication with queue ownership.
		// No live scene access, resource creation, job wait, or I/O occurs here.
		mutable std::mutex mutex;
		std::deque<Work> queue;
		std::unordered_map<GraphicsRequestId, GraphicsRequestReceipt> receipts;
		std::optional<Prepared> prepared;
		Publication game{std::make_shared<GraphicsSettingsSnapshot>(), {}};
		std::vector<Publication> cameras;
		GraphicsRequestId nextId = 1;
		std::uint64_t nextSequence = 1;

		Publication* FindPublication(const GraphicsTarget& target)
		{
			if(target.scope == GraphicsScope::Game) return &game;
			const auto found = std::find_if(cameras.begin(), cameras.end(), [&](const auto& entry) { return entry.snapshot->target == target; });
			return found == cameras.end() ? nullptr : &*found;
		}

		void RefreshCameraViews(Publication& camera, const GraphicsPreparationContext& context,
			const GraphicsSettingsController* owner = nullptr)
		{
			std::size_t targetCount = 0;
			bool unchanged = true;
			for(const auto& target : context.validTargets)
			{
				if(target.scope != GraphicsScope::Camera || target.camera != camera.snapshot->target.camera) continue;
				++targetCount;
				unchanged = unchanged && std::any_of(camera.views.begin(), camera.views.end(), [&](const auto& view) {
					return view->target == target && view->revision == camera.snapshot->revision
						&& view->publishedFrameId == camera.snapshot->publishedFrameId;
				});
			}
			// Most protected frames only confirm the same live view set. Avoid
			// allocating projection containers when that publication is unchanged.
			if(unchanged && targetCount == camera.views.size()) return;
			std::vector<std::shared_ptr<const GraphicsSettingsSnapshot>> views;
			for(const auto& target : context.validTargets)
			{
				if(target.scope != GraphicsScope::Camera || target.camera != camera.snapshot->target.camera) continue;
				if(std::any_of(views.begin(), views.end(), [&](const auto& view) { return view->target == target; })) continue;
				// Reuse projections of the same immutable publication when only the
				// backend resources changed. Retired view lifetimes never match.
				const auto existing = std::find_if(camera.views.begin(), camera.views.end(), [&](const auto& view) {
					return view->target == target && view->revision == camera.snapshot->revision
						&& view->publishedFrameId == camera.snapshot->publishedFrameId;
				});
				if(existing != camera.views.end()) views.push_back(*existing);
				else if(target == camera.snapshot->target) views.push_back(camera.snapshot);
				else
				{
					if(owner) owner->AllocationCheckpoint("camera-view-projection");
					auto projected = std::make_shared<GraphicsSettingsSnapshot>(*camera.snapshot);
					projected->target = target;
					views.push_back(std::move(projected));
				}
			}
			camera.views = std::move(views);
		}

		GraphicsRequestReceipt& Fail(GraphicsRequestId id, GraphicsRequestReason reason,
			std::string detail, GraphicsRequestState state = GraphicsRequestState::Rejected)
		{
			auto& receipt = receipts.at(id);
			receipt.state = state;
			receipt.reason = reason;
			receipt.detail = std::move(detail);
			return receipt;
		}

		void FailPreparation(GraphicsRequestId id) noexcept
		{
			// Memory pressure must not require another allocation to make a request
			// terminal. The typed reason remains queryable even with no detail text.
			const auto found = receipts.find(id);
			if(found == receipts.end()) return;
			found->second.state = GraphicsRequestState::Rejected;
			found->second.reason = GraphicsRequestReason::PreparationFailed;
			found->second.detail.clear();
		}
	};

	GraphicsSettingsController::GraphicsSettingsController() : impl(std::make_unique<Impl>()) {}
	GraphicsSettingsController::~GraphicsSettingsController() = default;

	GraphicsRequestReceipt GraphicsSettingsController::Reject(GraphicsSettingsRequest request, GraphicsRequestReason reason)
	{
		std::lock_guard lock(impl->mutex);
		GraphicsRequestReceipt receipt;
		receipt.requestId = impl->nextId++;
		receipt.sequence = impl->nextSequence++;
		receipt.state = GraphicsRequestState::Rejected;
		receipt.reason = reason;
		receipt.requestedRevision = request.baseRevision;
		for(const auto& patch : request.patches) receipt.affectedSettings.push_back(patch.id);
		receipt.affectedSettings.insert(receipt.affectedSettings.end(), request.resetSettings.begin(), request.resetSettings.end());
		impl->receipts.emplace(receipt.requestId, receipt);
		return receipt;
	}

	GraphicsRequestReceipt GraphicsSettingsController::Enqueue(GraphicsSettingsRequest request)
	{
		std::lock_guard lock(impl->mutex);
		GraphicsRequestReceipt receipt;
		receipt.requestId = impl->nextId++;
		receipt.sequence = impl->nextSequence++;
		receipt.state = GraphicsRequestState::Accepted;
		receipt.requestedRevision = request.baseRevision;
		for(const auto& patch : request.patches) receipt.affectedSettings.push_back(patch.id);
		receipt.affectedSettings.insert(receipt.affectedSettings.end(), request.resetSettings.begin(), request.resetSettings.end());
		AllocationCheckpoint("enqueue-receipt");
		impl->receipts.emplace(receipt.requestId, receipt);
		try
		{
			AllocationCheckpoint("enqueue-queue");
			impl->queue.push_back({std::move(request), receipt.requestId});
		}
		catch(...)
		{
			// No accepted request is visible unless its queue owner was admitted.
			impl->receipts.erase(receipt.requestId);
			throw;
		}
		return receipt;
	}

	std::optional<GraphicsPreparedCandidate> GraphicsSettingsController::Prepare(const GraphicsPreparationContext& context)
	{
		std::lock_guard lock(impl->mutex);
		// Only one candidate can own the head of the ordered stream. Backend
		// preparation can run after this function releases the admission lock.
		if(impl->prepared) return std::nullopt;
		while(!impl->queue.empty())
		{
			const auto workId = impl->queue.front().id;
			try
			{
				static_assert(std::is_nothrow_move_constructible_v<Impl::Work>);
				auto work = std::move(impl->queue.front());
				impl->queue.pop_front();
				const auto& request = work.request;
				if(request.lifecycle != context.lifecycle)
				{
					impl->Fail(work.id, GraphicsRequestReason::StaleLifetime, "The request belongs to a different renderer or project lifetime.");
					continue;
				}
				GraphicsTarget target;
				const auto targetReason = ResolveTarget(request.target, context, target);
				if(targetReason != GraphicsRequestReason::None)
				{
					impl->Fail(work.id, targetReason, "The requested target is unavailable or ambiguous.");
					continue;
				}
				const auto* publication = impl->FindPublication(target);
				const GraphicsRevision currentRevision = publication ? publication->snapshot->revision : 0;
				if(request.baseRevision != 0 && request.baseRevision != currentRevision)
				{
					impl->Fail(work.id, GraphicsRequestReason::StaleRevision, "The requested settings revision has changed.");
					continue;
				}
				if(request.operation == GraphicsRequestOperation::StageNextLaunch)
				{
					impl->receipts.at(work.id).activation = GraphicsActivation::NextLaunch;
					impl->Fail(work.id, GraphicsRequestReason::RestartRequired, "Explicit durable startup staging requires the startup adapter.",
						GraphicsRequestState::RestartRequired);
					continue;
				}
				const bool patch = request.operation == GraphicsRequestOperation::Patch;
				const bool reset = request.operation == GraphicsRequestOperation::Reset;
				const bool applyPreset = request.operation == GraphicsRequestOperation::Preset;
				if((!patch && !reset && !applyPreset) || (request.preset.has_value() != applyPreset)
					|| (patch && (request.patches.empty() || !request.resetSettings.empty()))
					|| (reset && (request.resetSettings.empty() || !request.patches.empty()))
					|| (applyPreset && (!request.patches.empty() || !request.resetSettings.empty())))
				{
					impl->Fail(work.id, GraphicsRequestReason::InvalidValue, "The request operation and payload do not match.");
					continue;
				}
				std::span<const GraphicsSettingValue> fields = request.patches;
				if(applyPreset)
				{
					if(target.scope != GraphicsScope::Game)
					{
						impl->Fail(work.id, GraphicsRequestReason::InvalidScope, "Named graphics presets apply to the whole game.");
						continue;
					}
					const auto preset = std::find_if(context.presets.begin(), context.presets.end(), [&](const auto& entry) {
						return entry.preset == *request.preset;
					});
					if(*request.preset > QualityPreset::Ultra || preset == context.presets.end() || preset->values.empty())
					{
						impl->Fail(work.id, GraphicsRequestReason::InvalidValue, "The requested graphics preset is not available.");
						continue;
					}
					// Presets go through the same complete validation as individual
					// changes. A single unsupported member rejects the whole preset.
					fields = preset->values;
				}

				Impl::Prepared prepared;
				prepared.validationTarget = request.target;
				auto& candidate = prepared.candidate;
				candidate.requestId = work.id;
				candidate.sequence = impl->receipts.at(work.id).sequence;
				candidate.baseRevision = currentRevision;
				candidate.lifecycle = context.lifecycle;
				candidate.preparedResourceGeneration = context.lifecycle.backendResourceGeneration;
				if(publication)
				{
					AllocationCheckpoint("prepare-canonical-copy");
					candidate.snapshot = *publication->snapshot;
					prepared.overrides = publication->overrides;
				}
				candidate.snapshot.target = target;
				candidate.snapshot.lifecycle = context.lifecycle;
				candidate.snapshot.revision = currentRevision + 1;
				candidate.snapshot.publishedFrameId = 0;
				for(const auto& descriptor : context.descriptors)
				{
					if(!SupportsScope(descriptor, target.scope)) continue;
					const auto* inherited = target.scope == GraphicsScope::Camera ? FindValue(impl->game.snapshot->values, descriptor.id) : nullptr;
					if(!FindValue(candidate.snapshot.values, descriptor.id)) SetValue(candidate.snapshot.values, descriptor.id, inherited ? *inherited : descriptor.defaultValue);
				}
				GraphicsRequestReason reason = GraphicsRequestReason::None;
				std::string detail;
				std::vector<GraphicsSettingId> visited;
				const auto validateField = [&](GraphicsSettingId id, const GraphicsValue* value) {
					if(id == GraphicsSettingId::Invalid || HasSetting(visited, id))
					{
						reason = GraphicsRequestReason::InvalidSetting;
						detail = "The request contains an invalid or duplicate setting ID.";
						return;
					}
					visited.push_back(id);
					const auto* descriptor = FindDescriptor(context, id);
					if(!descriptor)
					{
						reason = GraphicsRequestReason::InvalidSetting;
						detail = "The setting is absent or ambiguous in the catalog.";
						return;
					}
					if(!SupportsScope(*descriptor, target.scope))
					{
						reason = GraphicsRequestReason::InvalidScope;
						detail = "The setting is not valid at this scope.";
						return;
					}
					if(!descriptor->available)
					{
						reason = descriptor->unavailableReason;
						detail = descriptor->detail;
						if(reason == GraphicsRequestReason::None) reason = GraphicsRequestReason::Unsupported;
						return;
					}
					if(descriptor->activation == GraphicsActivation::NextLaunch)
					{
						reason = GraphicsRequestReason::RestartRequired;
						detail = "The entire batch requires explicit next-launch staging.";
						return;
					}
					if(descriptor->activation == GraphicsActivation::ProfileOnly || descriptor->activation == GraphicsActivation::DevelopmentOnly)
					{
						reason = GraphicsRequestReason::Unsupported;
						detail = "The setting requires a profile or development adapter.";
						return;
					}
					const auto* inherited = reset && target.scope == GraphicsScope::Camera ? FindValue(impl->game.snapshot->values, id) : nullptr;
					const GraphicsValue& effective = value ? *value : (inherited ? *inherited : descriptor->defaultValue);
					if(IsGraphicsAASetting(id))
					{
						reason=ValidateGraphicsAASettingValue(*descriptor,effective);
						if(reason != GraphicsRequestReason::None)
						{ detail="The selected AA option is invalid or unavailable for this captured scope."; return; }
					}
					if(!ValidValue(*descriptor, effective))
					{
						reason = GraphicsRequestReason::InvalidValue;
						detail = "The value type, range, or option is invalid.";
						return;
					}
					SetValue(candidate.snapshot.values, id, effective);
					if(descriptor->activation == GraphicsActivation::Rebuild) candidate.activation = GraphicsActivation::Rebuild;
					if(patch && !HasSetting(prepared.overrides, id)) prepared.overrides.push_back(id);
					if(reset) std::erase(prepared.overrides, id);
				};
				for(const auto& field : fields)
				{
					validateField(field.id, &field.value);
					if(reason != GraphicsRequestReason::None) break;
				}
				if(reason == GraphicsRequestReason::None)
				{
					for(const auto id : request.resetSettings)
					{
						validateField(id, nullptr);
						if(reason != GraphicsRequestReason::None) break;
					}
				}
				if(reason == GraphicsRequestReason::None)
				{
					const auto color = ResolvePostProcessColorSettings(candidate.snapshot.values);
					if(color.minEV > color.maxEV)
					{
						reason = GraphicsRequestReason::InvalidValue;
						detail = "The automatic exposure minimum exceeds its maximum.";
					}
				}
				if(reason == GraphicsRequestReason::None)
				{
					reason = ValidatePostProcessEffectValues(candidate.snapshot.values);
					if(reason != GraphicsRequestReason::None)
						detail = "The requested camera or screen-space control tuple is invalid.";
				}
				if(reason == GraphicsRequestReason::None)
				{
					GraphicsAAConfiguration configuration;
					reason = ResolveGraphicsAAConfiguration(candidate.snapshot.values, configuration);
					if(reason != GraphicsRequestReason::None) detail = "The AA, upscaling and raster settings are incompatible. MSAA and temporal modes require VRS off.";
					else
					{
						// Samples and dimensions are derived from the canonical value
						// set, never from metadata edited by a producer or worker caller.
						candidate.snapshot.sampleCount = configuration.sampleCount;
						candidate.snapshot.internalWidth = ResolveGraphicsAAInternalDimension(candidate.snapshot.outputWidth, configuration);
						candidate.snapshot.internalHeight = ResolveGraphicsAAInternalDimension(candidate.snapshot.outputHeight, configuration);
					}
				}
				if(reason != GraphicsRequestReason::None)
				{
					if(reason == GraphicsRequestReason::RestartRequired) impl->receipts.at(work.id).activation = GraphicsActivation::NextLaunch;
					impl->Fail(work.id, reason, std::move(detail), reason == GraphicsRequestReason::RestartRequired ? GraphicsRequestState::RestartRequired : GraphicsRequestState::Rejected);
					continue;
				}
				if(applyPreset)
				{
					candidate.snapshot.preset = request.preset;
				}
				else if(target.scope == GraphicsScope::Game && candidate.snapshot.preset)
				{
					const auto previousPreset = std::find_if(context.presets.begin(), context.presets.end(), [&](const auto& entry) {
						return entry.preset == *candidate.snapshot.preset;
					});
					if(previousPreset != context.presets.end()
						&& std::any_of(previousPreset->values.begin(), previousPreset->values.end(), [&](const auto& setting) {
							const auto* effective = FindValue(candidate.snapshot.values, setting.id);
							return effective == nullptr || *effective != setting.value;
						}))
						candidate.snapshot.preset = QualityPreset::Custom;
				}
				// Copy the caller result before assigning the sole prepared owner. A
				// throwing return copy can otherwise strand the ordered stream forever.
				AllocationCheckpoint("prepare-result-copy");
				std::optional<GraphicsPreparedCandidate> result(candidate);
				static_assert(std::is_nothrow_move_constructible_v<Impl::Prepared>);
				static_assert(std::is_nothrow_move_constructible_v<decltype(result)>);
				auto& receipt = impl->receipts.at(work.id);
				if(applyPreset) receipt.affectedSettings = std::move(visited);
				receipt.activation = candidate.activation;
				receipt.state = candidate.activation == GraphicsActivation::Rebuild ? GraphicsRequestState::PendingRebuild : GraphicsRequestState::Accepted;
				receipt.requestedRevision = candidate.snapshot.revision;
				impl->prepared.emplace(std::move(prepared));
				return result;
			}
			catch(...)
			{
				impl->prepared.reset();
				impl->FailPreparation(workId);
			}
		}
		return std::nullopt;
	}

	GraphicsRequestReceipt GraphicsSettingsController::Publish(GraphicsPreparedCandidate candidate,
		const GraphicsPreparationContext& context, bool resourcesReady, std::uint64_t postProcessResourceGeneration,
		GraphicsRequestReason resourceFailureReason)
	{
		std::lock_guard lock(impl->mutex);
		const auto found = impl->receipts.find(candidate.requestId);
		if(found == impl->receipts.end())
		{
			GraphicsRequestReceipt receipt;
			receipt.requestId = candidate.requestId;
			receipt.state = GraphicsRequestState::Rejected;
			receipt.reason = GraphicsRequestReason::ConflictingRequest;
			return receipt;
		}
		if(!impl->prepared || impl->prepared->candidate.requestId != candidate.requestId) return found->second;
		auto prepared = std::move(*impl->prepared);
		impl->prepared.reset();
		try
		{
			const auto& canonical = prepared.candidate;
			if(candidate.sequence != canonical.sequence || candidate.baseRevision != canonical.baseRevision
				|| candidate.snapshot.target != canonical.snapshot.target || candidate.snapshot.values != canonical.snapshot.values)
				return impl->Fail(candidate.requestId, GraphicsRequestReason::ConflictingRequest, "The prepared candidate identity or values were changed.");
			GraphicsTarget target;
			if(canonical.lifecycle != context.lifecycle || candidate.lifecycle != canonical.lifecycle)
				return impl->Fail(candidate.requestId, GraphicsRequestReason::StaleLifetime, "The prepared renderer or backend lifetime has expired.");
			const auto targetReason = ResolveTarget(prepared.validationTarget, context, target);
			if(targetReason != GraphicsRequestReason::None) return impl->Fail(candidate.requestId, targetReason, "The prepared target has expired.");
			auto* publication = impl->FindPublication(target);
			if((publication ? publication->snapshot->revision : 0) != canonical.baseRevision)
				return impl->Fail(candidate.requestId, GraphicsRequestReason::StaleRevision, "The candidate baseline was replaced before commit.");
			if(!resourcesReady)
			{
				// The protected backend can distinguish a known unsupported view from
				// a failed allocation/pipeline. Other reasons cannot be smuggled through
				// this resource-only completion path or accidentally report success.
				const auto failure = resourceFailureReason == GraphicsRequestReason::Unsupported
					? GraphicsRequestReason::Unsupported : GraphicsRequestReason::PreparationFailed;
				return impl->Fail(candidate.requestId, failure, "Required backend resources could not be prepared.");
			}
			GraphicsAAConfiguration aaConfiguration;
			const auto aaReason = ResolveGraphicsAAConfiguration(canonical.snapshot.values, aaConfiguration);
			if(aaReason != GraphicsRequestReason::None)
				return impl->Fail(candidate.requestId, aaReason, "The canonical AA/raster tuple is invalid.");
			const bool processingChange=std::any_of(found->second.affectedSettings.begin(),found->second.affectedSettings.end(),
				[](const auto id){return id >= GraphicsSettingId::AntiAliasingMode;});
			const bool qualifiedRaster = aaConfiguration.antiAliasing != GraphicsAntiAliasingMode::Off
				|| aaConfiguration.upscaling != GraphicsUpscalingMode::Off;
			if(canonical.activation == GraphicsActivation::Rebuild && processingChange && qualifiedRaster
				&& postProcessResourceGeneration == 0)
				return impl->Fail(candidate.requestId,GraphicsRequestReason::PreparationFailed,"The selected AA variant has no trusted native preparation certificate.");
			if(postProcessResourceGeneration != 0
				&& (canonical.activation != GraphicsActivation::Rebuild
					|| postProcessResourceGeneration != context.lifecycle.backendResourceGeneration))
				return impl->Fail(candidate.requestId, GraphicsRequestReason::StaleLifetime, "The post-processing preparation does not cover this backend generation.");

			// Use the controller's owned candidate, so caller edits cannot change any
			// unvalidated values or publication metadata after worker preparation.
			AllocationCheckpoint("publish-canonical-copy");
			auto snapshot = std::make_shared<GraphicsSettingsSnapshot>(canonical.snapshot);
			snapshot->publishedFrameId = context.frameId;
			// A backend completion certifies a rebuild. The worker's candidate
			// and caller-editable copy cannot grant native resource readiness.
			const bool processingRebuilt = canonical.activation == GraphicsActivation::Rebuild
				&& std::any_of(found->second.affectedSettings.begin(), found->second.affectedSettings.end(), [](const auto id) {
					return id >= GraphicsSettingId::AntiAliasingMode;
				});
			if(postProcessResourceGeneration != 0 || processingRebuilt)
				snapshot->postProcessResourceGeneration = postProcessResourceGeneration;
			AllocationCheckpoint("publish-proposed-roots");
			auto proposedGame = impl->game;
			auto proposedCameras = impl->cameras;
			auto proposedPublication = target.scope == GraphicsScope::Game ? &proposedGame : nullptr;
			if(target.scope == GraphicsScope::Camera)
			{
				const auto camera = std::find_if(proposedCameras.begin(), proposedCameras.end(), [&](const auto& entry) {
					return entry.snapshot->target == target;
				});
				if(camera != proposedCameras.end()) proposedPublication = &*camera;
			}
			if(!proposedPublication)
			{
				proposedCameras.push_back({snapshot, std::move(prepared.overrides)});
				impl->RefreshCameraViews(proposedCameras.back(), context, this);
			}
			else
			{
				proposedPublication->snapshot = snapshot;
				proposedPublication->overrides = std::move(prepared.overrides);
				if(target.scope == GraphicsScope::Camera) impl->RefreshCameraViews(*proposedPublication, context, this);
			}
			if(target.scope == GraphicsScope::Game)
			{
				for(auto& camera : proposedCameras)
				{
					AllocationCheckpoint("publish-inherited-camera");
					auto inherited = std::make_shared<GraphicsSettingsSnapshot>(*camera.snapshot);
					for(auto& value : inherited->values)
					{
						if(HasSetting(camera.overrides, value.id)) continue;
						if(const auto* gameValue = FindValue(snapshot->values, value.id)) value.value = *gameValue;
					}
					const auto color = ResolvePostProcessColorSettings(inherited->values);
					if(color.minEV > color.maxEV)
						return impl->Fail(candidate.requestId, GraphicsRequestReason::InvalidValue,
							"The inherited game settings invert a live camera's exposure bounds.");
					if(const auto effects = ValidatePostProcessEffectValues(inherited->values); effects != GraphicsRequestReason::None)
						return impl->Fail(candidate.requestId, effects, "The game change conflicts with a live camera effect.");
					GraphicsAAConfiguration inheritedAA;
					const auto inheritedReason = ResolveGraphicsAAConfiguration(inherited->values, inheritedAA);
					if(inheritedReason != GraphicsRequestReason::None)
						return impl->Fail(candidate.requestId, inheritedReason, "The game change conflicts with a live camera's independent AA/raster tuple.");
					inherited->sampleCount = inheritedAA.sampleCount;
					inherited->internalWidth = ResolveGraphicsAAInternalDimension(inherited->outputWidth, inheritedAA);
					inherited->internalHeight = ResolveGraphicsAAInternalDimension(inherited->outputHeight, inheritedAA);
					if(postProcessResourceGeneration != 0 || std::none_of(camera.overrides.begin(), camera.overrides.end(), [](const auto id) {
						return id >= GraphicsSettingId::AntiAliasingMode;
					}))
					{
						// The game preparation covered its inherited exposure output.
						// An independent camera exposure keeps its own certificate.
						inherited->postProcessResourceGeneration = snapshot->postProcessResourceGeneration;
					}
					inherited->lifecycle = context.lifecycle;
					++inherited->revision;
					inherited->publishedFrameId = context.frameId;
					camera.snapshot = std::move(inherited);
					impl->RefreshCameraViews(camera, context, this);
				}
			}
			AllocationCheckpoint("publish-result-copy");
			auto appliedReceipt = found->second;
			appliedReceipt.state = GraphicsRequestState::Applied;
			appliedReceipt.reason = GraphicsRequestReason::None;
			appliedReceipt.effectiveRevision = snapshot->revision;
			appliedReceipt.appliedFrameId = context.frameId;
			auto result = appliedReceipt;
			static_assert(std::is_nothrow_swappable_v<Impl::Publication>);
			static_assert(std::is_nothrow_move_assignable_v<GraphicsRequestReceipt>);
			static_assert(std::is_nothrow_move_constructible_v<GraphicsRequestReceipt>);
			// All game roots, inherited camera values, projected views, and the caller
			// receipt exist before this noexcept commit. Readers see the old complete
			// state or this new complete state under the same admission lock.
			std::swap(impl->game, proposedGame);
			impl->cameras.swap(proposedCameras);
			found->second = std::move(appliedReceipt);
			return result;
		}
		catch(...)
		{
			impl->FailPreparation(candidate.requestId);
			return found->second;
		}
	}

	void GraphicsSettingsController::Invalidate(const GraphicsPreparationContext& context,
		std::span<const GraphicsCameraTarget> liveCameraOwners)
	{
		std::lock_guard lock(impl->mutex);
		std::erase_if(impl->queue, [&](const auto& work) {
			if(work.request.lifecycle != context.lifecycle)
			{
				impl->Fail(work.id, GraphicsRequestReason::StaleLifetime, "The queued owner lifetime was invalidated.", GraphicsRequestState::Cancelled);
				return true;
			}
			GraphicsTarget resolved;
			const auto reason = ResolveTarget(work.request.target, context, resolved);
			if(reason == GraphicsRequestReason::None) return false;
			impl->Fail(work.id, reason, "The queued camera target is no longer available.");
			return true;
		});
		if(impl->prepared)
		{
			GraphicsTarget target;
			const auto& candidate = impl->prepared->candidate;
			if(candidate.lifecycle != context.lifecycle || ResolveTarget(impl->prepared->validationTarget, context, target) != GraphicsRequestReason::None)
			{
				impl->Fail(candidate.requestId, GraphicsRequestReason::StaleLifetime, "The prepared target lifetime was invalidated.", GraphicsRequestState::Cancelled);
				impl->prepared.reset();
			}
		}
		std::erase_if(impl->cameras, [&](const auto& publication) {
			if(!SameOwnerLifetime(publication.snapshot->lifecycle,context.lifecycle)) return true;
			// ECS camera ownership and submitted-view membership are different
			// lifetimes. A missed view or target resize removes effective projections,
			// but must not erase the live camera's authored runtime override set.
			if(!liveCameraOwners.empty())
				return !publication.snapshot->target.camera || std::find(liveCameraOwners.begin(),liveCameraOwners.end(),
					*publication.snapshot->target.camera) == liveCameraOwners.end();
			// Existing context-only callers retain their previous decommissioning
			// semantics; a production empty owner set also has no valid camera views.
			GraphicsTarget target;
			return ResolveTarget(publication.snapshot->target,context,target) != GraphicsRequestReason::None;
		});
		for(auto& camera : impl->cameras) impl->RefreshCameraViews(camera, context);
		if(!SameOwnerLifetime(impl->game.snapshot->lifecycle, context.lifecycle))
		{
			// An owner lifetime reset retires the publication. Existing readers can
			// still hold the old const pointer, while the new owner starts empty.
			auto snapshot = std::make_shared<GraphicsSettingsSnapshot>();
			snapshot->lifecycle = context.lifecycle;
			impl->game = {std::move(snapshot), {}};
		}
		// A newly published camera can be read before it receives its first patch.
		// Each effective projection belongs to a real target supplied by the renderer.
		for(const auto& live : context.validTargets)
		{
			GraphicsTarget camera = live;
			camera.view.reset();
			if(camera.scope != GraphicsScope::Camera || !camera.camera || impl->FindPublication(camera)) continue;
			auto inherited = std::make_shared<GraphicsSettingsSnapshot>(*impl->game.snapshot);
			inherited->target = camera;
			inherited->lifecycle = context.lifecycle;
			inherited->publishedFrameId = context.frameId;
			std::erase_if(inherited->values, [&](const auto& value) {
				const auto* descriptor = FindDescriptor(context, value.id);
				return !descriptor || !descriptor->cameraScope;
			});
			Impl::Publication publication{std::move(inherited), {}};
			impl->RefreshCameraViews(publication, context);
			impl->cameras.push_back(std::move(publication));
		}
	}

	void GraphicsSettingsController::CancelPending(GraphicsRequestReason reason)
	{
		std::lock_guard lock(impl->mutex);
		for(const auto& work : impl->queue)
			impl->Fail(work.id, reason, {}, GraphicsRequestState::Cancelled);
		impl->queue.clear();
		if(impl->prepared)
		{
			impl->Fail(impl->prepared->candidate.requestId, reason, {}, GraphicsRequestState::Cancelled);
			impl->prepared.reset();
		}
	}

	void GraphicsSettingsController::RefreshNativeValues(const GraphicsPreparationContext& context,
		std::span<const GraphicsSettingValue> values, QualityPreset preset,
		std::uint64_t verifiedPostProcessGeneration)
	{
		std::lock_guard lock(impl->mutex);
	if(impl->game.snapshot->revision != 0 && impl->game.snapshot->preset == preset
		&& impl->game.snapshot->lifecycle == context.lifecycle
		&& (verifiedPostProcessGeneration == 0 || impl->game.snapshot->postProcessResourceGeneration == verifiedPostProcessGeneration)
			&& std::all_of(values.begin(), values.end(), [&](const auto& setting) {
				const auto* existing = FindValue(impl->game.snapshot->values, setting.id);
				return existing && *existing == setting.value;
			})) return;
		auto game = std::make_shared<GraphicsSettingsSnapshot>(*impl->game.snapshot);
		for(const auto& descriptor : context.descriptors)
			if(!FindValue(game->values, descriptor.id)) SetValue(game->values, descriptor.id, descriptor.defaultValue);
		for(const auto& value : values) SetValue(game->values, value.id, value.value);
		GraphicsAAConfiguration gameAA;
		if(ResolveGraphicsAAConfiguration(game->values, gameAA) != GraphicsRequestReason::None)
			throw std::invalid_argument("Native graphics observation contains an incompatible AA/raster tuple.");
		// Startup seeds and existing native setters use this same immutable
		// publication path, so their metadata must follow the selected tuple too.
		game->sampleCount = gameAA.sampleCount;
		game->internalWidth = ResolveGraphicsAAInternalDimension(game->outputWidth, gameAA);
		game->internalHeight = ResolveGraphicsAAInternalDimension(game->outputHeight, gameAA);
		game->lifecycle = context.lifecycle;
	// Startup may seed the first effective image settings only after preparing
	// their actual backend generation. Ordinary native observations retain it.
	if(verifiedPostProcessGeneration != 0 && verifiedPostProcessGeneration == context.lifecycle.backendResourceGeneration)
		game->postProcessResourceGeneration = verifiedPostProcessGeneration;
		game->preset = preset;
		++game->revision;
		game->publishedFrameId = context.frameId;
		auto cameras = impl->cameras;
		for(auto& camera : cameras)
		{
			auto inherited = std::make_shared<GraphicsSettingsSnapshot>(*camera.snapshot);
			for(const auto& descriptor : context.descriptors)
			{
				if(!descriptor.cameraScope || HasSetting(camera.overrides, descriptor.id)) continue;
				if(const auto* value = FindValue(game->values, descriptor.id)) SetValue(inherited->values, descriptor.id, *value);
			}
			GraphicsAAConfiguration inheritedAA;
			if(ResolveGraphicsAAConfiguration(inherited->values, inheritedAA) != GraphicsRequestReason::None)
				throw std::invalid_argument("Native graphics observation conflicts with an independent camera AA/raster tuple.");
			inherited->sampleCount = inheritedAA.sampleCount;
			inherited->internalWidth = ResolveGraphicsAAInternalDimension(inherited->outputWidth, inheritedAA);
			inherited->internalHeight = ResolveGraphicsAAInternalDimension(inherited->outputHeight, inheritedAA);
			inherited->lifecycle = context.lifecycle;
			++inherited->revision;
			inherited->publishedFrameId = context.frameId;
			camera.snapshot = std::move(inherited);
			impl->RefreshCameraViews(camera, context);
		}
		impl->game.snapshot = std::move(game);
		impl->cameras.swap(cameras);
	}

	void GraphicsSettingsController::UpdateViewExtent(const GraphicsTarget& target, std::uint32_t width, std::uint32_t height)
	{
		if(width == 0 || height == 0) return;
		std::lock_guard lock(impl->mutex);
		const auto update = [&](std::shared_ptr<const GraphicsSettingsSnapshot>& root) {
			GraphicsAAConfiguration configuration;
			if(ResolveGraphicsAAConfiguration(root->values, configuration) != GraphicsRequestReason::None) return;
			const auto internalWidth = ResolveGraphicsAAInternalDimension(width, configuration);
			const auto internalHeight = ResolveGraphicsAAInternalDimension(height, configuration);
			if(root->outputWidth == width && root->outputHeight == height && root->internalWidth == internalWidth
				&& root->internalHeight == internalHeight && root->sampleCount == configuration.sampleCount) return;
			// The owner supplies actual accepted output dimensions. The selected
			// qualified tuple determines its distinct internal dimensions/samples.
			auto snapshot = std::make_shared<GraphicsSettingsSnapshot>(*root);
			snapshot->outputWidth = width; snapshot->outputHeight = height;
			snapshot->internalWidth = internalWidth; snapshot->internalHeight = internalHeight;
			snapshot->sampleCount = configuration.sampleCount;
			root = std::move(snapshot);
		};
		if(target.scope == GraphicsScope::Game) update(impl->game.snapshot);
		else for(auto& camera : impl->cameras)
			for(auto& view : camera.views)
				if(view->target == target) update(view);
	}

	GraphicsSettingsSnapshot GraphicsSettingsController::ProjectCandidateForView(
		const GraphicsPreparedCandidate& candidate, const GraphicsTarget& target) const
	{
		std::lock_guard lock(impl->mutex);
		auto result = candidate.snapshot;
		if(target.scope != GraphicsScope::Camera || !target.camera) return result;
		// Device preparation must see the identical combination that would be
		// inherited at publication. Never substitute a game grade or exposure
		// for a camera which already owns an explicit value for that setting.
		if(result.target.scope == GraphicsScope::Game)
		{
			const auto camera = std::find_if(impl->cameras.begin(), impl->cameras.end(), [&](const auto& publication) {
				return publication.snapshot->target.camera == target.camera;
			});
			if(camera != impl->cameras.end())
				for(const auto id : camera->overrides)
					if(const auto* field = FindValue(camera->snapshot->values, id)) SetValue(result.values, id, *field);
		}
		// The native recorder validates the actual accepted view extent. The
		// canonical candidate may have originated from the game viewport.
		result.target = target;
		result.outputWidth = result.outputHeight = result.internalWidth = result.internalHeight = 0;
		GraphicsAAConfiguration configuration;
		if(ResolveGraphicsAAConfiguration(result.values, configuration) == GraphicsRequestReason::None) result.sampleCount = configuration.sampleCount;
		else result.postProcessResourceGeneration = 0;
		return result;
	}

	std::optional<GraphicsRequestReceipt> GraphicsSettingsController::GetReceipt(GraphicsRequestId requestId) const
	{
		std::lock_guard lock(impl->mutex);
		const auto found = impl->receipts.find(requestId);
		return found == impl->receipts.end() ? std::nullopt : std::optional(found->second);
	}

	std::shared_ptr<const GraphicsSettingsSnapshot> GraphicsSettingsController::Snapshot() const
	{
		std::lock_guard lock(impl->mutex);
		return impl->game.snapshot;
	}

	std::shared_ptr<const GraphicsSettingsSnapshot> GraphicsSettingsController::Snapshot(const GraphicsTarget& target) const
	{
		std::lock_guard lock(impl->mutex);
		if(target.scope == GraphicsScope::Game) return target.camera || target.view ? nullptr : impl->game.snapshot;
		if(target.scope != GraphicsScope::Camera || !target.camera) return nullptr;
		std::shared_ptr<const GraphicsSettingsSnapshot> result;
		for(const auto& camera : impl->cameras)
		{
			if(camera.snapshot->target.camera != target.camera) continue;
			for(const auto& view : camera.views)
			{
				if(target.view && view->target.view != target.view) continue;
				if(result) return nullptr;
				result = view;
			}
		}
		return result;
	}

	std::shared_ptr<const GraphicsSettingsSnapshot> GraphicsSettingsController::CameraOwnerSnapshot(const GraphicsCameraTarget& camera) const
	{
		std::lock_guard lock(impl->mutex);
		const auto found=std::find_if(impl->cameras.begin(),impl->cameras.end(),[&](const auto& publication) {
			return publication.snapshot->target.camera == camera;
		});
		// Return the canonical const owner, never synthesize an effective view.
		// A caller must separately validate current ECS and backend lifetime proof.
		return found == impl->cameras.end() ? nullptr : found->snapshot;
	}

	bool GraphicsSettingsController::HasPendingWork() const
	{
		std::lock_guard lock(impl->mutex);
		return !impl->queue.empty() || impl->prepared.has_value();
	}

	bool GraphicsSettingsController::CameraOverrides(const GraphicsCameraTarget& camera, GraphicsSettingId id) const
	{
		std::lock_guard lock(impl->mutex);
		return std::any_of(impl->cameras.begin(), impl->cameras.end(), [&](const auto& publication) {
			return publication.snapshot->target.camera == camera
				&& std::find(publication.overrides.begin(), publication.overrides.end(), id) != publication.overrides.end();
		});
	}
}
