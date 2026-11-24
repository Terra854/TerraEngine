// GraphicsSettingsCatalog.cpp - Pure catalog construction and atomic value adapters.
#include "GraphicsSettingsCatalog.h"
#include "PostProcessColorSettings.h"
#include "PostProcessEffectSettings.h"
#include "Renderer/RenderTypes.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
namespace TerraEngine::Renderer::GraphicsSettings
{
	namespace
	{
		// A single typed table drives both directions, retaining full-width byte
		// budgets and preventing discovery and native mapping from drifting apart.
		struct Binding
		{
			GraphicsSettingId id;
			const char* name;
			const char* units;
			GraphicsValueKind kind;
			GraphicsValue (*read)(const RenderSettings&, bool, const GraphicsProfileMetadata&);
			bool (*write)(RenderSettings&, bool&, GraphicsProfileMetadata&, const GraphicsValue&);
		};
		const Binding bindings[] = {
			{GraphicsSettingId::ShadowResolution, "shadowResolution", "pixels", GraphicsValueKind::Unsigned32,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return settings.shadowResolution; },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<std::uint32_t>(&value); if(!typed) return false; settings.shadowResolution = *typed; return true; } },
			{GraphicsSettingId::ShadowFilteringSize, "shadowFilteringSize", "samples", GraphicsValueKind::Unsigned32,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return settings.shadowFilteringSize; },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<std::uint32_t>(&value); if(!typed) return false; settings.shadowFilteringSize = *typed; return true; } },
			{GraphicsSettingId::MaxUploadedLights, "maxUploadedLights", "lights", GraphicsValueKind::Unsigned32,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return settings.maxUploadedLights; },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<std::uint32_t>(&value); if(!typed) return false; settings.maxUploadedLights = *typed; return true; } },
			{GraphicsSettingId::MaxDynamicLights, "maxDynamicLights", "lights", GraphicsValueKind::Unsigned32,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return settings.maxDynamicLights; },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<std::uint32_t>(&value); if(!typed) return false; settings.maxDynamicLights = *typed; return true; } },
			{GraphicsSettingId::MaxShadowLights, "maxShadowLights", "lights", GraphicsValueKind::Unsigned32,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return settings.maxShadowLights; },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<std::uint32_t>(&value); if(!typed) return false; settings.maxShadowLights = *typed; return true; } },
			{GraphicsSettingId::MaxAreaLights, "maxAreaLights", "lights", GraphicsValueKind::Unsigned32,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return settings.maxAreaLights; },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<std::uint32_t>(&value); if(!typed) return false; settings.maxAreaLights = *typed; return true; } },
			{GraphicsSettingId::EnableLTC, "enableLTC", "", GraphicsValueKind::Boolean,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return (settings.enableLTC != 0); },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<bool>(&value); if(!typed) return false; settings.enableLTC = *typed; return true; } },
			{GraphicsSettingId::EnableIBL, "enableIBL", "", GraphicsValueKind::Boolean,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return (settings.enableIBL != 0); },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<bool>(&value); if(!typed) return false; settings.enableIBL = *typed; return true; } },
			{GraphicsSettingId::IndirectStrength, "indirectStrength", "multiplier", GraphicsValueKind::Scalar,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return settings.indirectStrength; },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<float>(&value); if(!typed) return false; settings.indirectStrength = *typed; return true; } },
			{GraphicsSettingId::AmbientOcclusionStrength, "ambientOcclusionStrength", "multiplier", GraphicsValueKind::Scalar,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return settings.ambientOcclusionStrength; },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<float>(&value); if(!typed) return false; settings.ambientOcclusionStrength = *typed; return true; } },
			{GraphicsSettingId::AmbientOcclusionQuality, "ambientOcclusionQuality", "tier", GraphicsValueKind::Unsigned32,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return settings.ambientOcclusionQuality; },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<std::uint32_t>(&value); if(!typed) return false; settings.ambientOcclusionQuality = *typed; return true; } },
			{GraphicsSettingId::AnisotropicFiltering, "anisotropicFiltering", "samples", GraphicsValueKind::Unsigned32,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return settings.anisotropicFiltering; },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<std::uint32_t>(&value); if(!typed) return false; settings.anisotropicFiltering = *typed; return true; } },
			{GraphicsSettingId::VariableRateShading, "variableRateShading", "", GraphicsValueKind::Boolean,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return (settings.variableRateShading != 0); },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<bool>(&value); if(!typed) return false; settings.variableRateShading = *typed; return true; } },
			{GraphicsSettingId::ValidationEnabled, "validationEnabled", "", GraphicsValueKind::Boolean,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return (settings.validationEnabled != 0); },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<bool>(&value); if(!typed) return false; settings.validationEnabled = *typed; return true; } },
			{GraphicsSettingId::DebugEnabled, "debugEnabled", "", GraphicsValueKind::Boolean,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return (settings.debugEnabled != 0); },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<bool>(&value); if(!typed) return false; settings.debugEnabled = *typed; return true; } },
			{GraphicsSettingId::GlobalTaskShaderCullingEnabled, "globalTaskShaderCullingEnabled", "", GraphicsValueKind::Boolean,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return (culling != 0); },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<bool>(&value); if(!typed) return false; culling = *typed; return true; } },
			{GraphicsSettingId::VirtualGeometryLodErrorScale, "virtualGeometry.lodErrorScale", "multiplier", GraphicsValueKind::Scalar,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return settings.virtualGeometry.lodErrorScale; },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<float>(&value); if(!typed) return false; settings.virtualGeometry.lodErrorScale = *typed; return true; } },
			{GraphicsSettingId::VirtualGeometryMaxLodDrop, "virtualGeometry.maxLodDrop", "levels", GraphicsValueKind::Unsigned32,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return settings.virtualGeometry.maxLodDrop; },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<std::uint32_t>(&value); if(!typed) return false; settings.virtualGeometry.maxLodDrop = *typed; return true; } },
			{GraphicsSettingId::VirtualGeometryHiZOcclusionEnabled, "virtualGeometry.hiZOcclusionEnabled", "", GraphicsValueKind::Boolean,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return (settings.virtualGeometry.hiZOcclusionEnabled != 0); },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<bool>(&value); if(!typed) return false; settings.virtualGeometry.hiZOcclusionEnabled = *typed; return true; } },
			{GraphicsSettingId::VirtualGeometryHiZSelfOcclusionGuardEnabled, "virtualGeometry.hiZSelfOcclusionGuardEnabled", "", GraphicsValueKind::Boolean,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return (settings.virtualGeometry.hiZSelfOcclusionGuardEnabled != 0); },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<bool>(&value); if(!typed) return false; settings.virtualGeometry.hiZSelfOcclusionGuardEnabled = *typed; return true; } },
			{GraphicsSettingId::VirtualGeometryHiZNearPlaneGuardEnabled, "virtualGeometry.hiZNearPlaneGuardEnabled", "", GraphicsValueKind::Boolean,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return (settings.virtualGeometry.hiZNearPlaneGuardEnabled != 0); },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<bool>(&value); if(!typed) return false; settings.virtualGeometry.hiZNearPlaneGuardEnabled = *typed; return true; } },
			{GraphicsSettingId::VirtualGeometryHiZThinGeometryGuardEnabled, "virtualGeometry.hiZThinGeometryGuardEnabled", "", GraphicsValueKind::Boolean,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return (settings.virtualGeometry.hiZThinGeometryGuardEnabled != 0); },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<bool>(&value); if(!typed) return false; settings.virtualGeometry.hiZThinGeometryGuardEnabled = *typed; return true; } },
			{GraphicsSettingId::VirtualGeometryDebugVisualizationMode, "virtualGeometry.debugVisualizationMode", "mode", GraphicsValueKind::Unsigned32,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return static_cast<std::uint32_t>(settings.virtualGeometry.debugVisualizationMode); },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<std::uint32_t>(&value); if(!typed) return false; settings.virtualGeometry.debugVisualizationMode = static_cast<VirtualGeometryDebugVisualizationMode>(*typed); return true; } },
			{GraphicsSettingId::VirtualGeometryBudgetBytes, "virtualGeometry.geometryBudgetBytes", "bytes", GraphicsValueKind::Unsigned64,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return settings.virtualGeometry.geometryBudgetBytes; },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<std::uint64_t>(&value); if(!typed) return false; settings.virtualGeometry.geometryBudgetBytes = *typed; return true; } },
			{GraphicsSettingId::VirtualGeometryPreloadBias, "virtualGeometry.preloadBias", "multiplier", GraphicsValueKind::Scalar,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return settings.virtualGeometry.preloadBias; },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<float>(&value); if(!typed) return false; settings.virtualGeometry.preloadBias = *typed; return true; } },
			{GraphicsSettingId::VirtualGeometryLodTransitionHysteresisPixels, "virtualGeometry.lodTransitionHysteresisPixels", "pixels", GraphicsValueKind::Scalar,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return settings.virtualGeometry.lodTransitionHysteresisPixels; },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<float>(&value); if(!typed) return false; settings.virtualGeometry.lodTransitionHysteresisPixels = *typed; return true; } },
			{GraphicsSettingId::VirtualGeometryLodTransitionStableFrames, "virtualGeometry.lodTransitionStableFrames", "frames", GraphicsValueKind::Unsigned32,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return settings.virtualGeometry.lodTransitionStableFrames; },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<std::uint32_t>(&value); if(!typed) return false; settings.virtualGeometry.lodTransitionStableFrames = *typed; return true; } },
			{GraphicsSettingId::VirtualGeometryDeformationCurrentBudgetBytes, "virtualGeometry.deformationCurrentBudgetBytes", "bytes", GraphicsValueKind::Unsigned64,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return settings.virtualGeometry.deformationCurrentBudgetBytes; },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<std::uint64_t>(&value); if(!typed) return false; settings.virtualGeometry.deformationCurrentBudgetBytes = *typed; return true; } },
			{GraphicsSettingId::VirtualGeometryDeformationHistoryBudgetBytes, "virtualGeometry.deformationHistoryBudgetBytes", "bytes", GraphicsValueKind::Unsigned64,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return settings.virtualGeometry.deformationHistoryBudgetBytes; },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<std::uint64_t>(&value); if(!typed) return false; settings.virtualGeometry.deformationHistoryBudgetBytes = *typed; return true; } },
			{GraphicsSettingId::VirtualGeometryDeformationBoundsBudgetBytes, "virtualGeometry.deformationBoundsBudgetBytes", "bytes", GraphicsValueKind::Unsigned64,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return settings.virtualGeometry.deformationBoundsBudgetBytes; },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<std::uint64_t>(&value); if(!typed) return false; settings.virtualGeometry.deformationBoundsBudgetBytes = *typed; return true; } },
			{GraphicsSettingId::TextureBudgetMegabytes, "profile.textureBudgetMegabytes", "MiB", GraphicsValueKind::Unsigned32,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return profile.textureBudgetMegabytes; },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<std::uint32_t>(&value); if(!typed) return false; profile.textureBudgetMegabytes = *typed; return true; } },
			{GraphicsSettingId::ProfileResolutionScale, "profile.resolutionScale", "metadata multiplier", GraphicsValueKind::Scalar,
				[](const RenderSettings& settings, bool culling, const GraphicsProfileMetadata& profile)->GraphicsValue { (void)settings; (void)culling; (void)profile; return profile.resolutionScale; },
				[](RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, const GraphicsValue& value) { (void)settings; (void)culling; (void)profile; const auto* typed = std::get_if<float>(&value); if(!typed) return false; profile.resolutionScale = *typed; return true; } },
		};
		bool ValidValue(const GraphicsValue& value, const GraphicsSettingDescriptor& descriptor)
		{
			if(value.index() != static_cast<std::size_t>(descriptor.valueKind)) return false;
			if(const auto* number = std::get_if<float>(&value); number && !std::isfinite(*number)) return false;
			if(descriptor.minimum && (descriptor.minimum->index() != value.index() || value < *descriptor.minimum)) return false;
			if(descriptor.maximum && (descriptor.maximum->index() != value.index() || value > *descriptor.maximum)) return false;
			return descriptor.options.empty() || std::find(descriptor.options.begin(), descriptor.options.end(), value) != descriptor.options.end();
		}
	}
	const GraphicsSettingDescriptor* FindGraphicsSetting(std::span<const GraphicsSettingDescriptor> catalog, GraphicsSettingId id) noexcept
	{
		const auto found = std::find_if(catalog.begin(), catalog.end(), [id](const auto& field){return field.id == id;});
		return found == catalog.end() ? nullptr : &*found;
	}
	const GraphicsSettingDescriptor* FindGraphicsSetting(std::span<const GraphicsSettingDescriptor> catalog, std::string_view name) noexcept
	{
		const auto found = std::find_if(catalog.begin(), catalog.end(), [name](const auto& field){return field.name == name;});
		return found == catalog.end() ? nullptr : &*found;
	}
	std::vector<GraphicsSettingValue> CaptureNativeGraphicsSettings(const RenderSettings& settings, bool culling,
		const GraphicsProfileMetadata& profile, const GraphicsSceneSkyValues& authored)
	{
		std::vector<GraphicsSettingValue> values;
		values.reserve(std::size(bindings) + 4u);
		for(const auto& field : bindings) values.push_back({field.id, field.read(settings, culling, profile)});
		values.push_back({GraphicsSettingId::SceneIndirectStrength, authored.sceneIndirectStrength});
		values.push_back({GraphicsSettingId::SceneAmbientOcclusionStrength, authored.sceneAmbientOcclusionStrength});
		values.push_back({GraphicsSettingId::SceneAmbientOcclusionQuality, authored.sceneAmbientOcclusionQuality});
		values.push_back({GraphicsSettingId::SkyExposure, authored.skyExposure});
		return values;
	}
	std::vector<GraphicsSettingDescriptor> BuildGraphicsSettingsCatalog(const RenderSettings& defaults, bool taskCulling,
		const GraphicsProfileMetadata& profile, const GraphicsSceneSkyValues& authored, const GraphicsCatalogCapabilities& capabilities)
	{
		const auto baseline = CaptureNativeGraphicsSettings(defaults, taskCulling, profile, authored);
		std::vector<GraphicsSettingDescriptor> catalog;
		for(const auto& field : bindings)
		{
			GraphicsSettingDescriptor descriptor;
			descriptor.id = field.id; descriptor.name = field.name; descriptor.units = field.units;
			descriptor.valueKind = field.kind; descriptor.defaultValue = field.read(defaults, taskCulling, profile);
			descriptor.gameScope = true; descriptor.available = true;
			descriptor.unavailableReason = GraphicsRequestReason::None;
			if(field.kind == GraphicsValueKind::Unsigned32) descriptor.minimum = std::uint32_t{0};
			if(field.kind == GraphicsValueKind::Unsigned64) descriptor.minimum = std::uint64_t{0};
			if(field.kind == GraphicsValueKind::Scalar) descriptor.minimum = 0.0f;
			switch(field.id)
			{
			case GraphicsSettingId::ShadowResolution: descriptor.minimum=1u; descriptor.maximum=capabilities.maxShadowResolution; descriptor.activation=GraphicsActivation::Rebuild; break;
			case GraphicsSettingId::ShadowFilteringSize: descriptor.minimum=1u; descriptor.maximum=capabilities.maxShadowFilteringSize; break;
			case GraphicsSettingId::MaxUploadedLights: descriptor.minimum=1u; descriptor.maximum=capabilities.maxUploadedLights; break;
			case GraphicsSettingId::MaxDynamicLights: case GraphicsSettingId::MaxAreaLights: descriptor.maximum=capabilities.maxUploadedLights; break;
			case GraphicsSettingId::MaxShadowLights: descriptor.maximum=kMaxShadowResourceSlots; break;
			case GraphicsSettingId::IndirectStrength: descriptor.maximum=8.0f; descriptor.detail="Global lighting multiplier; authored scene environment remains separate."; break;
			case GraphicsSettingId::AmbientOcclusionStrength: descriptor.maximum=4.0f; break;
			case GraphicsSettingId::AmbientOcclusionQuality: descriptor.maximum=3u; break;
			case GraphicsSettingId::AnisotropicFiltering:
				descriptor.minimum=1u; descriptor.maximum=capabilities.maxAnisotropy;
				for(std::uint32_t option : {1u,2u,4u,8u,16u}) if(option <= capabilities.maxAnisotropy) descriptor.options.emplace_back(option);
				descriptor.activation=GraphicsActivation::Rebuild; break;
			case GraphicsSettingId::VariableRateShading: case GraphicsSettingId::ValidationEnabled:
				descriptor.activation=GraphicsActivation::NextLaunch;
				descriptor.options.emplace_back(false);
				if(field.id == GraphicsSettingId::VariableRateShading ? capabilities.vrsSupported : capabilities.validationSupported) descriptor.options.emplace_back(true);
				descriptor.detail="Activation requires Vulkan backend initialization; current native effective state must be queried separately."; break;
			case GraphicsSettingId::DebugEnabled: case GraphicsSettingId::VirtualGeometryDebugVisualizationMode:
				descriptor.activation=GraphicsActivation::DevelopmentOnly; descriptor.available=capabilities.developmentDiagnostics;
				descriptor.unavailableReason=descriptor.available ? GraphicsRequestReason::None : GraphicsRequestReason::Unsupported;
				if(field.id == GraphicsSettingId::VirtualGeometryDebugVisualizationMode) for(std::uint32_t mode=0;mode<=8; ++mode) descriptor.options.emplace_back(mode);
				break;
			case GraphicsSettingId::VirtualGeometryLodErrorScale: descriptor.minimum=0.05f; descriptor.maximum=64.0f; break;
			case GraphicsSettingId::VirtualGeometryPreloadBias: descriptor.maximum=16.0f; break;
			case GraphicsSettingId::VirtualGeometryLodTransitionHysteresisPixels: descriptor.maximum=8.0f; break;
			case GraphicsSettingId::VirtualGeometryLodTransitionStableFrames: descriptor.minimum=1u; descriptor.maximum=16u; break;
			case GraphicsSettingId::VirtualGeometryHiZOcclusionEnabled: case GraphicsSettingId::VirtualGeometryHiZSelfOcclusionGuardEnabled:
			case GraphicsSettingId::VirtualGeometryHiZNearPlaneGuardEnabled: case GraphicsSettingId::VirtualGeometryHiZThinGeometryGuardEnabled:
				descriptor.activation=GraphicsActivation::ProfileOnly; descriptor.available=false; descriptor.unavailableReason=GraphicsRequestReason::Unsupported;
				descriptor.gameScope=false; descriptor.authoringScope=GraphicsAuthoringScope::BuildProfile;
				descriptor.detail="Serialized compatibility value only; conservative shader-side Hi-Z sampling controls effective occlusion."; break;
			case GraphicsSettingId::TextureBudgetMegabytes: case GraphicsSettingId::ProfileResolutionScale:
				descriptor.activation=GraphicsActivation::ProfileOnly; descriptor.available=false; descriptor.unavailableReason=GraphicsRequestReason::Unsupported;
				descriptor.gameScope=false; descriptor.authoringScope=GraphicsAuthoringScope::BuildProfile;
				descriptor.detail="Existing export profile metadata; no active renderer consumer. Profile scale is independent from effective internal resolution scale."; break;
			default: break;
			}
			catalog.push_back(std::move(descriptor));
		}
		// Discovery preserves existing scene/SkyComponent authoring ownership.
		// Runtime game/camera mutation must use neither of these authored paths.
		for(const auto& value : baseline)
		{
			if(value.id < GraphicsSettingId::SceneIndirectStrength || value.id > GraphicsSettingId::SkyExposure) continue;
			GraphicsSettingDescriptor descriptor; descriptor.id=value.id; descriptor.defaultValue=value.value;
			descriptor.valueKind=value.id == GraphicsSettingId::SceneAmbientOcclusionQuality ? GraphicsValueKind::Unsigned32 : GraphicsValueKind::Scalar;
			descriptor.activation=GraphicsActivation::ProfileOnly; descriptor.available=false; descriptor.unavailableReason=GraphicsRequestReason::InvalidScope;
			descriptor.authoringScope=value.id == GraphicsSettingId::SkyExposure ? GraphicsAuthoringScope::SkyComponent : GraphicsAuthoringScope::Scene;
			descriptor.detail="Use the existing transactional authored scene/component API; this value is not a runtime graphics override.";
			switch(value.id)
			{
			case GraphicsSettingId::SceneIndirectStrength: descriptor.name="scene.indirectStrength"; descriptor.minimum=0.0f; descriptor.maximum=8.0f; descriptor.units="multiplier"; break;
			case GraphicsSettingId::SceneAmbientOcclusionStrength: descriptor.name="scene.ambientOcclusionStrength"; descriptor.minimum=0.0f; descriptor.maximum=4.0f; descriptor.units="multiplier"; break;
			case GraphicsSettingId::SceneAmbientOcclusionQuality: descriptor.name="scene.ambientOcclusionQuality"; descriptor.minimum=0u; descriptor.maximum=3u; descriptor.units="tier"; break;
			case GraphicsSettingId::SkyExposure: descriptor.name="sky.exposure"; descriptor.minimum=0.0f; descriptor.maximum=16.0f; descriptor.units="sky multiplier"; break;
			default: break;
			}
			catalog.push_back(std::move(descriptor));
		}
		GraphicsSettingDescriptor aa;
		aa.id = GraphicsSettingId::AntiAliasingMode;
		aa.name = "postProcess.antiAliasingMode";
		aa.valueKind = GraphicsValueKind::Unsigned32;
		aa.defaultValue = static_cast<std::uint32_t>(GraphicsAntiAliasingMode::Off);
		aa.gameScope = true;
		aa.cameraScope = true;
		aa.available = true;
		aa.activation = GraphicsActivation::Rebuild;
		aa.unavailableReason = GraphicsRequestReason::None;
		aa.detail = "One spatial mode at native scale and one sample; every affected view prepares before publication.";
		// Off never needs an effect pipeline. Each additional option requires its
		// own native qualification bit; unrelated color support cannot enable AA.
		for(std::uint32_t mode = 0; mode <= static_cast<std::uint32_t>(GraphicsAntiAliasingMode::CMAA2); ++mode)
			if(mode == 0 || (capabilities.spatialAAModeMask & (1u << mode)) != 0) aa.options.emplace_back(mode);
		if(capabilities.indexedRasterSupported && capabilities.msaa4Supported) aa.options.emplace_back(std::uint32_t{4});
		if(capabilities.indexedRasterSupported && capabilities.taaSupported) aa.options.emplace_back(std::uint32_t{5});
		aa.detail = "Spatial AA uses native single-sample output. Required MSAA/temporal variants use qualified indexed raster with VRS off.";
		catalog.push_back(std::move(aa));
		const bool upscaleSupported = capabilities.indexedRasterSupported && capabilities.temporalUpscalingSupported;
		for(const auto id : {GraphicsSettingId::InternalResolutionScale, GraphicsSettingId::UpscalingMode,
			GraphicsSettingId::UpscalingSharpness, GraphicsSettingId::GeometryRasterMode})
		{
			GraphicsSettingDescriptor field;
			field.id = id;
			field.gameScope = field.cameraScope = true;
			field.activation = GraphicsActivation::Rebuild;
			field.available = true;
			field.unavailableReason = GraphicsRequestReason::None;
			if(id == GraphicsSettingId::InternalResolutionScale)
			{
				field.name = "postProcess.internalResolutionScale";
				field.valueKind = GraphicsValueKind::Scalar;
				field.defaultValue = 1.0f;
				field.minimum = 0.5f; field.maximum = 1.0f;
				field.options = {1.0f};
				if(upscaleSupported) field.options.emplace_back(0.5f);
				field.detail = "Half width and height requires temporal upscaling with standalone AA Off.";
			}
			else if(id == GraphicsSettingId::UpscalingSharpness)
			{
				field.name = "postProcess.upscalingSharpness";
				field.valueKind = GraphicsValueKind::Scalar;
				field.defaultValue = 0.0f;
				field.minimum = 0.0f; field.maximum = 1.0f;
				field.available = upscaleSupported && capabilities.temporalSharpeningSupported;
				field.unavailableReason = field.available ? GraphicsRequestReason::None : GraphicsRequestReason::Unsupported;
				field.detail = "Output sharpening belongs to the qualified temporal upscaler.";
			}
			else
			{
				field.valueKind = GraphicsValueKind::Unsigned32;
				field.defaultValue = std::uint32_t{0};
				field.options = {std::uint32_t{0}};
				const bool raster = id == GraphicsSettingId::GeometryRasterMode;
				field.name = raster ? "geometry.rasterMode" : "postProcess.upscalingMode";
				if(raster ? capabilities.indexedRasterSupported : upscaleSupported) field.options.emplace_back(std::uint32_t{1});
				field.detail = raster ? "Automatic preserves legacy routing; Indexed selects a qualified ordinary indexed renderer."
					: "Temporal reconstruction requires indexed raster, VRS off, half internal extent and standalone AA Off.";
			}
			catalog.push_back(std::move(field));
		}
		AppendPostProcessColorDescriptors(catalog, capabilities.postProcessColorSupported);
		AppendPostProcessEffectDescriptors(catalog, static_cast<PostProcessEffectCapability>(capabilities.postProcessEffectMask));
		return catalog;
	}
	GraphicsRequestReason ValidateSpatialAAValue(const GraphicsSettingDescriptor& descriptor, const GraphicsValue& value) noexcept
	{
		if(descriptor.id != GraphicsSettingId::AntiAliasingMode) return GraphicsRequestReason::InvalidSetting;
		return ValidateGraphicsAASettingValue(descriptor, value);
	}
	bool IsGraphicsAASetting(GraphicsSettingId id) noexcept
	{
		return id == GraphicsSettingId::AntiAliasingMode || id == GraphicsSettingId::InternalResolutionScale
			|| id == GraphicsSettingId::UpscalingMode || id == GraphicsSettingId::UpscalingSharpness || id == GraphicsSettingId::GeometryRasterMode;
	}
	GraphicsRequestReason ValidateGraphicsAASettingValue(const GraphicsSettingDescriptor& descriptor, const GraphicsValue& value) noexcept
	{
		if(!IsGraphicsAASetting(descriptor.id)) return GraphicsRequestReason::InvalidSetting;
		if(descriptor.id == GraphicsSettingId::InternalResolutionScale || descriptor.id == GraphicsSettingId::UpscalingSharpness)
		{
			const auto* scalar = std::get_if<float>(&value);
			if(!scalar || !std::isfinite(*scalar)) return GraphicsRequestReason::InvalidValue;
			if(descriptor.id == GraphicsSettingId::UpscalingSharpness)
				return *scalar < 0.0f || *scalar > 1.0f ? GraphicsRequestReason::InvalidValue
					: descriptor.available ? GraphicsRequestReason::None : GraphicsRequestReason::Unsupported;
			if(*scalar <= 0.0f) return GraphicsRequestReason::InvalidValue;
			if(*scalar != 1.0f && *scalar != 0.5f) return GraphicsRequestReason::Unsupported;
		}
		else
		{
			const auto* mode = std::get_if<std::uint32_t>(&value);
			const auto maximum = descriptor.id == GraphicsSettingId::AntiAliasingMode ? 5u : 1u;
			if(!mode || *mode > maximum) return GraphicsRequestReason::InvalidValue;
		}
		if(!descriptor.available || std::find(descriptor.options.begin(), descriptor.options.end(), value) == descriptor.options.end())
			return GraphicsRequestReason::Unsupported;
		return GraphicsRequestReason::None;
	}
	GraphicsRequestReason ResolveGraphicsAAConfiguration(std::span<const GraphicsSettingValue> values, GraphicsAAConfiguration& configuration) noexcept
	{
		GraphicsAAConfiguration next;
		bool vrs = false;
		for(const auto& field : values)
		{
			if(field.id == GraphicsSettingId::AntiAliasingMode || field.id == GraphicsSettingId::UpscalingMode || field.id == GraphicsSettingId::GeometryRasterMode)
			{
				const auto* mode = std::get_if<std::uint32_t>(&field.value);
				if(!mode || *mode > (field.id == GraphicsSettingId::AntiAliasingMode ? 5u : 1u)) return GraphicsRequestReason::InvalidValue;
				if(field.id == GraphicsSettingId::AntiAliasingMode) next.antiAliasing = static_cast<GraphicsAntiAliasingMode>(*mode);
				else if(field.id == GraphicsSettingId::UpscalingMode) next.upscaling = static_cast<GraphicsUpscalingMode>(*mode);
				else next.raster = static_cast<GraphicsGeometryRasterMode>(*mode);
			}
			else if(field.id == GraphicsSettingId::InternalResolutionScale || field.id == GraphicsSettingId::UpscalingSharpness)
			{
				const auto* scalar = std::get_if<float>(&field.value);
				if(!scalar || !std::isfinite(*scalar)) return GraphicsRequestReason::InvalidValue;
				if(field.id == GraphicsSettingId::InternalResolutionScale) next.internalScale = *scalar; else next.sharpening = *scalar;
			}
			else if(field.id == GraphicsSettingId::VariableRateShading)
			{
				const auto* enabled = std::get_if<bool>(&field.value);
				if(!enabled) return GraphicsRequestReason::InvalidValue;
				vrs = *enabled;
			}
		}
		if(next.internalScale <= 0.0f || next.sharpening < 0.0f || next.sharpening > 1.0f) return GraphicsRequestReason::InvalidValue;
		const bool indexed = next.raster == GraphicsGeometryRasterMode::Indexed;
		const bool upscale = next.upscaling == GraphicsUpscalingMode::Temporal;
		const bool coverage = next.antiAliasing == GraphicsAntiAliasingMode::MSAA;
		const bool temporal = next.antiAliasing == GraphicsAntiAliasingMode::TAA;
		// Qualify the complete tuple before deriving any new effective metadata.
		// Additional renderer/VRS combinations must be implemented and qualified
		// explicitly before expanding these initial supported configurations.
		if((indexed && vrs) || ((coverage || temporal || upscale) && (!indexed || vrs))) return GraphicsRequestReason::Unsupported;
		if(upscale ? (next.antiAliasing != GraphicsAntiAliasingMode::Off || next.internalScale != 0.5f)
			: (next.internalScale != 1.0f || next.sharpening != 0.0f)) return GraphicsRequestReason::Unsupported;
		next.sampleCount = coverage ? 4u : 1u;
		configuration = next;
		return GraphicsRequestReason::None;
	}
	std::uint32_t ResolveGraphicsAAInternalDimension(std::uint32_t outputDimension, const GraphicsAAConfiguration& configuration) noexcept
	{
		if(outputDimension == 0u) return 0u;
		return configuration.internalScale == 0.5f ? std::max(1u, outputDimension / 2u) : outputDimension;
	}
	GraphicsRequestReason ValidateSpatialAAConfiguration(std::span<const GraphicsSettingValue> values, std::uint32_t sampleCount) noexcept
	{
		GraphicsAAConfiguration configuration;
		const auto reason = ResolveGraphicsAAConfiguration(values, configuration);
		return reason != GraphicsRequestReason::None ? reason : sampleCount == configuration.sampleCount
			? GraphicsRequestReason::None : GraphicsRequestReason::Unsupported;
	}
	bool ApplyNativeGraphicsSettings(std::span<const GraphicsSettingValue> values, std::span<const GraphicsSettingDescriptor> catalog,
		RenderSettings& settings, bool& culling, GraphicsProfileMetadata& profile, std::string& reason)
	{
		// Admission checks all values and maps only local copies. Neither a bad
		// type nor a late cross-field conflict can leave earlier changes visible.
		auto nextSettings=settings; auto nextProfile=profile; bool nextCulling=culling;
		std::unordered_set<GraphicsSettingId> seen;
		for(const auto& value : values)
		{
			const auto binding=std::find_if(std::begin(bindings),std::end(bindings),[&](const auto& entry){return entry.id == value.id;});
			const auto* descriptor=FindGraphicsSetting(catalog,value.id);
			if(!seen.insert(value.id).second || binding == std::end(bindings) || !descriptor || !ValidValue(value.value,*descriptor)
				|| !binding->write(nextSettings,nextCulling,nextProfile,value.value))
			{ reason="Unknown, duplicate, authored-only or invalid typed graphics value."; return false; }
		}
		if(nextSettings.maxDynamicLights > nextSettings.maxUploadedLights || nextSettings.maxShadowLights > nextSettings.maxDynamicLights
			|| nextSettings.maxAreaLights > nextSettings.maxDynamicLights)
		{ reason="Retained, shadow and area-light limits exceed their containing light budget."; return false; }
		settings=nextSettings; culling=nextCulling; profile=nextProfile; reason.clear(); return true;
	}
	bool ApplyLegacyQualityPreset(RenderSettings& settings, QualityPreset preset) noexcept
	{
		if(preset == QualityPreset::Custom) return true;
		if(preset > QualityPreset::Ultra) return false;
		// Literal legacy tiers preserve old behavior while sharing the same source
		// with the twelve-field runtime preset definitions below.
		switch(preset)
		{
		case QualityPreset::Low:
			settings.shadowResolution=512u;
			settings.shadowFilteringSize=1u;
			settings.maxUploadedLights=64u;
			settings.maxDynamicLights=32u;
			settings.maxShadowLights=1u;
			settings.maxAreaLights=0u;
			settings.enableLTC=0u;
			settings.enableIBL=0u;
			settings.indirectStrength=0.35f;
			settings.ambientOcclusionStrength=0.5f;
			settings.ambientOcclusionQuality=0u;
			settings.anisotropicFiltering=1u;
			settings.variableRateShading=0u;
			break;
		case QualityPreset::Medium:
			settings.shadowResolution=1024u;
			settings.shadowFilteringSize=3u;
			settings.maxUploadedLights=128u;
			settings.maxDynamicLights=64u;
			settings.maxShadowLights=2u;
			settings.maxAreaLights=2u;
			settings.enableLTC=1u;
			settings.enableIBL=1u;
			settings.indirectStrength=0.75f;
			settings.ambientOcclusionStrength=0.75f;
			settings.ambientOcclusionQuality=1u;
			settings.anisotropicFiltering=1u;
			settings.variableRateShading=0u;
			break;
		case QualityPreset::High:
			settings.shadowResolution=2048u;
			settings.shadowFilteringSize=5u;
			settings.maxUploadedLights=256u;
			settings.maxDynamicLights=128u;
			settings.maxShadowLights=3u;
			settings.maxAreaLights=4u;
			settings.enableLTC=1u;
			settings.enableIBL=1u;
			settings.indirectStrength=1.0f;
			settings.ambientOcclusionStrength=1.0f;
			settings.ambientOcclusionQuality=2u;
			settings.anisotropicFiltering=4u;
			settings.variableRateShading=0u;
			break;
		case QualityPreset::Ultra:
			settings.shadowResolution=4096u;
			settings.shadowFilteringSize=7u;
			settings.maxUploadedLights=512u;
			settings.maxDynamicLights=256u;
			settings.maxShadowLights=8u;
			settings.maxAreaLights=8u;
			settings.enableLTC=1u;
			settings.enableIBL=1u;
			settings.indirectStrength=1.25f;
			settings.ambientOcclusionStrength=1.25f;
			settings.ambientOcclusionQuality=3u;
			settings.anisotropicFiltering=16u;
			settings.variableRateShading=0u;
			break;
		default: return false;
		}
		return true;
	}
	std::vector<GraphicsPresetDefinition> BuildGraphicsPresetDefinitions()
	{
		std::vector<GraphicsPresetDefinition> presets;
		for(const auto preset : {QualityPreset::Low,QualityPreset::Medium,QualityPreset::High,QualityPreset::Ultra})
		{
			RenderSettings resolved; ApplyLegacyQualityPreset(resolved,preset);
			GraphicsPresetDefinition definition; definition.preset=preset;
			// VRS is the thirteenth legacy field, but requires backend startup. A
			// named runtime preset must remain usable as one live/rebuild request.
			for(std::size_t index=0;index<12u;++index) definition.values.push_back({bindings[index].id,bindings[index].read(resolved,true,{})});
			presets.push_back(std::move(definition));
		}
		return presets;
	}
}
