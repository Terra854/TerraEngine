// Pure authored resolution. All writes stay inside one isolated candidate.
#include "Renderer/GraphicsSettings/PostProcessProfileResolver.h"
#include "Renderer/GraphicsSettings/GraphicsSettingsCatalog.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <type_traits>

namespace TerraEngine::Renderer::GraphicsSettings
{
	namespace
	{
		struct Rejection { GraphicsRequestReason reason; std::string detail; };
		[[noreturn]] void Reject(GraphicsRequestReason reason, const char* detail) { throw Rejection{reason, detail}; }

		bool Bound(const GraphicsValue& value, const std::optional<GraphicsValue>& bound, bool minimum)
		{
			if (!bound) return true;
			if (value.index() != bound->index()) return false;
			return std::visit([&](const auto& item) {
				using T = std::decay_t<decltype(item)>;
				if constexpr (std::is_arithmetic_v<T>) return minimum ? item >= std::get<T>(*bound) : item <= std::get<T>(*bound);
				else return false;
			}, value);
		}

		bool ValidValue(const GraphicsSettingDescriptor& descriptor, const GraphicsValue& value)
		{
			if (value.index() != static_cast<std::size_t>(descriptor.valueKind)) return false;
			if (const auto* scalar = std::get_if<float>(&value); scalar && !std::isfinite(*scalar)) return false;
			return Bound(value, descriptor.minimum, true) && Bound(value, descriptor.maximum, false)
				&& (descriptor.options.empty() || std::find(descriptor.options.begin(), descriptor.options.end(), value) != descriptor.options.end());
		}

		class Resolver
		{
		public:
			explicit Resolver(const PostProcessProfileResolutionInput& source) : input(source) {}

			PostProcessResolvedProfile Run()
			{
				const auto& target = input.target;
				if (target.scope != GraphicsScope::Camera || !target.camera || !target.view)
					Reject(GraphicsRequestReason::InvalidScope, "Profile resolution requires an exact camera and published view.");
				const auto& camera = *target.camera;
				if (camera.cameraId == TerraEngine::EMPTY_UUID || !camera.sceneGeneration || !camera.objectLifetimeGeneration
					|| !camera.componentGeneration || !camera.bindingGeneration || !target.view->lifetimeGeneration
					|| !input.lifecycle.projectGeneration || !input.lifecycle.rendererSessionGeneration
					|| !input.lifecycle.backendResourceGeneration || !input.baseRevision)
					Reject(GraphicsRequestReason::StaleLifetime, "A required camera, view, revision, or lifecycle generation is missing.");
				for (double coordinate : input.cameraPosition)
					if (!std::isfinite(coordinate)) Reject(GraphicsRequestReason::InvalidValue, "Camera position is not finite.");
				for (const auto& descriptor : input.descriptors)
				{
					if (descriptor.id == GraphicsSettingId::Invalid || !catalog.emplace(descriptor.id, &descriptor).second
						|| !ValidValue(descriptor, descriptor.defaultValue))
						Reject(GraphicsRequestReason::InvalidSetting, "The copied catalog has duplicate IDs or invalid defaults.");
					values.emplace(descriptor.id, descriptor.defaultValue);
				}
				if (catalog.empty()) Reject(GraphicsRequestReason::InvalidSetting, "The copied setting catalog is empty.");
				// Defaults/preset are inherited native state. Authored camera layers
				// below must additionally obey scope, activation and availability.
				Patch(input.engineDefaults, false);
				Patch(input.projectDefaults, false);
				Patch(input.qualityPreset, false);
				if (input.sceneProfile) Profile(*input.sceneProfile, 1.0f);
				Volumes();
				if (input.cameraProfile) Profile(*input.cameraProfile, 1.0f);
				Patch(input.cameraOverrides, true);
				Patch(input.runtimeCameraOverrides, true);
				Players();
				PostProcessResolvedProfile result;
				result.baseRevision = input.baseRevision;
				result.settings.target = target;
				result.settings.lifecycle = input.lifecycle;
				// This is a candidate based on an existing revision, never proof of
				// publication, frame execution, or prepared backend resources.
				result.settings.revision = input.baseRevision;
				for (auto& [id, value] : values) result.settings.values.push_back({id, std::move(value)});
				result.customEffects = std::move(effects);
				const auto* low = Find(result.settings.values, GraphicsSettingId::AutoExposureMinEV);
				const auto* high = Find(result.settings.values, GraphicsSettingId::AutoExposureMaxEV);
				if ((low && !std::holds_alternative<float>(*low)) || (high && !std::holds_alternative<float>(*high)))
					Reject(GraphicsRequestReason::InvalidSetting, "Exposure bounds require scalar catalog types.");
				if (low && high && std::get<float>(*low) > std::get<float>(*high))
					Reject(GraphicsRequestReason::InvalidValue, "The resolved exposure minimum exceeds its maximum.");
				GraphicsAAConfiguration aa;
				const auto reason = ResolveGraphicsAAConfiguration(result.settings.values, aa);
				if (reason != GraphicsRequestReason::None) Reject(reason, "The resolved AA, scale, raster, or upscaling tuple is incompatible.");
				result.settings.sampleCount = aa.sampleCount;
				return result;
			}

		private:
			const PostProcessProfileResolutionInput& input;
			std::map<GraphicsSettingId, const GraphicsSettingDescriptor*> catalog;
			std::map<GraphicsSettingId, GraphicsValue> values;
			std::vector<TerraEngine::UUID> effects;

			static const GraphicsValue* Find(const std::vector<GraphicsSettingValue>& fields, GraphicsSettingId id)
			{
				const auto found = std::find_if(fields.begin(), fields.end(), [id](const auto& item) { return item.id == id; });
				return found == fields.end() ? nullptr : &found->value;
			}

			const GraphicsSettingDescriptor& Validate(const GraphicsSettingValue& field, bool authored, bool checkAvailability = true)
			{
				const auto found = catalog.find(field.id);
				if (found == catalog.end()) Reject(GraphicsRequestReason::InvalidSetting, "An authored field is absent from the copied catalog.");
				const auto& descriptor = *found->second;
				if (authored && (!descriptor.cameraScope || descriptor.authoringScope != GraphicsAuthoringScope::None
					|| (descriptor.activation != GraphicsActivation::Live && descriptor.activation != GraphicsActivation::Rebuild)))
					Reject(GraphicsRequestReason::InvalidScope, "A profile field is not a live or rebuild camera setting.");
				if (authored && checkAvailability && IsGraphicsAASetting(field.id))
				{
					const auto reason = ValidateGraphicsAASettingValue(descriptor, field.value);
					if (reason != GraphicsRequestReason::None) Reject(reason, "The authored AA option is invalid or unavailable for the copied scope.");
				}
				if (!ValidValue(descriptor, field.value)) Reject(GraphicsRequestReason::InvalidValue, "An authored value has an invalid type, range, or option.");
				if (authored && checkAvailability && !descriptor.available && field.value != descriptor.defaultValue)
					Reject(GraphicsRequestReason::Unsupported, "An authored contribution requires an unavailable setting.");
				return descriptor;
			}

			void Patch(const std::vector<GraphicsSettingValue>& fields, bool authored)
			{
				std::set<GraphicsSettingId> seen;
				for (const auto& field : fields)
				{
					if (!seen.insert(field.id).second) Reject(GraphicsRequestReason::InvalidSetting, "One layer contains duplicate setting IDs.");
					Validate(field, authored); values[field.id] = field.value;
				}
			}

			void Profile(const Resource::PostProcessProfile& profile, float weight)
			{
				if (profile.schemaVersion > Resource::PostProcessProfile::kCurrentSchemaVersion)
					Reject(GraphicsRequestReason::Unsupported, "The profile schema is newer than this engine.");
				if (profile.assetId == TerraEngine::EMPTY_UUID || profile.revision == 0u)
					Reject(GraphicsRequestReason::InvalidValue, "An authored profile requires a stable asset ID and revision.");
				std::set<GraphicsSettingId> seen;
				for (const auto& field : profile.values)
				{
					if (!seen.insert(field.id).second) Reject(GraphicsRequestReason::InvalidSetting, "A profile contains duplicate values.");
					Validate(field, true, false);
				}
				seen.clear();
				for (const auto id : profile.overrideMask)
				{
					if (!seen.insert(id).second) Reject(GraphicsRequestReason::InvalidSetting, "A profile contains duplicate override masks.");
					const auto* value = Find(profile.values, id);
					if (!value) Reject(GraphicsRequestReason::InvalidValue, "A masked profile field has no authored value.");
					if (const auto* scalar = std::get_if<float>(value))
					{
						const auto* previous = std::get_if<float>(&values.at(id));
						if (!previous) Reject(GraphicsRequestReason::InvalidValue, "Continuous profile blending requires scalar values.");
						// Use double intermediate arithmetic to avoid overflow between
						// finite opposite-sign float endpoints, then revalidate ranges.
						float blended = static_cast<float>(double(*previous) * (1.0 - double(weight)) + double(*scalar) * double(weight));
						Validate({id, blended}, true); values[id] = blended;
					}
					else if (weight >= 0.5f) { Validate({id, *value}, true); values[id] = *value; }
				}
				std::set<TerraEngine::UUID> references;
				for (const auto& id : profile.customEffects)
					if (id == TerraEngine::EMPTY_UUID || !references.insert(id).second)
						Reject(GraphicsRequestReason::InvalidValue, "Custom effect references must be nonempty and unique.");
				if (profile.overrideCustomEffects && weight >= 0.5f) effects = profile.customEffects;
			}

			void Volumes()
			{
				std::vector<const PostProcessVolumeInput*> ordered;
				std::set<TerraEngine::UUID> identities;
				for (const auto& volume : input.volumes)
				{
					if ((volume.layerMask & input.cameraLayerMask) == 0u) continue;
					if (!std::isfinite(volume.weight) || volume.weight < 0.0f || volume.weight > 1.0f
						|| !std::isfinite(volume.blendDistance) || volume.blendDistance < 0.0f)
						Reject(GraphicsRequestReason::InvalidValue, "Volume blend weight or distance is invalid.");
					if (volume.weight == 0.0f) continue;
					if (volume.stableId == TerraEngine::EMPTY_UUID || !identities.insert(volume.stableId).second)
						Reject(GraphicsRequestReason::InvalidValue, "Contributing volumes require unique stable identities.");
					ordered.push_back(&volume);
				}
				std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
					return left->priority != right->priority ? left->priority < right->priority : left->stableId < right->stableId;
				});
				for (const auto* volume : ordered)
				{
					double squaredDistance = 0.0;
					if (!volume->global) for (std::size_t axis = 0; axis < 3; ++axis)
					{
						const double low = volume->boundsMinimum[axis], high = volume->boundsMaximum[axis];
						if (!std::isfinite(low) || !std::isfinite(high) || low > high)
							Reject(GraphicsRequestReason::InvalidValue, "Local volume bounds are invalid.");
						const double delta = std::max({low - input.cameraPosition[axis], 0.0, input.cameraPosition[axis] - high});
						squaredDistance += delta * delta;
					}
					if (!std::isfinite(squaredDistance)) Reject(GraphicsRequestReason::InvalidValue, "Volume distance overflowed the copied coordinate frame.");
					double distance = std::sqrt(squaredDistance);
					float influence = distance == 0.0 ? volume->weight : (volume->blendDistance > 0.0f
						? volume->weight * static_cast<float>(std::max(0.0, 1.0 - distance / volume->blendDistance)) : 0.0f);
					if (influence <= 0.0f) continue;
					if (!volume->profile) Reject(GraphicsRequestReason::TargetUnavailable, "A contributing volume profile is unavailable.");
					Profile(*volume->profile, influence);
				}
			}

			void Players()
			{
				std::set<GraphicsSettingId> allowed;
				for (const auto id : input.playerAllowedSettings)
					if (!catalog.contains(id) || !allowed.insert(id).second)
						Reject(GraphicsRequestReason::InvalidSetting, "The trusted player policy has unknown or duplicate IDs.");
				for (const auto& field : input.playerOverrides)
					if (!allowed.contains(field.id)) Reject(GraphicsRequestReason::InvalidScope, "The player patch exceeds its allowed policy.");
				Patch(input.playerOverrides, true);
				std::set<GraphicsSettingId> disabled;
				for (const auto id : input.playerDisabledSettings)
				{
					if (!allowed.contains(id) || !disabled.insert(id).second)
						Reject(GraphicsRequestReason::InvalidScope, "The player disable exceeds its allowed policy or repeats an ID.");
					const auto& descriptor = *catalog.at(id);
					GraphicsValue off;
					if (descriptor.valueKind == GraphicsValueKind::Boolean) off = false;
					else if (descriptor.valueKind == GraphicsValueKind::Unsigned32
						&& std::find(descriptor.options.begin(), descriptor.options.end(), GraphicsValue{std::uint32_t{0}}) != descriptor.options.end()) off = std::uint32_t{0};
					else Reject(GraphicsRequestReason::InvalidValue, "A player disable requires a declared Boolean or enum Off value.");
					Validate({id, off}, true); values[id] = std::move(off);
				}
			}
		};
	}

	PostProcessProfileResolution ResolvePostProcessProfile(const PostProcessProfileResolutionInput& input)
	{
		try { return {GraphicsRequestReason::None, {}, Resolver(input).Run()}; }
		catch (const Rejection& error) { return {error.reason, error.detail, std::nullopt}; }
	}

	bool IsPostProcessResolutionCurrent(const PostProcessResolvedProfile& candidate,
		const GraphicsTarget& target, const GraphicsLifecycleStamp& lifecycle, GraphicsRevision currentRevision) noexcept
	{
		return candidate.baseRevision != 0u && candidate.baseRevision == currentRevision
			&& candidate.settings.revision == candidate.baseRevision
			&& candidate.settings.target == target && candidate.settings.lifecycle == lifecycle
			&& candidate.settings.publishedFrameId == 0u && candidate.settings.postProcessResourceGeneration == 0u;
	}
}
