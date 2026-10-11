#include "../TestHarness.h"
#include "EditorAutomationRegistry.h"
#include "Tests/Support/RendererEfficiency/Editor/EditorRendererEfficiencyValidation.h"
#include "Tests/Support/RendererEfficiency/Editor/RendererEfficiencyEvidenceExport.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyArtifacts.h"
#include "Providers/EditorDiagnosticsAutomationProvider.h"
#include "Editor/TerraEngine-Editor/Core/EditorSchedulerDeclaration.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace TerraEngine::EditorTests
{
	using namespace TerraEngine::Editor::Automation;
	using TerraEngine::Renderer::RendererEfficiencyOptions;

	namespace
	{
		constexpr std::array<const char*, 6> kEfficiencyTools = {
			"renderer.efficiency.stats", "renderer.efficiency.validation.set",
			"renderer.efficiency.validation.reset", "renderer.efficiency.validation.start",
			"renderer.efficiency.validation.status", "renderer.efficiency.validation.stop"};

		bool IsReadTool(std::string_view name)
		{
			return name == kEfficiencyTools[0] || name == kEfficiencyTools[4];
		}

		// Each fixture creates and deletes only its own unique temporary directory.
		// The ownership parser receives the actual root rather than a caller's flag.
		class OwnedProjectFixture
		{
		public:
			OwnedProjectFixture()
			{
				root = std::filesystem::temp_directory_path() / ("TerraRendererEfficiency-"
					+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
				std::filesystem::create_directories(root);
			}
			~OwnedProjectFixture()
			{
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
			}
			void WriteMarker(const std::filesystem::path& declaredRoot, std::string hash = "fixture-hash") const
			{
				const nlohmann::json marker = {{"schemaVersion", 1},
					{"kind", "RendererEfficiencyValidationProject"},
					{"projectRoot", std::filesystem::weakly_canonical(declaredRoot).generic_string()},
					{"fixtureId", "shadow-invalidation"}, {"fixtureHash", std::move(hash)}};
				std::ofstream output(root / "renderer-efficiency-project.json", std::ios::binary);
				output << marker.dump(2);
			}
			std::filesystem::path root;
		};

		AutomationRequestContext AvailableRequest()
		{
			AutomationRequestContext request;
			request.state.projectAvailable = true;
			request.state.sceneAvailable = true;
			request.state.rendererAvailable = true;
			request.grantedPermission = AutomationPermission::ProjectWrite;
			return request;
		}
	}

	void RegisterRendererEfficiencyAutomationTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.Editor.Export.ReusesOnlyExactCompletedColorBytes", [](TestContext& context) {
			using namespace TerraEngine::Renderer;
			// These CPU fixtures test the real serializer, not physical GPU acceptance.
			OwnedProjectFixture project; const std::string hash(64, 'a'); project.WriteMarker(project.root, hash);
			RendererEfficiencyEvidenceExportContext capture; capture.projectRoot = project.root; capture.fixtureId = "shadow-invalidation"; capture.fixtureHash = hash;
			capture.operationId = TerraEngine::ToHexString(TerraEngine::GenerateUUID()); capture.fixtureStepId = "shadow-invalidation-00";
			capture.outputDirectory = project.root / "renderer-efficiency-evidence" / capture.operationId / "step0";
			capture.runId = "encoding-unit"; capture.mode = "Reference"; capture.seed = 65001; capture.settingsGeneration = 2; capture.mutationGeneration = 3; capture.rendererSessionGeneration = 4;
			auto color = std::make_shared<RendererEfficiencyFrameColor>(); color->width = 2; color->height = 1; color->sourceFormat = 97;
			color->bytes = {0,56,0,56,0,56,0,60,0,56,0,56,0,56,0,60}; // Two full binary16 RGBA pixels.
			const auto pointLight = TerraEngine::GenerateUUID(); std::vector<RendererEfficiencyEvidence> rows;
			for(unsigned index = 0; index < 9; ++index) {
				RendererEfficiencyEvidence row; row.status = RendererEfficiencyEvidenceStatus::Valid; row.resolution = {1,1}; row.frameSerial = 5; row.commandSerial = 6; row.submitSerial = row.completionSerial = 7;
				row.viewType = RenderViewType::Scene; row.viewId = 1; row.lightId = index < 6 ? pointLight : TerraEngine::GenerateUUID(); row.faceIndex = index < 6 ? index : 0; row.contentOwner = "unit-owner-" + std::to_string(index);
				row.effectiveOptions.settingsGeneration = 2; row.effectiveOptions.rendererSessionGeneration = 4; row.fixtureCorrelation = {capture.operationId,capture.fixtureStepId,0,3,2,4};
				for(auto* counter : {&row.eligibleCasterSegments,&row.submittedCasterSegments,&row.actualDrawSubmissions}) { counter->status = RendererEfficiencyEvidenceStatus::Valid; counter->value = 0; counter->reason.clear(); }
				auto native = std::make_shared<RendererEfficiencyArtifacts>(); native->shadowKey.rendererSessionGeneration = 4; native->shadowKey.viewType = row.viewType; native->shadowKey.viewId = 1; native->shadowKey.lightId = row.lightId; native->shadowKey.faceIndex = row.faceIndex;
				native->shadowKey.resourceKind = index < 6 ? ShadowResourceKind::PointCube : ShadowResourceKind::SpotAtlas; native->shadowKey.allocationGeneration = 1; native->shadowKey.extent = {1,1}; native->shadowKey.depthFormat = native->depthSourceFormat = 126; native->contentToken = index + 1;
				const auto bits = std::bit_cast<std::uint32_t>(0.5F); for(unsigned byte = 0; byte < 4; ++byte) native->depthF32LE.push_back(static_cast<std::uint8_t>(bits >> (byte * 8))); native->coverageU8 = {1};
				// Separate immutable owners with identical bytes must also hit; pointer
				// identity alone is neither required nor sufficient for this cache.
				native->frameColor = std::make_shared<RendererEfficiencyFrameColor>(*color); row.artifacts = native; rows.push_back(std::move(row));
			}
			RendererEfficiencyEvidenceExportResult result; std::string error;
			context.Expect(ExportRendererEfficiencyEvidence(capture, rows, result, error), "Complete nine-row native-shaped bundle must serialize: " + error);
			context.ExpectEqual(std::uint64_t{1}, result.colorEncodingCount, "Nine identical completed HDR color rows must perform one actual conversion/PNG encode.");
			context.Expect(result.frames.size() == 9 && result.artifacts.size() == 36, "Memoization must retain every fresh frame and all artifact references.");
			const auto image = result.frames[0].at("artifacts").at(2);
			std::ifstream imageFile(project.root / image.at("path").get<std::string>(), std::ios::binary);
			const std::vector<char> png((std::istreambuf_iterator<char>(imageFile)), {});
			std::set<std::string> counterPaths;
			for(std::size_t index = 0; index < rows.size(); ++index) {
				const auto& artifacts = result.frames[index].at("artifacts"); context.Expect(artifacts.at(2) == image, "Exact color bytes must reference the same SHA-qualified PNG artifact.");
				std::ifstream sameImage(project.root / artifacts.at(2).at("path").get<std::string>(), std::ios::binary); context.Expect(std::vector<char>((std::istreambuf_iterator<char>(sameImage)), {}) == png, "Every PNG reference must resolve to the same actual bytes.");
				const auto path = artifacts.at(3).at("path").get<std::string>(); counterPaths.insert(path); std::ifstream input(project.root / path); const auto counters = nlohmann::json::parse(input);
				context.Expect(counters.at("operationId") == capture.operationId && counters.at("qualifiedShadowViewKey").at("faceIndex") == rows[index].faceIndex && counters.at("readbackProvenance").at("completionSerial") == "7" && counters.at("sourceContentOwner").at("nativeOwner") == rows[index].contentOwner, "Fresh counter artifacts must retain each current row's native receipt and owner identity.");
			}
			context.ExpectEqual(std::size_t{9}, counterPaths.size(), "Same PNG must not collapse distinct face/owner counter artifacts.");
			context.Expect(ExportRendererEfficiencyEvidence(capture, rows, result, error) && result.colorEncodingCount == 1, "A separate export job must encode its own current bytes instead of consulting a persistent cache.");
			const auto miss = [&](std::shared_ptr<RendererEfficiencyFrameColor> changed, std::string_view reason) {
				auto current = rows; auto native = std::make_shared<RendererEfficiencyArtifacts>(*current.back().artifacts); native->frameColor = std::move(changed); current.back().artifacts = native;
				RendererEfficiencyEvidenceExportResult actual; context.Expect(ExportRendererEfficiencyEvidence(capture, current, actual, error), "Valid cache-miss bundle must serialize: " + error); context.ExpectEqual(std::uint64_t{2}, actual.colorEncodingCount, reason);
			};
			auto changed = std::make_shared<RendererEfficiencyFrameColor>(*color); changed->bytes[1] = 52; miss(changed, "Any actual HDR raw-byte change must encode again.");
			changed = std::make_shared<RendererEfficiencyFrameColor>(*color); changed->width = 1; changed->height = 2; miss(changed, "Equal byte counts with changed native extents must encode again.");
			// Equal RGBA8 source bytes interpreted in different channel order are distinct inputs.
			auto rgba = std::make_shared<RendererEfficiencyFrameColor>(); rgba->width = rgba->height = 1; rgba->sourceFormat = 37; rgba->bytes = {10,20,30,255};
			auto formatRows = rows; for(auto& row : formatRows) { auto native = std::make_shared<RendererEfficiencyArtifacts>(*row.artifacts); native->frameColor = rgba; row.artifacts = native; }
			auto bgra = std::make_shared<RendererEfficiencyFrameColor>(*rgba); bgra->sourceFormat = 44; auto native = std::make_shared<RendererEfficiencyArtifacts>(*formatRows.back().artifacts); native->frameColor = bgra; formatRows.back().artifacts = native;
			context.Expect(ExportRendererEfficiencyEvidence(capture, formatRows, result, error) && result.colorEncodingCount == 2, "Changed source format with identical raw bytes must not hit the cache.");
			for(bool fault : {false,true}) {
				auto invalid = rows; if(fault) invalid.back().status = RendererEfficiencyEvidenceStatus::Unavailable; else invalid.erase(invalid.begin());
				RendererEfficiencyEvidenceExportResult unchanged; unchanged.frames = {"sentinel"}; unchanged.colorEncodingCount = 99;
				context.Expect(!ExportRendererEfficiencyEvidence(capture, invalid, unchanged, error), "Incomplete cube and unavailable fault rows must retain strict bundle rejection.");
				context.Expect(unchanged.frames == nlohmann::json::array({"sentinel"}) && unchanged.colorEncodingCount == 99, "Rejected bundles must leave all caller output unchanged.");
			}
		});
		registry.Add("Renderer.Efficiency.Editor.Parser.AcceptsExactModesAndBudgetBounds", [](TestContext& context) {
			const RendererEfficiencyOptions base;
			RendererEfficiencyOptions parsed;
			std::string error;
			const nlohmann::json reference = {{"shadowReuse", "AlwaysRefresh"}, {"lightingUploads", "Full"},
				{"lightingDepth", "CompleteScan"}, {"shadowSubmission", "Singleton"},
				{"validationPreparation", "Serial"}, {"staticShadowCacheBudgetBytes", 0}};
			context.Expect(ParseRendererEfficiencyOptions(reference, base, parsed, error), "Every exact reference mode and signed zero budget must parse.");
			context.ExpectEqual(parsed.staticShadowCacheBudgetBytes, std::uint64_t{0}, "Zero must disable static admission without clamping.");
			context.Expect(parsed.shadowReuse == TerraEngine::Renderer::RendererEfficiencyShadowReuse::AlwaysRefresh
				&& parsed.lightingUploads == TerraEngine::Renderer::RendererEfficiencyLightingUploads::Full
				&& parsed.lightingDepth == TerraEngine::Renderer::RendererEfficiencyLightingDepth::CompleteScan
				&& parsed.shadowSubmission == TerraEngine::Renderer::RendererEfficiencyShadowSubmission::Singleton
				&& parsed.validationPreparation == TerraEngine::Renderer::RendererEfficiencyPreparation::Serial,
				"Parsing must assign every requested mode, not merely accept its spelling.");
			const nlohmann::json defaults = {{"shadowReuse", "Enabled"}, {"lightingUploads", "Dirty"},
				{"lightingDepth", "Cooperative"}, {"shadowSubmission", "Grouped"},
				{"validationPreparation", "Concurrent"}, {"staticShadowCacheBudgetBytes", std::uint64_t{1073741824}}};
			context.Expect(ParseRendererEfficiencyOptions(defaults, base, parsed, error), "All default modes and the inclusive unsigned upper bound must parse.");
			context.ExpectEqual(parsed.staticShadowCacheBudgetBytes, std::uint64_t{1073741824}, "The upper bound must be retained exactly.");
			context.Expect(ParseRendererEfficiencyOptions(nlohmann::json::object(), base, parsed, error), "An empty options request preserves the supplied base options.");
			context.ExpectEqual(parsed.staticShadowCacheBudgetBytes, base.staticShadowCacheBudgetBytes, "Unspecified values must come from the supplied base.");
		});

		registry.Add("Renderer.Efficiency.Editor.Parser.RejectsWholeRequestAtomically", [](TestContext& context) {
			RendererEfficiencyOptions base;
			base.staticShadowCacheBudgetBytes = 512;
			const std::array<nlohmann::json, 13> invalid = {
				nlohmann::json{{"staticShadowCacheBudgetBytes", -1}},
				nlohmann::json{{"staticShadowCacheBudgetBytes", std::uint64_t{1073741825}}},
				nlohmann::json{{"staticShadowCacheBudgetBytes", std::numeric_limits<std::uint64_t>::max()}},
				nlohmann::json{{"staticShadowCacheBudgetBytes", 1.5}},
				nlohmann::json{{"staticShadowCacheBudgetBytes", true}},
				nlohmann::json{{"staticShadowCacheBudgetBytes", "64"}},
				nlohmann::json{{"shadowReuse", "enabled"}},
				nlohmann::json{{"lightingUploads", "Unknown"}},
				nlohmann::json{{"lightingDepth", false}},
				nlohmann::json{{"shadowSubmission", 0}},
				nlohmann::json{{"validationPreparation", "Automatic"}},
				nlohmann::json{{"staticShadowCacheBudgetBytes", 0}, {"unknownOption", 1}},
				nlohmann::json::array()};
			for(const auto& request : invalid)
			{
				RendererEfficiencyOptions parsed = base;
				std::string error;
				context.Expect(!ParseRendererEfficiencyOptions(request, base, parsed, error), "Malformed options must fail: " + request.dump());
				context.Expect(!error.empty(), "Rejected requests need an actionable diagnostic.");
				context.ExpectEqual(parsed.staticShadowCacheBudgetBytes, base.staticShadowCacheBudgetBytes, "Rejection must preserve prior output, including valid fields earlier in the request.");
				context.Expect(parsed.shadowReuse == base.shadowReuse && parsed.lightingUploads == base.lightingUploads
					&& parsed.lightingDepth == base.lightingDepth && parsed.shadowSubmission == base.shadowSubmission
					&& parsed.validationPreparation == base.validationPreparation, "Rejected requests must leave every output mode unchanged.");
			}
		});

		registry.Add("Renderer.Efficiency.Editor.Ownership.RequiresMatchingCurrentProjectAndFixture", [](TestContext& context) {
			OwnedProjectFixture fixture;
			std::string error;
			context.Expect(!ValidateRendererEfficiencyOwnedProject(fixture.root, "shadow-invalidation", "fixture-hash", error), "An ordinary project without the materializer marker must be rejected.");
			fixture.WriteMarker(fixture.root);
			context.Expect(ValidateRendererEfficiencyOwnedProject(fixture.root, "shadow-invalidation", "fixture-hash", error), "A matching materializer receipt must permit an owned session.");
			context.Expect(!ValidateRendererEfficiencyOwnedProject(fixture.root, "lighting-dirty", "fixture-hash", error), "A different fixture cannot borrow ownership.");
			context.Expect(!ValidateRendererEfficiencyOwnedProject(fixture.root, "shadow-invalidation", "other-hash", error), "A modified fixture hash must invalidate ownership.");
			fixture.WriteMarker(fixture.root / ".." / "outside-project");
			context.Expect(!ValidateRendererEfficiencyOwnedProject(fixture.root, "shadow-invalidation", "fixture-hash", error), "A receipt naming a root outside the current project must be rejected.");
			std::ofstream(fixture.root / "renderer-efficiency-project.json", std::ios::trunc) << "{ malformed";
			context.Expect(!ValidateRendererEfficiencyOwnedProject(fixture.root, "shadow-invalidation", "fixture-hash", error), "Malformed markers must fail closed without throwing.");
		});

		registry.Add("Renderer.Efficiency.Editor.Session.OwnedLifecycleCannotInventGpuProgress", [](TestContext& context) {
			OwnedProjectFixture fixture;
			fixture.WriteMarker(fixture.root);
			EditorRendererEfficiencyValidation host;
			TerraEngine::Renderer::RendererEfficiencyOptionsSnapshot options;
			options.optionsSessionGeneration = 7; options.settingsGeneration = 12;
			options.options.staticShadowCacheBudgetBytes = 4096;
			std::string error;
			const nlohmann::json request = {{"fixtureId", "shadow-invalidation"}, {"fixtureHash", "fixture-hash"}, {"stage", "P1"}, {"seed", 17}};
			context.Expect(host.Start(fixture.root, 2, request, options, error), "A valid owned project can start the foundation lifecycle.");
			const auto started = host.Status();
			context.Expect(!host.Start(fixture.root, 2, request, options, error), "Concurrent sessions must conflict without replacing the prior token.");
			context.ExpectEqual(host.PriorToken().options.staticShadowCacheBudgetBytes, std::uint64_t{4096}, "Session restoration must retain the exact pre-session options.");
			TerraEngine::Renderer::RendererEfficiencyEvidence evidence;
			evidence.submitSerial = 99; evidence.completionSerial = 99;
			evidence.effectiveOptions = options;
			host.ObserveAtProtectedFrame(fixture.root, 2, evidence);
			const auto pending = host.Status();
			context.ExpectEqual(pending.value("completedFrames", 1ULL), 0ULL, "Unavailable evidence with serials must not advance GPU progress.");
			context.ExpectEqual(pending.value("measuredCompletedFrames", 1ULL), 0ULL, "No fixture driver means no accepted measured samples.");
			context.ExpectEqual(pending.value("status", std::string{}), std::string("Unavailable"), "Foundation lifecycle must honestly report absent GPU acceptance.");
			context.Expect(!host.Stop("wrong-operation", error), "An unrelated operation token cannot stop the session.");
			context.Expect(host.Stop(started["operationId"].get<std::string>(), error), "The owning operation can stop the session.");
			context.Expect(!host.Active(), "Stop must disable production immediately.");
			context.Expect(host.Start(fixture.root, 2, request, options, error), "A stopped session can be replaced deliberately.");
			host.ObserveAtProtectedFrame(fixture.root, 3, evidence);
			context.Expect(!host.Active() && host.Status().value("state", std::string{}) == "Failed", "Project generation changes invalidate an owned session.");
		});

		registry.Add("Renderer.Efficiency.Editor.Session.RendererEpochChangeSettlesAndAllowsFreshStart", [](TestContext& context) {
			OwnedProjectFixture fixture; fixture.WriteMarker(fixture.root);
			EditorRendererEfficiencyValidation host;
			TerraEngine::Renderer::RendererEfficiencyOptionsSnapshot options;
			options.optionsSessionGeneration = 7;
			const nlohmann::json request = {{"fixtureId", "shadow-invalidation"}, {"fixtureHash", "fixture-hash"}, {"stage", "P1"}};
			std::string error;
			context.Expect(host.Start(fixture.root, 2, request, options, error), "Initial owned session must start.");
			const auto oldOperation = host.Status()["operationId"].get<std::string>();
			TerraEngine::Renderer::RendererEfficiencyEvidence evidence;
			options.optionsSessionGeneration = 8; evidence.effectiveOptions = options;
			host.ObserveAtProtectedFrame(fixture.root, 2, evidence);
			context.Expect(!host.Active() && host.Status().value("state", std::string{}) == "Failed", "Recovery must terminate the old session even when project generation is unchanged.");
			context.Expect(!host.PriorToken().valid, "An old epoch token must not be offered for restoration.");
			context.Expect(host.Stop(oldOperation, error) && !host.Active(), "Stop must settle an already failed operation without reactivating it.");
			context.Expect(host.Start(fixture.root, 2, request, options, error), "Recovery failure must not block a fresh owned session.");
			context.Expect(host.PriorToken().valid && host.PriorToken().optionsSessionGeneration == 8, "Fresh admission must capture the new epoch token.");
		});

		registry.Add("Renderer.Efficiency.Editor.Session.InvalidStartLeavesHostUntouched", [](TestContext& context) {
			OwnedProjectFixture fixture; fixture.WriteMarker(fixture.root);
			EditorRendererEfficiencyValidation host;
			TerraEngine::Renderer::RendererEfficiencyOptionsSnapshot options;
			const nlohmann::json valid = {{"fixtureId", "shadow-invalidation"}, {"fixtureHash", "fixture-hash"}, {"stage", "P1"}};
			const std::array<nlohmann::json, 6> changes = {nlohmann::json{{"runId", 2}}, nlohmann::json{{"mode", "Compare"}},
				nlohmann::json{{"measuredFrames", 0}}, nlohmann::json{{"seed", -1}},
				nlohmann::json{{"fixtureManifestSha256", "mismatch"}}, nlohmann::json{{"faultInjection", "submitFailure"}}};
			for(const auto& change : changes)
			{
				auto request = valid; request.update(change); std::string error;
				const auto before = host.Status();
				context.Expect(!host.Start(fixture.root, 2, request, options, error), "Invalid start parameters must reject the entire admission.");
				context.ExpectEqual(host.Status(), before, "Rejected start must not partially replace operation identity, prior options or progress.");
			}
		});

		registry.Add("Renderer.Efficiency.Editor.Provider.DeclaresPermissionsAndProtectedBoundaries", [](TestContext& context) {
			// Provider metadata alone cannot protect the frame pump. Verify the
			// real editor system owns the same immutable reads and queued writes.
			const auto system = TerraEngine::Editor::MakeEditorSchedulerDeclaration();
			context.ExpectEqual(system.phase, std::string("PostFrame"), "Renderer validation observation belongs to the existing PostFrame boundary.");
			context.Expect(system.phaseKind == TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit
				&& system.boundaryClassification == TerraEngine::Scheduling::HardBoundaryClassification::Protected,
				"Renderer validation control must remain protected editor work.");
			for(const char* resource : {"RendererEfficiencyOptions", "RendererEfficiencyEvidence"})
				context.Expect(std::find(system.reads.begin(), system.reads.end(), resource) != system.reads.end(),
					"The actual editor declaration must read " + std::string(resource));
			context.Expect(std::find(system.writes.begin(), system.writes.end(), "RendererEfficiencyOptionsQueue") != system.writes.end(),
				"The actual editor declaration must own transient renderer options ingress writes.");
			EditorDiagnosticsAutomationProvider provider([](std::string_view) {
				return AutomationHandler{[](const AutomationRequestContext&) { return AutomationDispatchResult{true, ErrorCategory::Internal, {}, {}}; }};
			});
			const auto capabilities = provider.GetCapabilities();
			for(const char* name : kEfficiencyTools)
			{
				const auto found = std::find_if(capabilities.begin(), capabilities.end(), [name](const auto& entry) { return entry.descriptor.name == name; });
				context.Expect(found != capabilities.end(), "The actual diagnostics provider must advertise " + std::string(name));
				const auto& descriptor = found->descriptor;
				const bool read = IsReadTool(name);
				context.ExpectEqual(descriptor.permission, read ? AutomationPermission::Read : AutomationPermission::ProjectWrite, "Transient validation controls use project-write permission.");
				context.ExpectEqual(descriptor.approvalPolicy, AutomationApprovalPolicy::None, "Transient controls must not require fabricated approval tokens.");
				context.Expect(descriptor.availability.requiresRenderer, "Renderer evidence and controls require a renderer.");
				context.ExpectEqual(descriptor.execution.protectedCommit, !read, "Only mutation handlers commit at a protected boundary.");
				context.Expect(descriptor.execution.workerPreparation && !descriptor.execution.preparationReads.empty(), "Worker-safe preparation must name its immutable input resources.");
				context.Expect(!descriptor.execution.visibilityBoundary.empty(), "Discovery must expose the publication boundary.");
				context.Expect(read ? descriptor.execution.commitWrites.empty() : !descriptor.execution.commitWrites.empty(), "Commit resources must agree with read/mutation ownership.");
			}
		});

		registry.Add("Renderer.Efficiency.Editor.Provider.PermissionAndAvailabilityRejectBeforeHandler", [](TestContext& context) {
			std::size_t calls = 0;
			EditorAutomationRegistry tools;
			const auto provider = std::make_shared<EditorDiagnosticsAutomationProvider>([&calls](std::string_view) {
				return AutomationHandler{[&calls](const AutomationRequestContext&) { ++calls; return AutomationDispatchResult{true, ErrorCategory::Internal, {}, {}}; }};
			});
			context.Expect(tools.RegisterProvider(provider).registered, "Production diagnostics provider must register atomically.");
			for(const char* name : kEfficiencyTools)
			{
				context.Expect(tools.Find(name, 1, AvailableRequest().state).has_value(), "Tool must exist before exercising its policy.");
				auto request = AvailableRequest();
				request.grantedPermission = AutomationPermission::Read;
				const auto result = tools.Dispatch(name, 1, request);
				context.ExpectEqual(result.ok, IsReadTool(name), "Read permission must never invoke a mutating validation handler.");
				if(!IsReadTool(name)) context.ExpectEqual(result.error, ErrorCategory::Permission, "Rejected controls use the standard permission category.");
				request = AvailableRequest();
				request.state.rendererAvailable = false;
				const auto before = calls;
				const auto unavailable = tools.Dispatch(name, 1, request);
				context.Expect(!unavailable.ok && unavailable.error == ErrorCategory::Unavailable && !unavailable.message.empty(), "Missing renderer must produce a descriptive availability result.");
				context.ExpectEqual(calls, before, "Unavailable requests must not reach a handler.");
			}
			context.ExpectEqual(calls, std::size_t{2}, "Only the two read tools may have reached the stub under read permission.");
		});
	}
}
