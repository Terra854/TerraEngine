// Frozen public skinned-cook recipes around complete physical page budgets.
#include "TestHarness.h"
#include "MeshCompiler.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "Resource/VirtualGeometryCodec.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace TerraEngine::EditorTests {
namespace {
	struct Fixture {
		std::filesystem::path root;
		Fixture() {
			static std::atomic<unsigned> serial{0};
			root = std::filesystem::temp_directory_path() / ("TerraDeformBudget-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(serial.fetch_add(1)));
			if(!std::filesystem::create_directory(root)) throw std::runtime_error("Cannot reserve deformation fixture root.");
		}
		~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
	};
	SkinnedMeshCookInput Grid(unsigned height, unsigned morphs, unsigned joints) {
		// Freeze finite-discovery recipes: same positions/topology across palette
		// controls, so only the actual palette records shift the page byte budget.
		constexpr unsigned width = 50U;
		SkinnedMeshCookInput input;
		for(unsigned y = 0; y <= height; ++y) for(unsigned x = 0; x <= width; ++x) {
			const float fx = static_cast<float>(x) / width, fy = static_cast<float>(y) / height;
			SkinnedMeshCookVertex vertex;
			vertex.baseVertex.position = {fx * 2.0F - 1.0F, fy * 2.0F - 1.0F, 0.05F * std::sin(fx * 12.0F) * std::cos(fy * 10.0F)};
			vertex.baseVertex.normal = {0.0F,0.0F,1.0F}; vertex.baseVertex.uv = {fx,fy}; vertex.influences = {{(y * (width + 1U) + x) % joints, 1.0F}};
			input.vertices.push_back(std::move(vertex));
		}
		for(unsigned y = 0; y < height; ++y) for(unsigned x = 0; x < width; ++x) {
			const unsigned a = y * (width + 1U) + x, b = a + 1U, c = a + width + 1U, d = c + 1U;
			input.indices.insert(input.indices.end(), {a,c,b,b,c,d});
		}
		for(unsigned index = 0; index < morphs; ++index) {
			SkinnedMeshMorphTarget morph; morph.stableTargetId = index + 1U;
			morph.positionDeltas.resize(input.vertices.size(), {0.0F,0.0F,0.01F}); input.morphTargets.push_back(std::move(morph));
		}
		return input;
	}
	SkinnedMeshCookSettings Settings(unsigned joints) {
		SkinnedMeshCookSettings settings; settings.sourceSkeletonJointCount = joints; settings.skeletonCompatibilityFingerprint = 0x534B454C45544F4EULL; return settings;
	}
	std::vector<std::uint8_t> Read(const std::filesystem::path& file) {
		std::ifstream input(file, std::ios::binary); return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}
	struct Allocation { std::uint32_t profile = 0; std::uint64_t largestBlob = 0; };
	Allocation Inspect(TestContext& context, const std::filesystem::path& output) {
		const auto bytes = Read(output); const auto validated = TerraEngine::Resource::ValidateTemfProductionFile(bytes, {});
		context.Expect(validated.IsComplete(), "A successful public cook must publish a fully validated production container.");
		Allocation result; result.profile = validated.directory.header.logicalPageSizeBytes;
		for(const auto& page : validated.directory.pages) context.Expect(page.attributedStoredBytes <= result.profile, "Every physical page must include directory and payload attribution within its profile.");
		for(const auto& descriptor : validated.directory.blocks) {
			if(descriptor.streamKind != static_cast<std::uint16_t>(TemfStreamKind::ProceduralUpdate) || descriptor.attributeMask != kTemfMetadataDeformableDemandedPageBlobs) continue;
			const auto decoded = TerraEngine::Resource::DecodeTemfProductionBlock({descriptor, std::span(bytes).subspan(static_cast<std::size_t>(descriptor.encodedPayloadOffset), static_cast<std::size_t>(descriptor.encodedBytes)), validated.directory.quantizationDomains, 0U}, {});
			context.Expect(decoded.IsComplete() && decoded.bytes.size() == descriptor.decodedBytes, "The indivisible demanded-page payload must decode through its actual production descriptor.");
			result.largestBlob = std::max(result.largestBlob, descriptor.decodedBytes);
		}
		context.Expect(result.largestBlob != 0U, "Boundary recipes must exercise actual demanded pages, not the direct-resident fallback."); return result;
	}
	constexpr std::uint64_t kCompletePageOverhead = sizeof(TemfPageDescriptor) + sizeof(TemfBlockDescriptor) + (kTemfDirectoryAlignment - 1U);
	Allocation Cook(TestContext& context, const std::filesystem::path& output, unsigned height, unsigned morphs, unsigned joints) {
		SkinnedMeshCookReport report;
		const bool compiled = CompileSkinnedMeshFromRaw("PageBoundary/Grid", output, Grid(height,morphs,joints), &report, Settings(joints));
		context.Expect(compiled, "The accepted recipe must allocate its complete page packet: " + report.diagnosticMessage);
		return Inspect(context, output);
	}
}
void RegisterDeformationPageBoundaryTests(TestRegistry& registry) {
	registry.Add("Editor.VirtualGeometry.DeformationPage.Below64KiBCompleteBudgetControl", [](TestContext& context) {
		Fixture fixture; const auto result = Cook(context, fixture.root / "below.temf", 57U,1U,80U);
		context.Expect(result.profile == kTemfPageSize64KiB && result.largestBlob == 65344U && result.largestBlob + kCompletePageOverhead <= result.profile,
			"Frozen below-boundary recipe must remain a real complete 64KiB allocation.");
	});
	registry.Add("Editor.VirtualGeometry.DeformationPage.Promotes64KiBForDescriptorOverhead", [](TestContext& context) {
		Fixture fixture; const auto result = Cook(context, fixture.root / "promoted.temf", 57U,1U,96U);
		context.Expect(result.largestBlob <= kTemfPageSize64KiB && result.largestBlob + kCompletePageOverhead > kTemfPageSize64KiB && result.profile == kTemfPageSize128KiB,
			"Payload bytes alone fit 64KiB; the complete packet must select the larger profile.");
	});
	registry.Add("Editor.VirtualGeometry.DeformationPage.Above64KiBPayloadControl", [](TestContext& context) {
		Fixture fixture; const auto result = Cook(context, fixture.root / "above.temf", 57U,1U,256U);
		context.Expect(result.largestBlob == 66048U && result.profile == kTemfPageSize128KiB && result.largestBlob + kCompletePageOverhead <= result.profile,
			"A plainly oversized 64KiB payload must retain the already-working promotion control.");
	});
	registry.Add("Editor.VirtualGeometry.DeformationPage.Below128KiBCompleteBudgetControl", [](TestContext& context) {
		Fixture fixture; const auto result = Cook(context, fixture.root / "maximum-control.temf", 41U,2U,160U);
		context.Expect(result.profile == kTemfPageSize128KiB && result.largestBlob > 130000U && result.largestBlob + kCompletePageOverhead <= result.profile,
			"The maximum-profile control must retain room for all required directory/alignment bytes.");
	});
	registry.Add("Editor.VirtualGeometry.DeformationPage.MaximumRejectionPreservesLastKnownGood", [](TestContext& context) {
		Fixture fixture; const auto output = fixture.root / "retained.temf"; (void)Cook(context, output,57U,1U,80U); const auto original = Read(output);
		SkinnedMeshCookReport rejected;
		// Pinned size-observer evidence for this public recipe: raw 131024,
		// complete packet 131215 (191 directory/alignment bytes), exceeding
		// the maximum 131072-byte profile. No observer is used by this test.
		const bool compiled = CompileSkinnedMeshFromRaw("PageBoundary/Maximum", output, Grid(41U,2U,224U), &rejected, Settings(224U));
		context.Expect(!compiled && !rejected.published && rejected.preservedLastKnownGood && Read(output) == original,
			"A rejected maximum-profile packet must leave the complete last-known-good generation byte identical.");
		context.Expect(rejected.failure == SkinnedMeshCookFailure::PayloadRangeOverflow && rejected.diagnosticCode == "SkinnedMeshPayloadRangeOverflow",
			"The complete over-maximum packet must reject at the typed range guard before assembly or publication.");
	});
}
}
