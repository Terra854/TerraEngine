// These supplementary guards follow real stage definitions after relocation.
// Runtime ownership, allocation, and replay tests provide the behavioral evidence.
#include "TestHarness.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		struct SourceUnit { std::string path, text; };
		std::string ReadRequired(TestContext& context, const std::string& path)
		{
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / path, std::ios::binary);
			context.Expect(input.good(), "Every required source-contract file must be readable.");
			std::string text((std::istreambuf_iterator<char>(input)), {});
			context.Expect(!text.empty(), "An empty source read cannot satisfy a relocation guard.");
			return text;
		}
		std::string Region(TestContext& context, const std::string& text, std::string_view begin, std::string_view end)
		{
			const auto first = text.find(begin);
			const auto last = first == std::string::npos ? std::string::npos : text.find(end, first + begin.size());
			const bool found = first != std::string::npos && last != std::string::npos && last > first;
			context.Expect(found, "Both ordered region boundaries must exist in the same required source file.");
			return found ? text.substr(first, last - first) : std::string{};
		}
		bool Has(const std::string& text, std::string_view word) { return !text.empty() && text.find(word) != std::string::npos; }
		std::size_t Count(const std::string& text, std::string_view word)
		{
			std::size_t count = 0, cursor = 0;
			while((cursor = text.find(word, cursor)) != std::string::npos) { ++count; cursor += word.size(); }
			return count;
		}
		std::vector<SourceUnit> RendererClosure(TestContext& context)
		{
			std::vector<std::string> paths;
			const auto root = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			const auto renderer = root / "Engine/Renderer";
			const auto eligible = [](const auto& path) { const auto ext = path.extension(); return ext == ".cpp" || ext == ".h" || ext == ".inl"; };
			for(const auto& entry : std::filesystem::directory_iterator(renderer))
				if(entry.is_regular_file() && entry.path().filename().string().starts_with("RenderSystem") && eligible(entry.path()))
					paths.push_back(entry.path().lexically_relative(root).generic_string());
			for(const auto& entry : std::filesystem::recursive_directory_iterator(renderer / "Frame"))
				if(entry.is_regular_file() && eligible(entry.path())) paths.push_back(entry.path().lexically_relative(root).generic_string());
			std::sort(paths.begin(), paths.end());
			context.Expect(paths.size() > 2 && std::find(paths.begin(), paths.end(), "Engine/Renderer/RenderSystem.cpp") != paths.end()
				&& std::find(paths.begin(), paths.end(), "Engine/Renderer/RenderSystem.h") != paths.end(), "Inspect the complete coordinator/facade/Frame closure.");
			std::vector<SourceUnit> result;
			for(const auto& path : paths) result.push_back({path, ReadRequired(context, path)});
			return result;
		}
		bool Absent(const std::vector<SourceUnit>& closure, std::string_view word)
		{
			return !closure.empty() && std::all_of(closure.begin(), closure.end(), [&](const auto& unit) { return !unit.text.empty() && !Has(unit.text, word); });
		}
		bool Before(const std::string& text, std::string_view first, std::string_view second)
		{
			const auto a = text.find(first), b = text.find(second);
			return a != std::string::npos && b != std::string::npos && a < b;
		}
		std::string Compact(std::string text)
		{
			std::erase_if(text, [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; });
			return text;
		}
	}
	void RegisterRenderPublicationSourceGuardTests(TestRegistry& registry)
	{
		registry.Add("Renderer.PublicationSourceGuard.OwnedNoFailReceipt", [](TestContext& context) {
			const auto header = ReadRequired(context, "Engine/Renderer/Graph/RenderFramePublication.h");
			const auto sealing = ReadRequired(context, "Engine/Renderer/Frame/RenderFrameSealing.cpp");
			const auto receipt = Region(context, header, "struct RenderPublicationReceipt", "struct RenderFramePublication");
			const auto finalizer = Region(context, sealing, "RenderSystem::FinalizeFramePublication(", "RenderSystem::OrderFramePreparedViews(");
			context.Expect(!receipt.empty() && !Has(receipt, "std::string diagnostic") && Has(finalizer, "publicationReceipt.DiagnosticText()"),
				"Receipt ownership and its relocated logging consumer remain allocation independent.");
		});
		registry.Add("Renderer.PublicationSourceGuard.RemovesLegacySubmission", [](TestContext& context) {
			const auto closure = RendererClosure(context);
			const auto read = [&](const char* path) { return ReadRequired(context, path); };
			const auto coordinator = read("Engine/Renderer/RenderSystem.cpp");
			const auto publication = read("Engine/Renderer/Graph/RenderFramePublication.h");
			const auto publicationImplementation = read("Engine/Renderer/Graph/RenderFramePublication.cpp");
			const auto types = read("Engine/Renderer/RenderTypes.h");
			const auto sealing = read("Engine/Renderer/Frame/RenderFrameSealing.cpp");
			const auto sealingHeader = read("Engine/Renderer/Frame/RenderFrameSealing.h");
			const auto visible = read("Engine/Renderer/Frame/RenderViewPreparation.cpp");
			const auto snapshot = read("Engine/Renderer/Frame/RenderSnapshotPreparation.cpp");
			const auto capture = read("Engine/Renderer/Frame/RenderFrameCapture.cpp");
			const auto dynamic = read("Engine/Renderer/Frame/RenderDynamicPublication.cpp");
			const auto spatial = read("Engine/Renderer/Frame/RenderSpatialPreparation.cpp");
			const auto topology = read("Engine/Renderer/Frame/RenderTopologyPreparation.cpp");
			const auto tasks = read("Engine/Renderer/Frame/RenderPreparationTasks.h");
			std::vector<SourceUnit> vulkan;
			for(const char* path : {"VulkanContext.cpp", "VulkanContextSource.h", "VulkanBuiltinPasses.cpp", "VulkanEditorRendering.cpp", "VulkanFrameRendering.cpp", "VulkanFrameSubmission.cpp", "VulkanPostProcessing.cpp", "VulkanReadback.cpp"})
				vulkan.push_back({path, read((std::string("Engine/Renderer/RHI/Vulkan/") + path).c_str())});
			context.Expect(Absent(closure, "MaterializeCommands"), "No renderer facade or stage may reconstruct legacy command collections.");
			const auto shadow = Region(context, types, "struct ShadowDrawList", "};");
			context.Expect(!shadow.empty() && !Has(shadow, "RenderItemRecord") && !Has(shadow, "commands"), "Shadow views retain compact ordinals/selectors instead of copied full command arrays.");
			const auto viewBuild = Region(context, types, "struct RenderViewBuildState", "struct RenderOutputRequest");
			context.Expect(!viewBuild.empty() && !Has(viewBuild, "std::vector<RenderItemRecord>"), "Prepared views must not own a second full command collection.");
			context.Expect(Absent(closure, "selectedCommands") && !Has(publication, "selectedCommands") && Absent(vulkan, "selectedCommands"), "No copied full per-view command payload may return.");
			context.Expect(Absent(closure, "input.groupedInstanceRecords = frame.groupedInstanceRecords")
				&& Has(Region(context, sealing, "RenderSystem::MakeFramePresentationInput(", "RenderSystem::QueueFramePreparedSubmission("), "input.groupedInstanceRecords = std::move(frame.groupedInstanceRecords)"), "View lowering transfers the grouped instance stream.");
			const auto prepared = Region(context, sealingHeader, "struct RenderSystem::PreparedRenderSubmission", "struct RenderSystem::FrameViewBuildOutputs");
			context.Expect(!prepared.empty() && !Has(prepared, "RenderViewBuildState frame"), "Queued submission retains immutable presentation rather than another build-state owner.");
			const auto hit = Region(context, visible, "RenderSystem: Reuse Visible Draw Commands", "RenderSystem: Reuse Visible Diagnostics");
			context.Expect(Has(hit, "std::vector<RenderItemRecord> cachedCommands;") && Has(hit, "!existing->second.hasViewDrawEvidence") && !Has(hit, "std::vector<RenderItemRecord> cachedCommands ="), "Visible-cache hits defer full command materialization.");
			const auto queue = Region(context, sealing, "RenderSystem::QueueFramePreparedSubmission(", "RenderSystem::CopyFrameVirtualGeometryPayload(");
			context.Expect(Has(queue, "reuseCachedViewLanes") && Has(queue, "prepared.presentation.retainedCommandLanes = cachedRecord->commandLanes")
				&& Has(queue, "prepared.selection.retainedPayload = cachedRecord->retainedViewSelection") && Has(queue, "prepared.presentation.retainedCommandSelectionIndices") && Has(queue, "cachedRecord->stableOrdinals[index]"), "Stable cached views retain compact lanes and ordinals without rescanning full commands.");
			context.Expect(Absent(closure, "backendWorkPlan") && !Has(publication, "backendWorkPlan") && Absent(vulkan, "backendWorkPlan"), "Sealed frames retain compact backend work instead of copied graph plans.");
			context.Expect(Has(snapshot, "RenderSystem::AssembleFrameSnapshotRows(") && Has(snapshot, "snapshotRenderable.animationBinding = entry.animationBinding")
				&& Has(topology, "if(!reuseRenderableRecords)") && Has(spatial, "renderFrameSnapshot.renderables[index].animationBinding"), "Rebuilt snapshots retain complete bindings while warm frames preserve the immutable rows.");
			const auto membership = Region(context, capture, "RenderSystem::RefreshFrameRenderableMembership(", "RenderSystem::CollectFrameAnimationBinding(");
			context.Expect(Has(membership, "const bool applyDelta =") && Has(membership, "protectedSceneChanges->ComponentChanged()") && Has(membership, "renderableMembershipCache.erase(existing)"), "Validated scene packets patch membership without a full ECS scan for each edit.");
			context.Expect(Has(dynamic, "RenderSystem::PrepareFrameDynamicPages(") && Has(dynamic, "lane.transformGeneration =") && Has(dynamic, "renderable.transformGeneration") && Has(spatial, "sparseTransformRefresh"), "Dynamic pages retain object-local generations and sparse matrix refresh.");
			context.Expect(Has(tasks, "taskIndex = 1u; taskIndex < workerCount") && Has(tasks, "work(0U, std::min(itemCount, chunkSize))"), "Chunked work uses its current scheduler worker for one range.");

			const auto coroutine = Region(context, coordinator, "RenderSystem::BuildFrameExecution(", "void RenderSystem::Shutdown()");
			const std::string suspend = "co_await std::suspend_always{};";
			std::vector<std::size_t> boundaries;
			std::size_t cursor = 0;
			for(int index = 0; index < 3; ++index) { const auto found = coroutine.find(suspend, cursor); if(found == std::string::npos) break; boundaries.push_back(found); cursor = found + suspend.size(); }
			context.Expect(boundaries.size() == 3 && Count(coroutine, suspend) == 3, "Coordinator retains exactly three real coroutine suspensions.");
			const auto worker = boundaries.size() == 3 ? coroutine.substr(boundaries[0], boundaries[2] - boundaries[0]) : std::string{};
			context.Expect(!worker.empty() && !Has(worker, "objManager.") && !Has(worker, "objectManager.") && !Has(worker, "resourceManager.") && !Has(worker, "ResourceManager::Instance()") && !Has(worker, "rhi->"), "Worker coordinator delegates through captured values without reopening mutable scene/resource/backend state.");
			for(const char* call : {"AssembleFrameSnapshotRows(", "PrepareFramePersistentTopology(", "PrepareFrameSpatialInputs(", "PrepareFrameBaseCommands(", "PrepareFrameLightCandidates(", "PrepareFrameSceneView(", "PrepareFrameGameViews(", "PrepareFrameDedicatedViews(", "PrepareFrameDynamicPages(", "FinalizeFramePublication("})
				context.Expect(Has(worker, call), "Retained worker coordinator must call the actual relocated work, not merely keep suspension markers.");
			for(const char* name : {"RenderPublicationCapture", "GeometryRoutePrepare", "VirtualGeometryPrepare", "BackendCommit", "RenderPublication"})
				context.Expect(Has(coordinator, std::string("workloadName = \"") + name + "\""), "All five unchanged native scheduler callbacks remain declared.");

			// Scan actual delegated module bodies as well as the coordinator. The
			// two preserved VG publication getters are reviewed separately below;
			// execution/profiling services are not live scene-data access.
			for(const auto& unit : closure) {
				if(!unit.path.starts_with("Engine/Renderer/Frame/") || !unit.path.ends_with(".cpp") || unit.path.ends_with("RenderFrameCapture.cpp") || unit.path.ends_with("RenderBackendCommit.cpp") || unit.path.ends_with("RenderStreamingFeedback.cpp")) continue;
				const auto inspected = unit.path.ends_with("RenderDiagnosticsPublication.cpp")
					? Region(context, unit.text, "CapabilityStage ToCapabilityStage(", "void RenderSystem::PublishFrameDiagnostics(") : unit.text;
				context.Expect(Count(inspected, "VirtualGeometryStreamingSystem::Instance()")
					== (unit.path.ends_with("RenderSnapshotPreparation.cpp") ? 2U : 0U),
					"The reviewed copied VG snapshots are the only streaming singleton acquisitions in delegated worker modules.");
				for(const char* forbidden : {"objManager.", "objectManager.", "ObjectManager::instance()", "ResourceManager::Instance()", "resourceManager.", "rhi->"})
					context.Expect(!Has(inspected, forbidden), "Actual delegated worker helpers must not reopen mutable ECS/resource/backend owners.");
			}
			context.Expect(Count(snapshot, "VirtualGeometryStreamingSystem::Instance().ResidencySnapshot()") == 1 && Count(snapshot, "VirtualGeometryStreamingSystem::Instance().PublishedPages()") == 1, "Only the two original read-only VG snapshot acquisitions remain in snapshot assembly.");
			const auto streamingHeader = read("Engine/Resource/VirtualGeometryStreamingSystem.h");
			context.Expect(Has(streamingHeader, "std::vector<VirtualGeometryPagePublication> PublishedPages() const;")
				&& Has(streamingHeader, "VirtualGeometryPhysicalPageResidencySnapshot ResidencySnapshot() const;"), "The two reviewed VG getters retain const by-value signatures.");
			const auto streaming = read("Engine/Resource/VirtualGeometryStreamingSystem.cpp");
			const auto pages = Compact(Region(context, streaming, "VirtualGeometryStreamingSystem::PublishedPages() const", "VirtualGeometryStreamingSystem::ResidencySnapshot() const"));
			const auto residency = Compact(Region(context, streaming, "VirtualGeometryStreamingSystem::ResidencySnapshot() const", "VirtualGeometryStreamingSystem::DiagnosticsSnapshot() const"));
			context.Expect(Has(pages, "{std::scoped_locklock(candidateMutex);returnpublicationStore.Snapshot();}") && Has(residency, "{std::scoped_locklock(candidateMutex);returnresidencyStore.Snapshot();}"), "Preserved worker VG reads lock and return owned value snapshots without publishing mutations.");
			const auto stores = read("Engine/Resource/VirtualGeometryPagePublication.cpp");
			context.Expect(Has(Compact(Region(context, stores, "VirtualGeometryPagePublicationStore::Snapshot() const", "VirtualGeometryPagePublicationStore::LastPublicationFrame()")), "{returnvisiblePages;}")
				&& Has(Compact(Region(context, stores, "VirtualGeometryPhysicalPageResidencyStore::Snapshot() const", "VirtualGeometryPhysicalPageResidencyStore::PublishAtProtectedBoundary(")), "{returnvisibleSnapshot;}"), "VG publication getters preserve copied vector/scalar owners; no live store reference escapes.");

			// Compare positions only inside their actual source file. Required marker
			// existence prevents a missing call from passing through npos ordering.
			const auto backend = read("Engine/Renderer/Frame/RenderBackendCommit.cpp");
			const auto feedback = read("Engine/Renderer/Frame/RenderStreamingFeedback.cpp");
			const auto commitCall = coroutine.find("CommitFrameBackend(");
			const auto diagnosticsCall = coroutine.find("PublishFrameDiagnostics(");
			const auto feedbackCall = coroutine.find("PublishFrameStreamingFeedback(");
			const auto touch = backend.find("resourceManager.PublishVisibleStreamingTouches");
			context.Expect(boundaries.size() == 3 && commitCall != std::string::npos && feedbackCall != std::string::npos
				&& boundaries[2] < commitCall && diagnosticsCall != std::string::npos && commitCall < diagnosticsCall && diagnosticsCall < feedbackCall && touch != std::string::npos
				&& Before(backend, "resourceManager.PublishVisibleStreamingTouches", "rhi->BeginFrame()") && Has(feedback, "SubmitPriorFrameFeedback"), "Streaming publication and actual feedback occur through real protected helpers; missing markers must never pass npos ordering.");
			const auto callbacks = Region(context, coordinator, "RenderSystem::DeclareFrameWorkloads(", "RenderSystem::GetLiveGpuSubmissionBoundary()");
			context.Expect(Count(callbacks, "execution->Resume()") == 4, "Four actual resumes and final retirement retain the five callback bodies.");
			for(const char* assignment : {"snapshot.work =", "routePrepare.work =", "prepare.work =", "commit.work =", "publication.work ="})
				context.Expect(Has(callbacks, assignment), "Every native callback must retain its actual assigned work.");
			const auto vulkanContext = read("Engine/Renderer/RHI/Vulkan/VulkanContext.cpp");
			const auto consume = Region(context, vulkanContext, "VulkanContext::ConsumeRenderFramePublication", "VulkanContext::ConsumePublishedWorkPlans");
			context.Expect(!consume.empty() && !Has(consume, "ValidateRenderFramePublication"), "Backend consumption must not repeat crowd-sized publication validation.");
			context.Expect(Absent(closure, "DX12"), "This renderer refactor does not add DX12 behavior.");
			const auto exchange = Region(context, publicationImplementation, "RenderFramePublicationStore::TryPublish", "RenderFramePublicationStore::ResetForSession");
			context.Expect(!exchange.empty() && !Has(exchange, "ValidateRenderFramePublication"), "Protected publication exchanges only an already sealed root.");
		});
	}
}
