// ShadowWorkPlan.cpp — exact semantic inputs and submission-qualified ownership.
// Preparation is pure CPU work; the backend calls this ledger at protected
// recording/submission boundaries using its existing ordered graphics receipts.
#include "ShadowWorkPlan.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <tuple>
#include <map>
#include <set>

namespace TerraEngine::Renderer
{
	bool ShadowCasterStamp::operator==(const ShadowCasterStamp& other) const noexcept
	{
		// Tie references only: comparing owned vectors never copies or normalizes
		// their payloads. Only the two observation counters are excluded.
		const auto inputs = [](const ShadowCasterStamp& s) noexcept {
			return std::tie(s.objectId, s.lifetimeGeneration, s.segmentId,
				s.meshId, s.assetId, s.materialId, s.shaderId, s.textureId,
				s.meshRevision, s.assetRevision, s.materialRevision, s.shaderRevision, s.textureRevision,
				s.texturePublications, s.worldPosition, s.referenceOrigin, s.rotation, s.scale,
				s.shadowBehavior, s.materialShadowParameters, s.animationInstanceId,
				s.paletteGeneration, s.deformationGeneration, s.deformationProgramGeneration,
				s.deformationContentIdLow, s.deformationContentIdHigh, s.deformationPalette, s.morphWeights,
				s.geometryRoute, s.selectedLod, s.selectionFlags, s.firstMeshlet, s.meshletCount,
				s.firstCluster, s.clusterCount, s.pageId, s.fallbackPageId, s.pageContentRevision,
				s.residencyGeneration, s.payloadContentRevision, s.payloadByteOffset, s.payloadByteSize,
				s.payloadSource, s.emittedSource, s.staticEligible, s.staticSupport, s.staticClassificationReason, s.evidenceMask);
		};
		return inputs(*this) == inputs(other);
	}

	namespace
	{
		template<typename T, std::size_t N>
		bool Finite(const std::array<T, N>& values)
		{
			return std::all_of(values.begin(), values.end(), [](auto value) { return std::isfinite(value); });
		}
		bool CanonicalOrigin(const ShadowCanonicalOrigin& origin)
		{
			return Finite(origin.offsetMetres)
				&& std::all_of(origin.offsetMetres.begin(), origin.offsetMetres.end(),
					[](double offset) { return offset >= -32768.0 && offset < 32768.0; });
		}
		auto MembershipIdentity(const ShadowCasterStamp& row)
		{
			return std::tie(row.objectId, row.lifetimeGeneration, row.segmentId);
		}
		bool ValidMembership(const ShadowCasterMembership& membership)
		{
			if (!membership.reusable) return false;
			for (std::size_t i = 0; i < membership.rows.size(); ++i)
			{
				if (!IsShadowCasterStampReusable(membership.rows[i])) return false;
				if (i != 0 && !(MembershipIdentity(membership.rows[i - 1]) < MembershipIdentity(membership.rows[i])))
					return false;
			}
			return true;
		}
		template<typename T>
		void HashValue(std::uint64_t& digest, T value)
		{
			// Hash each initialized semantic value rather than object padding.
			// Equality always checks exact rows; this inexpensive digest only rejects.
			digest ^= static_cast<std::uint64_t>(value);
			digest *= 1099511628211ULL;
		}
		void HashValue(std::uint64_t& digest, double value)
		{
			// IEEE signed zeros compare equal, so their digest must also be equal.
			HashValue(digest, std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
		}
		template<typename T, std::size_t N>
		void HashValue(std::uint64_t& digest, const std::array<T, N>& values)
		{
			for (const auto value : values) HashValue(digest, value);
		}
		std::uint64_t DigestRows(std::span<const ShadowCasterStamp> rows)
		{
			std::uint64_t digest = 14695981039346656037ULL;
			HashValue(digest, rows.size());
			for (const auto& row : rows)
			{
			HashValue(digest, row.objectId);
			HashValue(digest, row.lifetimeGeneration);
			HashValue(digest, row.segmentId);
			HashValue(digest, row.meshId);
			HashValue(digest, row.assetId);
			HashValue(digest, row.materialId);
			HashValue(digest, row.shaderId);
			HashValue(digest, row.textureId);
			HashValue(digest, row.meshRevision);
			HashValue(digest, row.assetRevision);
			HashValue(digest, row.materialRevision);
			HashValue(digest, row.shaderRevision);
			HashValue(digest, row.textureRevision);
			HashValue(digest, row.texturePublications.size());
			for (const auto& texture : row.texturePublications) {
				HashValue(digest, texture.textureId);
				HashValue(digest, texture.publicationGeneration);
				HashValue(digest, texture.bindingIndex);
			}
			HashValue(digest, row.worldPosition.cell);
			HashValue(digest, row.worldPosition.offsetMetres);
			HashValue(digest, row.referenceOrigin.cell);
			HashValue(digest, row.referenceOrigin.offsetMetres);
			HashValue(digest, row.rotation);
			HashValue(digest, row.scale);
			HashValue(digest, row.shadowBehavior);
			HashValue(digest, row.materialShadowParameters);
			HashValue(digest, row.animationInstanceId);
			// Observation counters do not change immutable shader inputs; retain
			// them in exported rows but exclude them from semantic fast rejection.
			HashValue(digest, row.paletteGeneration);
			HashValue(digest, row.deformationGeneration);
			HashValue(digest, row.deformationProgramGeneration);
			HashValue(digest, row.deformationContentIdLow);
			HashValue(digest, row.deformationContentIdHigh);
			HashValue(digest, row.deformationPalette.size());
			for (const auto& matrix : row.deformationPalette) HashValue(digest, matrix);
			HashValue(digest, row.morphWeights.size());
			for (double weight : row.morphWeights) HashValue(digest, weight);
			HashValue(digest, row.geometryRoute);
			HashValue(digest, row.selectedLod);
			HashValue(digest, row.selectionFlags);
			HashValue(digest, row.firstMeshlet);
			HashValue(digest, row.meshletCount);
			HashValue(digest, row.firstCluster);
			HashValue(digest, row.clusterCount);
			HashValue(digest, row.pageId);
			HashValue(digest, row.fallbackPageId);
			HashValue(digest, row.pageContentRevision);
			HashValue(digest, row.residencyGeneration);
			HashValue(digest, row.payloadContentRevision);
			HashValue(digest, row.payloadByteOffset);
			HashValue(digest, row.payloadByteSize);
			HashValue(digest, row.payloadSource);
			HashValue(digest, row.emittedSource);
			HashValue(digest, row.staticEligible);
			HashValue(digest, row.staticSupport.authoredStatic);
			HashValue(digest, row.staticSupport.knownDefaultShadowAbi);
			HashValue(digest, row.staticSupport.ownerPublicationsComplete);
			HashValue(digest, row.staticSupport.materialShadowSupported);
			HashValue(digest, row.staticSupport.geometrySupported);
			HashValue(digest, row.staticSupport.deformationAbsent);
			HashValue(digest, row.staticClassificationReason);
			HashValue(digest, row.evidenceMask);
			}
			return digest;
		}
		bool IsCube(const ShadowViewKey& view) { return static_cast<std::uint32_t>(view.resourceKind) == 1U; }
		bool PhysicalOverlap(const ShadowViewKey& left, const ShadowViewKey& right)
		{
			// A point candidate writes all six physical layers atomically, so every
			// face overlaps the same cube ownership group. Other regions are face 0.
			return left.rendererSessionGeneration == right.rendererSessionGeneration
				&& left.resourceKind == right.resourceKind
				&& left.allocationGeneration == right.allocationGeneration
				&& left.regionId == right.regionId
				&& (IsCube(left) || left.faceIndex == right.faceIndex);
		}
		bool SameContentInputs(const ShadowViewKey& left, const ShadowViewKey& right)
		{
			// Logical view selectors may differ only through an explicit alias.
			// Every physical, projection, origin and light input remains exact.
			return left.rendererSessionGeneration == right.rendererSessionGeneration
				&& left.referenceOrigin == right.referenceOrigin && left.lightId == right.lightId
				&& left.resourceKind == right.resourceKind && left.faceIndex == right.faceIndex
				&& left.allocationGeneration == right.allocationGeneration && left.regionId == right.regionId
				&& left.extent == right.extent && left.depthFormat == right.depthFormat
				&& left.lightProjectionStamp == right.lightProjectionStamp;
		}
		bool ValidProjection(const std::array<double, 16>& matrix)
		{
			return Finite(matrix) && std::any_of(matrix.begin(), matrix.end(), [](double v) { return v != 0.0; });
		}
		bool ValidView(const ShadowViewKey& view, std::uint64_t session)
		{
			const auto kind = static_cast<std::uint32_t>(view.resourceKind);
			if (session == 0 || view.rendererSessionGeneration != session
				|| view.allocationGeneration == 0 || view.lightId == EMPTY_UUID || kind > 3
				|| view.extent[0] == 0 || view.extent[1] == 0 || view.depthFormat == 0
				|| !CanonicalOrigin(view.referenceOrigin) || !ValidProjection(view.lightProjectionStamp.viewProjection)
				|| !Finite(view.lightProjectionStamp.lightParameters)
				|| !Finite(view.lightProjectionStamp.shadowFilterParameters)
				|| (IsCube(view) ? view.faceIndex >= 6 : view.faceIndex != 0))
				return false;
			if (IsCube(view))
			{
				if (!view.lightProjectionStamp.cubeProjectionEvidenceComplete) return false;
				for (const auto& matrix : view.lightProjectionStamp.cubeViewProjections)
					if (!ValidProjection(matrix)) return false;
			}
			return true;
		}
		bool ValidCandidate(const ShadowRefreshCandidate& candidate, std::uint64_t session, bool recorded)
		{
			if (!ValidView(candidate.view, session) || candidate.contentToken == 0
				|| candidate.frameSerial == 0 || candidate.commandSerial == 0
				|| !ValidMembership(candidate.membership))
				return false;
			const std::uint8_t required = IsCube(candidate.view) ? 0x3FU : 1U;
			return candidate.requiredFaceMask == required
				&& (!recorded || (candidate.recordingComplete && candidate.recordedFaceMask == required));
		}
		ShadowCachePublicationEntry Entry(const ShadowRefreshCandidate& candidate, ShadowCachePublicationState status)
		{
			ShadowCachePublicationEntry entry;
			entry.view = candidate.view;
			entry.membership = candidate.membership;
			entry.contentToken = candidate.contentToken;
			entry.frameSerial = candidate.frameSerial;
			entry.commandSerial = candidate.commandSerial;
			entry.status = status;
			return entry;
		}
		void Store(std::vector<ShadowCachePublicationEntry>& entries, ShadowCachePublicationEntry value, bool physical)
		{
			const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
				if (physical) return PhysicalOverlap(entry.view, value.view);
				// Retain only the current qualified value for this view's physical
				// slot. Projection/origin changes and light reassignment replace the
				// obsolete large caster collection. Find/CanReuse still compare the
				// complete key, so storage compaction never grants a looser hit.
				const auto& a = entry.view;
				const auto& b = value.view;
				return a.rendererSessionGeneration == b.rendererSessionGeneration
					&& a.viewType == b.viewType && a.viewId == b.viewId
					&& a.resourceKind == b.resourceKind && a.faceIndex == b.faceIndex
					&& a.allocationGeneration == b.allocationGeneration && a.regionId == b.regionId;
			});
			if (found == entries.end()) entries.push_back(std::move(value));
			else *found = std::move(value);
		}
		bool Submitted(const ShadowCachePublicationEntry& entry)
		{
			return (entry.status == ShadowCachePublicationState::Submitted
				|| entry.status == ShadowCachePublicationState::Complete) && entry.submitSerial != 0;
		}
	}

	bool IsShadowCasterStampReusable(const ShadowCasterStamp& stamp)
	{
		return (stamp.evidenceMask & kCompleteShadowCasterEvidence) == kCompleteShadowCasterEvidence
			&& stamp.objectId != EMPTY_UUID && stamp.meshId != EMPTY_UUID && stamp.lifetimeGeneration != 0
			&& CanonicalOrigin(stamp.worldPosition) && CanonicalOrigin(stamp.referenceOrigin)
			&& Finite(stamp.rotation) && Finite(stamp.scale) && Finite(stamp.materialShadowParameters)
			&& std::all_of(stamp.texturePublications.begin(), stamp.texturePublications.end(), [](const auto& texture) {
				return texture.textureId != EMPTY_UUID && texture.publicationGeneration != 0;
			})
			&& std::all_of(stamp.deformationPalette.begin(), stamp.deformationPalette.end(), [](const auto& matrix) { return Finite(matrix); })
			&& std::all_of(stamp.morphWeights.begin(), stamp.morphWeights.end(), [](double weight) { return std::isfinite(weight); })
			&& stamp.payloadByteOffset <= std::numeric_limits<std::uint64_t>::max() - stamp.payloadByteSize
			&& stamp.firstMeshlet <= std::numeric_limits<std::uint64_t>::max() - stamp.meshletCount
			&& stamp.firstCluster <= std::numeric_limits<std::uint64_t>::max() - stamp.clusterCount;
	}
	bool ExactShadowCasterStampEqual(const ShadowCasterStamp& left, const ShadowCasterStamp& right)
	{
		return IsShadowCasterStampReusable(left) && IsShadowCasterStampReusable(right) && left == right;
	}
	ShadowCasterMembership NormalizeShadowCasterMembership(std::span<const ShadowCasterStampInput> inputs)
	{
		ShadowCasterMembership result;
		result.rows.reserve(inputs.size());
		for (const auto& input : inputs)
		{
			if (!IsShadowCasterStampReusable(input.semantic))
			{
				result.reason = "Required immutable caster evidence is missing or noncanonical.";
				return result;
			}
			result.rows.push_back(input.semantic);
		}
		std::sort(result.rows.begin(), result.rows.end(), [](const auto& a, const auto& b) {
			return MembershipIdentity(a) < MembershipIdentity(b);
		});
		for (std::size_t i = 1; i < result.rows.size(); ++i)
			if (MembershipIdentity(result.rows[i - 1]) == MembershipIdentity(result.rows[i]))
			{
				result.reason = "Duplicate object/lifetime/segment coverage.";
				return result;
			}
		result.digest = DigestRows(result.rows);
		result.reusable = true;
		return result;
	}
	bool ExactShadowCasterMembershipEqual(const ShadowCasterMembership& left, const ShadowCasterMembership& right)
	{
		return ValidMembership(left) && ValidMembership(right) && left.digest == right.digest && left.rows == right.rows;
	}

	StaticShadowClassification ClassifyStaticShadowCaster(const ShadowCasterStamp& stamp)
	{
		const auto& proof = stamp.staticSupport;
		if (!proof.authoredStatic) return {false, StaticShadowClassificationReason::NotAuthoredStatic};
		if (!proof.knownDefaultShadowAbi) return {false, StaticShadowClassificationReason::UnknownShaderAbi};
		if (!proof.ownerPublicationsComplete) return {false, StaticShadowClassificationReason::IncompleteOwners};
		if (!proof.materialShadowSupported) return {false, StaticShadowClassificationReason::UnsupportedMaterial};
		if (!proof.geometrySupported) return {false, StaticShadowClassificationReason::UnsupportedGeometry};
		if (!proof.deformationAbsent) return {false, StaticShadowClassificationReason::Deformable};
		if (!IsShadowCasterStampReusable(stamp)) return {false, StaticShadowClassificationReason::IncompleteSemanticProof};
		return {true, StaticShadowClassificationReason::Eligible};
	}

	StaticShadowPartition BuildStaticShadowPartition(const ShadowViewKey& view,
		std::span<const ShadowCasterStampInput> inputs)
	{
		StaticShadowPartition plan; plan.view = view;
		if (inputs.size() > std::numeric_limits<std::uint32_t>::max()) {
			plan.reason = "Caster count exceeds original-index capacity."; return plan;
		}
		for (std::size_t i = 0; i < inputs.size(); ++i) plan.allSourceIndices.push_back(static_cast<std::uint32_t>(i));
		if (!ValidView(view, view.rendererSessionGeneration) || static_cast<std::uint8_t>(view.viewType) > 1) {
			plan.reason = "Static partition requires a valid qualified render view."; return plan;
		}
		std::set<std::tuple<UUID, std::uint64_t, std::uint64_t>> identities;
		// Structural coverage is different from shader-input reuse proof. Unknown
		// dynamic owners must remain drawable without poisoning proven statics.
		for (const auto& input : inputs) {
			const auto& row = input.semantic;
			if (row.objectId == EMPTY_UUID || row.meshId == EMPTY_UUID || !row.lifetimeGeneration || !row.meshletCount
				|| !CanonicalOrigin(row.worldPosition) || !CanonicalOrigin(row.referenceOrigin)
				|| !Finite(row.rotation) || !Finite(row.scale) || !Finite(row.materialShadowParameters)
				|| !std::all_of(row.deformationPalette.begin(), row.deformationPalette.end(), [](const auto& matrix) { return Finite(matrix); })
				|| !std::all_of(row.morphWeights.begin(), row.morphWeights.end(), [](double weight) { return std::isfinite(weight); })
				|| row.firstMeshlet > UINT64_MAX - row.meshletCount || row.firstCluster > UINT64_MAX - row.clusterCount
				|| row.payloadByteOffset > UINT64_MAX - row.payloadByteSize
				|| !identities.emplace(row.objectId, row.lifetimeGeneration, row.segmentId).second) {
				plan.reason = "Malformed or duplicate original caster coverage."; return plan;
			}
		}
		std::vector<ShadowCasterStampInput> statics;
		for (std::size_t i = 0; i < inputs.size(); ++i) {
			const auto& input = inputs[i]; plan.allMembership.rows.push_back(input.semantic);
			if (ClassifyStaticShadowCaster(input.semantic).eligible) {
				statics.push_back(input); plan.staticSourceIndices.push_back(static_cast<std::uint32_t>(i));
			} else {
				plan.dynamicMembership.rows.push_back(input.semantic); plan.dynamicSourceIndices.push_back(static_cast<std::uint32_t>(i));
			}
		}
		const auto finishCoverage = [](ShadowCasterMembership& membership) {
			std::sort(membership.rows.begin(), membership.rows.end(), [](const auto& a, const auto& b) { return MembershipIdentity(a) < MembershipIdentity(b); });
			membership.digest = DigestRows(membership.rows);
			membership.reusable = std::all_of(membership.rows.begin(), membership.rows.end(), IsShadowCasterStampReusable);
			if (!membership.reusable) membership.reason = "Complete drawable coverage contains unknown semantic proof.";
		};
		finishCoverage(plan.allMembership); finishCoverage(plan.dynamicMembership);
		plan.staticMembership = NormalizeShadowCasterMembership(statics);
		plan.valid = true;
		return plan;
	}

	namespace
	{
		bool AddBytes(std::uint64_t a, std::uint64_t b, std::uint64_t& sum)
		{
			if (a > UINT64_MAX - b) return false;
			sum = a + b; return true;
		}
		// All floating fields have already passed finite/canonical validation.
		// This complete stable key avoids container order or hash tie breakers.
		auto StaticViewOrder(const ShadowViewKey& view)
		{
			const auto& projection = view.lightProjectionStamp;
			return std::tie(view.lightId, view.rendererSessionGeneration, view.viewType, view.viewId,
				view.resourceKind, view.faceIndex, view.allocationGeneration, view.regionId, view.extent, view.depthFormat,
				view.referenceOrigin.cell, view.referenceOrigin.offsetMetres, projection.viewProjection,
				projection.lightParameters, projection.shadowFilterParameters, projection.semanticRevision,
				projection.cubeViewProjections, projection.cubeProjectionEvidenceComplete);
		}
		bool ValidStaticRequest(const StaticShadowAdmissionRequest& request)
		{
			return ValidView(request.view, request.view.rendererSessionGeneration)
				&& static_cast<std::uint8_t>(request.view.viewType) <= 1
				&& std::isfinite(request.retainedLightPriority) && request.queriedDepthBytes
				&& request.requiredFaceMask == (IsCube(request.view) ? 0x3f : 1);
		}
	}
	StaticShadowAdmissionPlan BuildStaticShadowAdmissionPlan(std::uint64_t requestedBytes,
		std::uint64_t previousEffectiveBytes, std::span<const StaticShadowAdmissionRequest> requests,
		std::span<const StaticShadowAllocationCharge> charges)
	{
		StaticShadowAdmissionPlan plan; plan.requestedBytes = requestedBytes;
		const auto reject = [&](const char* reason) {
			plan.reason = reason; plan.admittedRequestIndices.clear(); plan.deferredRequestIndices.clear();
			plan.retainedChargeIndices.clear(); plan.retireChargeIndices.clear(); return plan;
		};
		if (requests.size() > UINT32_MAX || charges.size() > UINT32_MAX) return reject("Static admission index overflow.");
		std::vector<std::uint64_t> required;
		for (std::size_t i = 0; i < requests.size(); ++i) {
			std::uint64_t bytes = 0;
			if (!ValidStaticRequest(requests[i]) || !AddBytes(requests[i].queriedDepthBytes, requests[i].queriedAuxBytes, bytes))
				return reject("Invalid or overflowing queried static allocation.");
			for (std::size_t j = 0; j < i; ++j) if (requests[i].view == requests[j].view) return reject("Duplicate qualified static request.");
			required.push_back(bytes);
		}
		for (std::size_t i = 0; i < charges.size(); ++i) {
			const auto& charge = charges[i]; std::uint64_t bytes = 0;
			if (!ValidView(charge.view, charge.view.rendererSessionGeneration) || static_cast<std::uint8_t>(charge.view.viewType) > 1
				|| !charge.allocationGeneration || !charge.depthBytes || !std::isfinite(charge.retainedLightPriority)
				|| static_cast<std::uint8_t>(charge.state) > static_cast<std::uint8_t>(StaticShadowChargeState::Retiring)
				|| !AddBytes(charge.depthBytes, charge.auxiliaryBytes, bytes) || !AddBytes(plan.chargedBytes, bytes, plan.chargedBytes))
				return reject("Invalid or overflowing physical static charge.");
			for (std::size_t j = 0; j < i; ++j)
				if (charge.allocationGeneration == charges[j].allocationGeneration && charge.view == charges[j].view)
					return reject("Duplicate physical static charge.");
			auto& category = charge.state == StaticShadowChargeState::Reserved ? plan.reservedBytes
				: (charge.state == StaticShadowChargeState::Resident ? plan.residentBytes : plan.retiringBytes);
			if (!AddBytes(category, bytes, category)) return reject("Static category byte overflow.");
		}
		plan.draining = plan.chargedBytes > requestedBytes;
		plan.effectiveBytes = plan.draining ? previousEffectiveBytes : requestedBytes;
		if (plan.chargedBytes > plan.effectiveBytes) return reject("Previous effective bound cannot cover retained physical charges.");
		std::vector<bool> retained(requests.size(), false);
		for (std::size_t i = 0; i < charges.size(); ++i) {
			const auto& charge = charges[i];
			const auto found = std::find_if(requests.begin(), requests.end(), [&](const auto& request) { return request.view == charge.view; });
			if (!plan.draining && charge.state != StaticShadowChargeState::Retiring && found != requests.end()) {
				const auto index = static_cast<std::size_t>(found - requests.begin());
				if (retained[index]) return reject("Multiple live owners for one qualified static request.");
				retained[index] = true; plan.retainedChargeIndices.push_back(static_cast<std::uint32_t>(i));
			} else if (charge.state != StaticShadowChargeState::Retiring) plan.retireChargeIndices.push_back(static_cast<std::uint32_t>(i));
		}
		std::vector<std::uint32_t> order;
		for (std::size_t i = 0; i < requests.size(); ++i) order.push_back(static_cast<std::uint32_t>(i));
		std::sort(order.begin(), order.end(), [&](auto a, auto b) {
			if (requests[a].retainedLightPriority != requests[b].retainedLightPriority)
				return requests[a].retainedLightPriority > requests[b].retainedLightPriority;
			return StaticViewOrder(requests[a].view) < StaticViewOrder(requests[b].view);
		});
		std::uint64_t available = plan.effectiveBytes - plan.chargedBytes;
		for (const auto index : order) {
			if (retained[index]) continue;
			if (!plan.draining && requestedBytes && required[index] <= available) {
				available -= required[index]; plan.newReservationBytes += required[index];
				plan.admittedRequestIndices.push_back(index);
			} else plan.deferredRequestIndices.push_back(index);
		}
		plan.valid = true; return plan;
	}
	bool ReconcileStaticShadowAllocation(const StaticShadowAdmissionRequest& reservation,
		std::uint64_t actualDepthBytes, std::uint64_t actualAuxiliaryBytes,
		std::uint64_t chargedBytes, std::uint64_t effectiveBytes, std::string& reason)
	{
		reason.clear(); std::uint64_t reserved = 0, actual = 0;
		if (!ValidStaticRequest(reservation) || !actualDepthBytes
			|| !AddBytes(reservation.queriedDepthBytes, reservation.queriedAuxBytes, reserved)
			|| !AddBytes(actualDepthBytes, actualAuxiliaryBytes, actual)
			|| actualDepthBytes > reservation.queriedDepthBytes || actualAuxiliaryBytes > reservation.queriedAuxBytes
			|| chargedBytes < reserved || chargedBytes > effectiveBytes) {
			reason = "Actual allocation exceeds its reservation or effective physical bound."; return false;
		}
		return actual <= reserved && chargedBytes - reserved <= effectiveBytes - actual;
	}

	ShadowCachePublicationLedger::ShadowCachePublicationLedger(std::uint64_t session) : rendererSessionGeneration(session) {}
	bool ShadowCachePublicationLedger::Prepare(const ShadowRefreshCandidate& candidate)
	{
		if (!ValidCandidate(candidate, rendererSessionGeneration, false)) return false;
		Store(entries, Entry(candidate, ShadowCachePublicationState::Prepared), false);
		return true;
	}
	bool ShadowCachePublicationLedger::Record(const ShadowRefreshCandidate& candidate)
	{
		// Revoke all logical aliases before checking recording completeness. Even
		// rejected five-face work may overwrite a physical image when submitted.
		if (candidate.view.rendererSessionGeneration != rendererSessionGeneration) return false;
		for (auto& entry : entries)
			if (PhysicalOverlap(entry.view, candidate.view)) entry.status = ShadowCachePublicationState::Invalid;
		for (auto& owner : submittedPhysicalOwners)
			if (PhysicalOverlap(owner.view, candidate.view)) owner.status = ShadowCachePublicationState::Invalid;
		if (std::none_of(blockedPhysicalRegions.begin(), blockedPhysicalRegions.end(),
			[&](const auto& view) { return PhysicalOverlap(view, candidate.view); }))
			blockedPhysicalRegions.push_back(candidate.view);
		if (!ValidCandidate(candidate, rendererSessionGeneration, true))
		{
			// An incomplete newer writer is a tombstone: confirming an older pending
			// command later cannot erase that barrier and revive outdated contents.
			std::erase_if(pending, [&](const auto& older) { return PhysicalOverlap(older.view, candidate.view); });
			return false;
		}
		pending.push_back(candidate);
		Store(entries, Entry(candidate, ShadowCachePublicationState::Recorded), false);
		return true;
	}
	bool ShadowCachePublicationLedger::ConfirmSubmission(std::uint64_t commandSerial, std::uint64_t submitSerial, bool succeeded)
	{
		return ConfirmSubmissionBatch(std::span(&commandSerial, 1), submitSerial, succeeded);
	}
	bool ShadowCachePublicationLedger::HasRecordedCandidate(std::uint64_t commandSerial) const noexcept
	{
		return std::any_of(pending.begin(), pending.end(), [commandSerial](const auto& item) { return item.commandSerial == commandSerial; });
	}
	bool ShadowCachePublicationLedger::HasRecordedCandidate(const ShadowViewKey& view,
		std::uint64_t contentToken, std::uint64_t commandSerial) const noexcept
	{
		if (!contentToken || !commandSerial) return false;
		// Only the latest recorded physical writer can be copied. An older exact
		// key is not safe after a later writer recorded over that image region.
		for (auto item = pending.rbegin(); item != pending.rend(); ++item)
			if (PhysicalOverlap(item->view, view)) return item->view == view
				&& item->contentToken == contentToken && item->commandSerial == commandSerial
				&& ValidCandidate(*item, rendererSessionGeneration, true);
		return false;
	}
	bool ShadowCachePublicationLedger::ConfirmSubmissionBatch(std::span<const std::uint64_t> commands,
		std::uint64_t submitSerial, bool succeeded)
	{
		bool valid = !commands.empty() && succeeded && submitSerial != 0 && submitSerial > lastSubmissionSerial;
		for (std::size_t i = 0; i < commands.size(); ++i)
		{
			valid = valid && commands[i] != 0
				&& std::find(commands.begin(), commands.begin() + i, commands[i]) == commands.begin() + i
				&& std::any_of(pending.begin(), pending.end(), [&](const auto& item) { return item.commandSerial == commands[i]; });
		}
		if (!valid)
		{
			// Reject coverage as one unit before publishing any supplied command.
			for (auto command : commands) Discard(command);
			return false;
		}
		// GPU command order, rather than CPU recording order, determines the final
		// physical owner. Within each command, its recording order is preserved.
		std::vector<ShadowRefreshCandidate> confirmed;
		for (auto command : commands)
			for (const auto& item : pending)
				if (item.commandSerial == command) confirmed.push_back(item);
		std::erase_if(pending, [&](const auto& item) { return std::find(commands.begin(), commands.end(), item.commandSerial) != commands.end(); });
		for (const auto& item : confirmed)
		{
			auto entry = Entry(item, ShadowCachePublicationState::Submitted);
			entry.submitSerial = submitSerial;
			Store(submittedPhysicalOwners, entry, true);
			Store(entries, std::move(entry), false);
			if (std::none_of(pending.begin(), pending.end(),
				[&](const auto& writer) { return PhysicalOverlap(writer.view, item.view); }))
				std::erase_if(blockedPhysicalRegions,
					[&](const auto& view) { return PhysicalOverlap(view, item.view); });
		}
		lastSubmissionSerial = submitSerial;
		return true;
	}
	void ShadowCachePublicationLedger::Discard(std::uint64_t commandSerial)
	{
		for (const auto& item : pending)
		{
			if (item.commandSerial != commandSerial) continue;
			for (auto& entry : entries)
				if (PhysicalOverlap(entry.view, item.view)) entry.status = ShadowCachePublicationState::Invalid;
			for (auto& owner : submittedPhysicalOwners)
				if (PhysicalOverlap(owner.view, item.view)) owner.status = ShadowCachePublicationState::Invalid;
		}
		// Remove the aborted command and every earlier overlapping writer. A newer
		// complete recording may establish fresh ownership; an older one must never
		// clear the aborted writer's barrier on a delayed successful confirmation.
		std::vector<ShadowRefreshCandidate> retained;
		retained.reserve(pending.size());
		for (std::size_t i = 0; i < pending.size(); ++i)
		{
			bool discard = pending[i].commandSerial == commandSerial;
			for (std::size_t j = i; !discard && j < pending.size(); ++j)
				discard = pending[j].commandSerial == commandSerial && PhysicalOverlap(pending[i].view, pending[j].view);
			if (!discard) retained.push_back(pending[i]);
		}
		pending.swap(retained);
	}
	void ShadowCachePublicationLedger::Complete(std::uint64_t submitSerial, std::uint64_t completionSerial)
	{
		if (submitSerial == 0 || completionSerial < submitSerial) return;
		const auto advance = [&](auto& rows) {
			for (auto& entry : rows)
				if (entry.submitSerial == submitSerial && Submitted(entry))
				{
					entry.completionSerial = std::max(entry.completionSerial, completionSerial);
					entry.status = ShadowCachePublicationState::Complete;
				}
		};
		advance(submittedPhysicalOwners);
		advance(entries);
	}
	bool ShadowCachePublicationLedger::CanReuse(const ShadowViewKey& view, std::uint64_t contentToken) const
	{
		if (!ValidView(view, rendererSessionGeneration) || contentToken == 0
			|| std::any_of(blockedPhysicalRegions.begin(), blockedPhysicalRegions.end(),
				[&](const auto& region) { return PhysicalOverlap(region, view); }))
			return false;
		const auto logical = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
			return entry.view == view && entry.contentToken == contentToken && Submitted(entry) && ValidMembership(entry.membership);
		});
		if (logical == entries.end()) return false;
		return std::any_of(submittedPhysicalOwners.begin(), submittedPhysicalOwners.end(), [&](const auto& owner) {
			return PhysicalOverlap(owner.view, view) && owner.contentToken == contentToken && Submitted(owner)
				&& owner.submitSerial == logical->submitSerial && SameContentInputs(owner.view, logical->view)
				&& ExactShadowCasterMembershipEqual(owner.membership, logical->membership);
		});
	}
	bool ShadowCachePublicationLedger::AliasSubmittedContent(const ShadowViewKey& view, std::uint64_t token,
		const ShadowCasterMembership& membership)
	{
		if (!ValidView(view, rendererSessionGeneration) || token == 0 || !ValidMembership(membership)
			|| std::any_of(blockedPhysicalRegions.begin(), blockedPhysicalRegions.end(),
				[&](const auto& region) { return PhysicalOverlap(region, view); })) return false;
		const auto owner = std::find_if(submittedPhysicalOwners.begin(), submittedPhysicalOwners.end(), [&](const auto& entry) {
			return Submitted(entry) && entry.contentToken == token && SameContentInputs(entry.view, view)
				&& ExactShadowCasterMembershipEqual(entry.membership, membership);
		});
		if (owner == submittedPhysicalOwners.end()) return false;
		// Copy the actual submission/completion receipt without recording a write.
		// A later physical overwrite revokes this qualified alias with every owner.
		auto alias = *owner; alias.view = view;
		Store(entries, std::move(alias), false);
		return true;
	}
	std::optional<ShadowCachePublicationEntry> ShadowCachePublicationLedger::FindSubmittedPhysicalOwner(const ShadowViewKey& view) const
	{
		const auto owner = std::find_if(submittedPhysicalOwners.begin(), submittedPhysicalOwners.end(), [&](const auto& entry) {
			return PhysicalOverlap(entry.view, view) && Submitted(entry);
		});
		return owner == submittedPhysicalOwners.end() ? std::nullopt : std::optional<ShadowCachePublicationEntry>(*owner);
	}
	std::optional<ShadowCachePublicationEntry> ShadowCachePublicationLedger::Find(const ShadowViewKey& view) const
	{
		for (auto it = pending.rbegin(); it != pending.rend(); ++it)
			if (it->view == view) return Entry(*it, ShadowCachePublicationState::Recorded);
		for (auto it = entries.rbegin(); it != entries.rend(); ++it)
			if (it->view == view) return *it;
		return std::nullopt;
	}
	void ShadowCachePublicationLedger::InvalidatePhysicalRegion(const ShadowViewKey& view) noexcept
	{
		// Match the same native overlap relation used by recording and submission.
		// Erasure only moves or destroys existing values: retirement cannot allocate,
		// reset the accepted submit clock, or revoke another image's pending writer.
		std::erase_if(entries, [&](const auto& entry) { return PhysicalOverlap(entry.view, view); });
		std::erase_if(submittedPhysicalOwners, [&](const auto& entry) { return PhysicalOverlap(entry.view, view); });
		std::erase_if(pending, [&](const auto& candidate) { return PhysicalOverlap(candidate.view, view); });
		std::erase_if(blockedPhysicalRegions, [&](const auto& region) { return PhysicalOverlap(region, view); });
	}
	void ShadowCachePublicationLedger::Reset(std::uint64_t session)
	{
		rendererSessionGeneration = session;
		entries.clear(); submittedPhysicalOwners.clear(); pending.clear(); blockedPhysicalRegions.clear();
		lastSubmissionSerial = 0;
	}
	ShadowBatchPlan BuildShadowBatchPlan(const ShadowViewKey& view,
		std::span<const ShadowBatchMember> members, const ShadowBatchLimits& limits)
	{
		// Pure worker-safe planning owns copied values only. It never suppresses a
		// native singleton: the caller still must publish and validate the full
		// shadow-specific stream at its existing protected recording boundary.
		ShadowBatchPlan plan; plan.view = view;
		const auto fail = [&](ShadowBatchFailure reason) {
			plan.coverageComplete = false; plan.failure = reason;
			plan.groups.clear(); plan.singletonIndices = plan.allSingletonIndices;
		};
		if (members.size() > std::numeric_limits<std::uint32_t>::max())
		{ fail(ShadowBatchFailure::Capacity); return plan; }
		plan.allSingletonIndices.reserve(members.size());
		for (std::uint32_t i = 0; i < members.size(); ++i) plan.allSingletonIndices.push_back(i);
		plan.singletonIndices = plan.allSingletonIndices;
		if (!ValidView(view, view.rendererSessionGeneration)
			|| static_cast<std::uint8_t>(view.viewType) > 1)
		{ fail(ShadowBatchFailure::InvalidView); return plan; }
		if (!limits.instanceCapacity || !limits.maxInstancesPerGroup || !limits.maxGroups
			|| !limits.maxTaskGroupsX || !limits.maxTaskGroupsTotal)
		{ fail(ShadowBatchFailure::InvalidLimits); return plan; }
		if (limits.firstInstance > std::numeric_limits<std::uint32_t>::max() - limits.instanceCapacity)
		{ fail(ShadowBatchFailure::Capacity); return plan; }
		using Contribution = std::tuple<UUID, std::uint64_t, std::uint64_t>;
		std::set<Contribution> contributions;
		// Ordered exact keys avoid digest-only authorization. Each bucket retains
		// original member order; bucket recording order follows its first member.
		std::map<ShadowBatchCompatibilityKey, std::vector<std::uint32_t>> buckets;
		std::vector<std::uint32_t> singles;
		for (std::uint32_t i = 0; i < members.size(); ++i)
		{
			const auto& member = members[i]; const auto& key = member.key;
			if (member.objectId == EMPTY_UUID || !member.lifetimeGeneration || !member.meshletCount
				|| member.firstMeshlet > std::numeric_limits<std::uint64_t>::max() - member.meshletCount
				|| member.firstCluster > std::numeric_limits<std::uint64_t>::max() - member.clusterCount
				|| member.payloadOffset > std::numeric_limits<std::uint64_t>::max() - member.payloadSize)
			{ fail(ShadowBatchFailure::InvalidMember); return plan; }
			if (!contributions.emplace(member.objectId, member.lifetimeGeneration, member.segmentId).second)
			{ fail(ShadowBatchFailure::DuplicateContribution); return plan; }
			bool completeKey = key.meshId != EMPTY_UUID && key.materialId != EMPTY_UUID && key.shaderId != EMPTY_UUID
				&& key.pipeline && key.layout && key.pipelineGeneration && key.layoutGeneration
				&& key.meshGeneration && key.materialGeneration && key.shaderGeneration && key.residencyGeneration
				&& key.dispatchMeshletCount && member.meshletCount == key.dispatchMeshletCount
				&& key.pushRangeCount && key.pushRangeCount <= key.pushRanges.size();
			for (std::size_t set = 0; set < key.descriptors.size(); ++set)
				completeKey = completeKey && key.descriptors[set] && key.descriptorGenerations[set];
			for (std::size_t range = 0; completeKey && range < key.pushRangeCount; ++range)
			{
				const auto& push = key.pushRanges[range];
				completeKey = push[0] && push[2] && push[1] % 4 == 0 && push[2] % 4 == 0
					&& push[1] <= std::numeric_limits<std::uint32_t>::max() - push[2];
			}
			if (!completeKey || !member.knownDefaultTaskAbi || !member.supportedRoute
				|| !member.deformationBridgeComplete || member.conservativeDeformation)
			{ singles.push_back(i); continue; }
			buckets[key].push_back(i);
		}
		std::vector<const std::vector<std::uint32_t>*> ordered;
		ordered.reserve(buckets.size());
		for (const auto& [key, indices] : buckets) ordered.push_back(&indices);
		std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) { return a->front() < b->front(); });
		std::uint64_t uploaded = 0;
		for (const auto* indices : ordered)
		{
			const auto& key = members[indices->front()].key;
			const std::uint64_t tasksPerInstance = (std::uint64_t(key.dispatchMeshletCount) + 31U) / 32U;
			const auto chunkLimit = std::min({std::uint64_t(limits.maxInstancesPerGroup),
				std::uint64_t(limits.maxTaskGroupsX) / tasksPerInstance,
				std::uint64_t(limits.maxTaskGroupsTotal) / tasksPerInstance});
			if (chunkLimit < 2) { singles.insert(singles.end(), indices->begin(), indices->end()); continue; }
			for (std::size_t begin = 0; begin < indices->size();)
			{
				const auto count = std::min<std::uint64_t>(chunkLimit, indices->size() - begin);
				if (count < 2) { singles.push_back((*indices)[begin]); ++begin; continue; }
				if (plan.groups.size() >= limits.maxGroups || count > limits.instanceCapacity - uploaded)
				{ fail(ShadowBatchFailure::Capacity); return plan; }
				ShadowBatchGroup group; group.key = key; group.representativeIndex = (*indices)[begin];
				group.firstInstance = static_cast<std::uint32_t>(limits.firstInstance + uploaded);
				group.instanceCount = static_cast<std::uint32_t>(count);
				group.taskGroupsX = static_cast<std::uint32_t>(tasksPerInstance * count);
				group.memberIndices.assign(indices->begin() + begin, indices->begin() + begin + static_cast<std::size_t>(count));
				plan.groups.push_back(std::move(group)); uploaded += count; begin += static_cast<std::size_t>(count);
			}
		}
		std::sort(singles.begin(), singles.end()); plan.singletonIndices = std::move(singles);
		plan.coverageComplete = true; return plan;
	}
}
