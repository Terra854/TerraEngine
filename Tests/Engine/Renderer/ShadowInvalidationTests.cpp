// ShadowInvalidationTests.cpp — immutable semantic shadow-content regressions.
#include "TestHarness.h"
#include "Renderer/ShadowWorkPlan.h"

#include <functional>
#include <limits>
#include <string>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace TerraEngine::Renderer;
		UUID Identity(std::uint8_t value) { UUID id{}; id[0] = value; return id; }
		ShadowCasterStamp KnownCaster()
		{
			// Known zero deformation is legitimate for a static bind-pose mesh; proof
			// bits, not nonzero guesses, establish availability of all captured inputs.
			ShadowCasterStamp stamp;
			stamp.objectId = Identity(1); stamp.lifetimeGeneration = 7; stamp.segmentId = 11;
			stamp.meshId = Identity(2); stamp.assetId = Identity(3); stamp.materialId = Identity(4);
			stamp.shaderId = Identity(5); stamp.textureId = Identity(6);
			stamp.meshRevision = 1; stamp.assetRevision = 2; stamp.materialRevision = 3;
			stamp.shaderRevision = 4; stamp.textureRevision = 5;
			stamp.meshletCount = 8; stamp.clusterCount = 2;
			stamp.pageId = 17; stamp.fallbackPageId = 18; stamp.pageContentRevision = 19;
			stamp.residencyGeneration = 20; stamp.payloadContentRevision = 21;
			stamp.payloadByteSize = 256; stamp.payloadSource = 1; stamp.emittedSource = 1;
			stamp.evidenceMask = kCompleteShadowCasterEvidence;
			return stamp;
		}
		ShadowCasterMembership Membership(std::initializer_list<ShadowCasterStamp> rows)
		{
			std::vector<ShadowCasterStampInput> inputs;
			for (const auto& row : rows) inputs.push_back({row, {}});
			return NormalizeShadowCasterMembership(inputs);
		}
	}

	void RegisterShadowInvalidationTests(TestRegistry& registry)
	{
		using namespace TerraEngine::Renderer;
		using Change = std::function<void(ShadowCasterStamp&)>;
		// Every sampled binding participates independently. The legacy first
		// texture fields deliberately stay unchanged throughout these cases.
		for (const auto& operation : {std::string("SecondPublication"), std::string("BindingIndex"), std::string("BindingOrder")})
			registry.Add("Renderer.Efficiency.ShadowInvalidation.TextureBindings" + operation, [operation](TestContext& context) {
				auto before = KnownCaster();
				before.texturePublications = {{Identity(6), 5U, 0U}, {Identity(7), 9U, 1U}};
				auto after = before;
				if (operation == "SecondPublication") ++after.texturePublications[1].publicationGeneration;
				else if (operation == "BindingIndex") after.texturePublications[1].bindingIndex = 2U;
				else std::swap(after.texturePublications[0], after.texturePublications[1]);
				context.Expect(before.textureId == after.textureId && before.textureRevision == after.textureRevision,
					"The first texture's compatibility identity must remain unchanged.");
				auto first = Membership({before}), second = Membership({after});
				context.Expect(first.reusable && second.reusable, "Both complete sampled binding lists must be valid.");
				context.Expect(first.digest != second.digest, "Every binding identity, publication and ordering must affect the rejection digest.");
				first.digest = second.digest = 123U;
				context.Expect(!ExactShadowCasterMembershipEqual(first, second), "Equal digests cannot hide unequal sampled bindings.");
			});
		registry.Add("Renderer.Efficiency.ShadowInvalidation.TextureBindingMissingPublication", [](TestContext& context) {
			auto caster = KnownCaster();
			caster.texturePublications = {{Identity(6), 5U, 0U}, {Identity(7), 0U, 1U}};
			context.Expect(!IsShadowCasterStampReusable(caster) && !Membership({caster}).reusable,
				"An unavailable second sampled publication must reject the whole caster proof.");
			caster.texturePublications[1] = {EMPTY_UUID, 9U, 1U};
			context.Expect(!IsShadowCasterStampReusable(caster), "Missing sampled texture identity also rejects proof.");
		});
		const std::vector<std::pair<std::string, Change>> changes = {
			{"ObjectIdentity", [](auto& s) { s.objectId[0]++; }},
			{"LifetimeGeneration", [](auto& s) { s.lifetimeGeneration++; }},
			{"SelectedSegment", [](auto& s) { s.segmentId++; }},
			{"MeshIdentity", [](auto& s) { s.meshId[0]++; }},
			{"AssetIdentity", [](auto& s) { s.assetId[0]++; }},
			{"MeshRevision", [](auto& s) { s.meshRevision++; }},
			{"AssetRevision", [](auto& s) { s.assetRevision++; }},
			{"TransformCell", [](auto& s) { s.worldPosition.cell[0]++; }},
			{"TransformOffset", [](auto& s) { s.worldPosition.offsetMetres[1] = 0.01; }},
			{"TransformRotation", [](auto& s) { s.rotation = {0, 0, 1, 0}; }},
			{"TransformScale", [](auto& s) { s.scale[2] = 2; }},
			{"ReferenceOriginCell", [](auto& s) { s.referenceOrigin.cell[2]++; }},
			{"ReferenceOriginOffset", [](auto& s) { s.referenceOrigin.offsetMetres[0] = 0.5; }},
			{"MaterialIdentity", [](auto& s) { s.materialId[0]++; }},
			{"ShaderIdentity", [](auto& s) { s.shaderId[0]++; }},
			{"TextureIdentity", [](auto& s) { s.textureId[0]++; }},
			{"MaterialRevision", [](auto& s) { s.materialRevision++; }},
			{"ShaderRevision", [](auto& s) { s.shaderRevision++; }},
			{"TextureRevision", [](auto& s) { s.textureRevision++; }},
			{"ShadowBehavior", [](auto& s) { s.shadowBehavior++; }},
			{"CutoutThreshold", [](auto& s) { s.materialShadowParameters[0] = 0.7; }},
			{"AnimationInstance", [](auto& s) { s.animationInstanceId++; }},
			{"StationarySkeletonPose", [](auto& s) { s.deformationPalette.push_back({}); s.deformationPalette.back()[3] = 0.5; }},
			{"StationaryMorph", [](auto& s) { s.morphWeights.push_back(0.5); }},
			{"PaletteGeneration", [](auto& s) { s.paletteGeneration++; }},
			{"DeformationPayload", [](auto& s) { s.deformationGeneration++; }},
			{"DeformationProgram", [](auto& s) { s.deformationProgramGeneration++; }},
			{"DeformationContentLow", [](auto& s) { s.deformationContentIdLow++; }},
			{"DeformationContentHigh", [](auto& s) { s.deformationContentIdHigh++; }},
			{"GeometryRoute", [](auto& s) { s.geometryRoute++; }},
			{"SelectedLod", [](auto& s) { s.selectedLod++; }},
			{"FirstMeshlet", [](auto& s) { s.firstMeshlet++; }},
			{"MeshletCount", [](auto& s) { s.meshletCount++; }},
			{"FirstCluster", [](auto& s) { s.firstCluster++; }},
			{"ClusterCount", [](auto& s) { s.clusterCount++; }},
			{"PageIdentity", [](auto& s) { s.pageId++; }},
			{"FallbackPageIdentity", [](auto& s) { s.fallbackPageId++; }},
			{"PageContentRevision", [](auto& s) { s.pageContentRevision++; }},
			{"ResidencyGeneration", [](auto& s) { s.residencyGeneration++; }},
			{"PayloadContentRevision", [](auto& s) { s.payloadContentRevision++; }},
			{"PayloadSemanticRange", [](auto& s) { s.payloadByteOffset++; }},
			{"PayloadSize", [](auto& s) { s.payloadByteSize++; }},
			{"SelectedPayloadProvenance", [](auto& s) { s.payloadSource++; }},
			{"EmittedPayloadProvenance", [](auto& s) { s.emittedSource++; }},
			{"StaticEligibility", [](auto& s) { s.staticEligible = true; }},
			{"StaticClassification", [](auto& s) { s.staticClassificationReason++; }}
		};
		for (const auto& [name, change] : changes)
			registry.Add("Renderer.Efficiency.ShadowInvalidation." + name, [change](TestContext& context) {
				const auto before = KnownCaster(); auto after = before; change(after);
				context.Expect(IsShadowCasterStampReusable(before), "The unchanged fully proven caster is reusable.");
				context.Expect(!ExactShadowCasterStampEqual(before, after), "A semantic content change must invalidate exact caster equality.");
			});
		for (unsigned bit = 0; bit < 9; ++bit)
			registry.Add("Renderer.Efficiency.ShadowInvalidation.MissingProof" + std::to_string(bit), [bit](TestContext& context) {
				auto unknown = KnownCaster(); unknown.evidenceMask &= ~(1ULL << bit);
				context.Expect(!IsShadowCasterStampReusable(unknown), "Missing any required immutable evidence conservatively rejects reuse.");
				context.Expect(!Membership({unknown}).reusable, "A missing-proof member disables reuse for the complete membership.");
			});
		registry.Add("Renderer.Efficiency.ShadowInvalidation.KnownZeroDeformation", [](TestContext& context) {
			const auto caster = KnownCaster();
			context.Expect(IsShadowCasterStampReusable(caster), "Proven static pose/morph zero must not force refresh every frame.");
			context.Expect(ExactShadowCasterStampEqual(caster, caster), "Unchanged exact semantic content remains equal.");
		});
		registry.Add("Renderer.Efficiency.ShadowInvalidation.NonFiniteTransform", [](TestContext& context) {
			auto caster = KnownCaster(); caster.worldPosition.offsetMetres[0] = std::numeric_limits<double>::quiet_NaN();
			context.Expect(!IsShadowCasterStampReusable(caster), "A nonfinite canonical transform cannot establish a reusable stamp.");
		});
		registry.Add("Renderer.Efficiency.ShadowInvalidation.CanonicalSignedZero", [](TestContext& context) {
			auto first = KnownCaster(), second = first; second.rotation[1] = -0.0;
			context.Expect(ExactShadowCasterStampEqual(first, second), "Signed zero does not change rotation semantics.");
			context.Expect(ExactShadowCasterMembershipEqual(Membership({first}), Membership({second})), "Canonical digest must not manufacture a signed-zero mismatch.");
		});
		registry.Add("Renderer.Efficiency.ShadowInvalidation.MembershipOrderIndependent", [](TestContext& context) {
			auto first = KnownCaster(), second = first; second.objectId = Identity(2);
			const auto unordered = Membership({second, first}), ordered = Membership({first, second});
			context.Expect(unordered.reusable && ordered.reusable, "Complete membership should be reusable.");
			context.Expect(unordered.rows == ordered.rows, "Stable identity/segment sorting removes input iteration order.");
		});
		registry.Add("Renderer.Efficiency.ShadowInvalidation.IntentionalSegmentsPreserved", [](TestContext& context) {
			auto first = KnownCaster(), second = first; second.segmentId++;
			const auto membership = Membership({first, second});
			context.Expect(membership.reusable && membership.rows.size() == 2, "Distinct selected segments on one object contribute independently.");
		});
		registry.Add("Renderer.Efficiency.ShadowInvalidation.DuplicateCoverageRejected", [](TestContext& context) {
			const auto caster = KnownCaster(); const auto membership = Membership({caster, caster});
			context.Expect(!membership.reusable && !membership.reason.empty(), "Duplicate identity/lifetime/segment must reject with a coverage reason.");
		});
		registry.Add("Renderer.Efficiency.ShadowInvalidation.MembershipRemoval", [](TestContext& context) {
			auto first = KnownCaster(), second = first; second.objectId = Identity(2);
			context.Expect(!ExactShadowCasterMembershipEqual(Membership({first, second}), Membership({first})), "Removed casters change membership even if common rows are unchanged.");
		});
		registry.Add("Renderer.Efficiency.ShadowInvalidation.DigestCollisionDoesNotAuthorizeReuse", [](TestContext& context) {
			auto first = KnownCaster(), second = first; second.morphWeights.push_back(0.5);
			auto before = Membership({first}), after = Membership({second}); before.digest = after.digest = 123;
			context.Expect(!ExactShadowCasterMembershipEqual(before, after), "Equal digests cannot authorize unequal semantic rows.");
		});
		for (bool palette : {false, true})
			registry.Add(std::string("Renderer.Efficiency.ShadowInvalidation.ExactCpuDeformation") + (palette ? "Palette" : "Morph"), [palette](TestContext& context) {
				auto before = KnownCaster(); before.deformationPalette.push_back({}); before.morphWeights.push_back(0.25);
				auto after = before;
				if (palette) after.deformationPalette[0][3] = 0.5; else after.morphWeights[0] = 0.5;
				auto first = Membership({before}), second = Membership({after});
				context.Expect(first.digest != second.digest, "Exact immutable CPU shader scalar changes participate in the fast rejection digest.");
				first.digest = second.digest = 123;
				context.Expect(!ExactShadowCasterMembershipEqual(first, second), "Even forced equal fingerprints cannot conceal changed exact CPU shader inputs.");
				if (palette) before.deformationPalette[0][0] = std::numeric_limits<double>::infinity();
				else before.morphWeights[0] = std::numeric_limits<double>::quiet_NaN();
				context.Expect(!IsShadowCasterStampReusable(before), "Nonfinite CPU deformation inputs cannot establish reusable proof.");
			});
		registry.Add("Renderer.Efficiency.ShadowInvalidation.EmptyMembershipReusable", [](TestContext& context) {
			const auto empty = Membership({});
			context.Expect(empty.reusable && empty.rows.empty(), "A proven empty region can avoid repeated clears after submission.");
		});
		registry.Add("Renderer.Efficiency.ShadowInvalidation.TransientFrameViewPacketSlotExcluded", [](TestContext& context) {
			ShadowCasterStampInput first{KnownCaster(), {1, 2, 3, 4}}, second = first;
			second.transient = {1000, 2000, 3000, 4000};
			// Exact drawn inputs remain unchanged when authority republishes its
			// observation counters. Keep those counters in diagnostic rows.
			second.semantic.poseGeneration = 100;
			second.semantic.morphGeneration = 200;
			const auto before = NormalizeShadowCasterMembership(std::span(&first, 1));
			const auto after = NormalizeShadowCasterMembership(std::span(&second, 1));
			context.Expect(before.rows == after.rows && before.digest == after.digest, "Frame/view publication serials and packet/upload selectors never change content identity.");
			context.Expect(ExactShadowCasterMembershipEqual(before, after), "Transient-only changes preserve reuse eligibility.");
			context.Expect(after.rows.front().poseGeneration == 100 && after.rows.front().morphGeneration == 200,
				"Normalization must preserve raw pose/morph observation counters for evidence.");
		});
	}
}
