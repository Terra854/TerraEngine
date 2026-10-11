#pragma once
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureControls.h"
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <cmath>

namespace TerraEngine::Renderer
{
	// Immutable resolved values feed the existing Scene/Game spatial queries and
	// output owners. They never modify author camera components or editor orbit UI.
	struct RendererEfficiencyResolvedFixtureCamera
	{
		RenderViewType viewType = RenderViewType::Scene;
		std::uint32_t outputId = 0;
		glm::mat4 viewMatrix{1}, projectionMatrix{1};
		glm::vec3 cameraLocalPosition{0};
		TerraEngine::Scene::WorldPosition cameraPosition{}, referenceOrigin{};
		glm::uvec2 nativeExtent{0};
	};
	inline bool ResolveRendererEfficiencyFixtureCameras(const RendererEfficiencyFixtureIntent& intent,
		std::vector<RendererEfficiencyResolvedFixtureCamera>& output, std::string& reason)
	{
		const auto reject = [&](std::string text) { reason = std::move(text); return false; };
		if(intent.cameras.empty()) { output.clear(); reason.clear(); return true; }
		if(intent.correlation.operationId.empty() || intent.cameras.size() > 2)
			return reject("Sealed cameras require an owned operation and one or two unique Scene1/Game2 outputs.");
		if(intent.referenceOriginDelta && !intent.viewOrigins.empty()) return reject("Shared and per-view camera rebases are ambiguous.");
		std::vector<RendererEfficiencyResolvedFixtureCamera> prepared;
		std::array<bool,2> present{};
		for(const auto& source : intent.cameras)
		{
			const bool scene = source.viewType == RenderViewType::Scene && source.outputId == 1;
			const bool game = source.viewType == RenderViewType::Game && source.outputId == 2;
			if(!scene && !game) return reject("Sealed camera does not name a real Scene1/Game2 output.");
			const unsigned slot = game ? 1 : 0;
			if(present[slot]) return reject("Duplicate sealed camera output identity."); present[slot] = true;
			for(const auto& position : {source.cameraPosition, source.lookAtTarget, source.referenceOrigin})
			{
				const auto normalized = Scene::NormalizeWorldPosition(position);
				if(!normalized.Succeeded() || !(normalized.position == position)) return reject("Sealed camera contains noncanonical/nonfinite world coordinates.");
			}
			const double upSquared = glm::dot(source.up, source.up);
			if(!std::isfinite(upSquared) || upSquared < 1e-12) return reject("Sealed camera up direction is nonfinite or degenerate.");
			if(!std::isfinite(source.verticalFovDegrees) || source.verticalFovDegrees <= 0 || source.verticalFovDegrees >= 179
				|| !std::isfinite(source.nearPlane) || !std::isfinite(source.farPlane) || source.nearPlane <= 0 || source.farPlane <= source.nearPlane)
				return reject("Sealed camera projection parameters are invalid.");
			const float nativeFov = static_cast<float>(source.verticalFovDegrees), nativeNear = static_cast<float>(source.nearPlane), nativeFar = static_cast<float>(source.farPlane);
			if(!std::isfinite(nativeFov) || nativeFov <= 0 || nativeFov >= 179 || !std::isfinite(nativeNear) || nativeNear <= 0 || !std::isfinite(nativeFar) || nativeFar <= nativeNear)
				return reject("Sealed projection cannot retain its valid range in native floats.");
			if(!source.nativeExtent.x || !source.nativeExtent.y || source.nativeExtent.x > 16384 || source.nativeExtent.y > 16384
				|| std::uint64_t(source.nativeExtent.x) * source.nativeExtent.y > 64ULL * 1024 * 1024)
				return reject("Sealed camera native extent exceeds bounded readback size.");
			auto origin = source.referenceOrigin;
			if(const auto delta = RendererEfficiencyFixtureOriginDelta(intent, source.viewType, source.outputId))
			{
				const auto moved = Scene::ApplyWorldDelta(origin, *delta);
				if(!moved.Succeeded()) return reject("Sealed camera reference-origin rebase overflows canonical coordinates.");
				origin = moved.position;
			}
			// Bound both local positions before converting to GPU floats. Huge cells
			// remain exact; only their nearby difference enters the view matrix.
			const auto eye = Scene::LocalizeWorldPosition(source.cameraPosition, origin, Scene::kTransformSpaceMaximumRelativeRadiusMetres);
			const auto target = Scene::LocalizeWorldPosition(source.lookAtTarget, origin, Scene::kTransformSpaceMaximumRelativeRadiusMetres);
			if(!eye.Succeeded() || !target.Succeeded()) return reject("Sealed camera or target is outside its bounded reference frame.");
			const auto direction = target.offsetMetres - eye.offsetMetres;
			const double lengthSquared = glm::dot(direction,direction);
			if(!std::isfinite(lengthSquared) || lengthSquared < 1e-12) return reject("Sealed camera target equals its camera position.");
			const auto up = source.up / std::sqrt(upSquared), forward = direction / std::sqrt(lengthSquared);
			if(glm::dot(glm::cross(forward,up),glm::cross(forward,up)) < 1e-12) return reject("Sealed camera up direction is parallel to its target direction.");
			RendererEfficiencyResolvedFixtureCamera resolved;
			resolved.viewType = source.viewType; resolved.outputId = source.outputId;
			resolved.cameraPosition = source.cameraPosition; resolved.referenceOrigin = origin;
			resolved.cameraLocalPosition = glm::vec3(eye.offsetMetres); resolved.nativeExtent = source.nativeExtent;
			resolved.viewMatrix = glm::lookAtRH(glm::vec3(eye.offsetMetres),glm::vec3(target.offsetMetres),glm::vec3(up));
			resolved.projectionMatrix = glm::perspectiveRH_ZO(glm::radians(static_cast<float>(source.verticalFovDegrees)),
				static_cast<float>(source.nativeExtent.x) / source.nativeExtent.y, static_cast<float>(source.nearPlane), static_cast<float>(source.farPlane));
			resolved.projectionMatrix[1][1] *= -1;
			for(unsigned column=0;column<4;++column) for(unsigned row=0;row<4;++row)
				if(!std::isfinite(resolved.viewMatrix[column][row]) || !std::isfinite(resolved.projectionMatrix[column][row])) return reject("Sealed camera cannot produce finite native matrices.");
			prepared.push_back(std::move(resolved));
		}
		std::vector<std::size_t> order;
		std::vector<RendererEfficiencyFixtureViewOrderEntry> actual;
		for(const auto& camera : prepared) actual.push_back({camera.viewType,camera.outputId});
		if(!ResolveRendererEfficiencyFixtureViewOrder(intent,actual,order,reason)) return false;
		output = std::move(prepared); reason.clear(); return true;
	}
	inline bool ValidateRendererEfficiencyFixtureCameras(const RendererEfficiencyFixtureIntent& intent, std::string& reason)
	{
		std::vector<RendererEfficiencyResolvedFixtureCamera> resolved;
		return ResolveRendererEfficiencyFixtureCameras(intent, resolved, reason);
	}
}
