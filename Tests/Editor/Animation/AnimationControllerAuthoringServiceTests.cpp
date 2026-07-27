// AnimationControllerAuthoringServiceTests.cpp - real model/compiler/file behavior.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Animation/AnimationControllerAuthoringService.h"

#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <thread>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace {
		std::vector<std::uint8_t> ReadAll(const std::filesystem::path& path) {
			std::ifstream input(path, std::ios::binary);
			return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
		}

		bool WriteAll(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			output.write(reinterpret_cast<const char*>(bytes.data()),
				static_cast<std::streamsize>(bytes.size()));
			return static_cast<bool>(output);
		}
	}

	void RegisterAnimationControllerAuthoringServiceTests(TestRegistry& registry) {
		registry.Add("Editor.Animation.ControllerAuthoring.StarterDocumentDecodesAndCompiles", [](TestContext& context) {
			using namespace TerraEngine::Animation;
			using TerraEngine::Editor::Animation::AnimationControllerAuthoringService;
			AnimationControllerAsset created;
			std::vector<std::uint8_t> bytes;
			context.Expect(AnimationControllerAuthoringService::CreateStarterDocument(created, bytes)
				== AnimationStatus::Ok && !bytes.empty(),
				"Controller creation must emit a real cooked document, not a text placeholder.");
			AnimationControllerAsset decoded;
			EvaluationProgram program;
			context.Expect(DeserializeAnimationControllerAsset(bytes, 0U, decoded) == AnimationStatus::Ok
				&& decoded.assetId == created.assetId
				&& CompileAnimationControllerGraph(decoded, {}, program).status == AnimationStatus::Ok,
				"The starter controller must survive its production decoder and compiler.");
		});

		registry.Add("Editor.Animation.ControllerAuthoring.AtomicCommitUndoRedoAndCancel", [](TestContext& context) {
			using namespace TerraEngine::Animation;
			using namespace TerraEngine::Editor::Animation;
			const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
			const std::filesystem::path workspace = std::filesystem::temp_directory_path()
				/ ("TerraEngineControllerAuthoring-" + std::to_string(unique));
			const std::filesystem::path controllerPath = workspace / "Locomotion.teanimctrl";
			std::error_code error;
			std::filesystem::create_directories(workspace, error);
			AnimationControllerAsset starter;
			std::vector<std::uint8_t> originalBytes;
			context.Expect(AnimationControllerAuthoringService::CreateStarterDocument(starter, originalBytes)
				== AnimationStatus::Ok,
				"The file workflow fixture requires a valid starter controller.");
			WriteAll(controllerPath, originalBytes);

			AnimationControllerAuthoringService service;
			context.Expect(service.Open(controllerPath, 1U) == AnimationAuthoringStatus::Ok
				&& service.BeginEdit(starter.assetId, 1U) == AnimationAuthoringStatus::Ok,
				"The authoring model must open and detach an exact GUID/generation candidate.");
			std::uint64_t parameterId = 0U;
			std::uint64_t nodeId = 0U;
			std::uint64_t stateId = 0U;
			std::uint64_t transitionId = 0U;
			std::uint64_t layerId = 0U;
			context.Expect(service.AddParameter("Speed", AnimationControllerParameterType::Float, parameterId)
				== AnimationAuthoringStatus::Ok
				&& service.AddNode(AnimationControllerNodeKind::ProceduralPose, nodeId) == AnimationAuthoringStatus::Ok
				&& service.AddState(nodeId, false, stateId) == AnimationAuthoringStatus::Ok
				&& service.AddTransition(2U, stateId, transitionId) == AnimationAuthoringStatus::Ok
				&& service.AddLayer(stateId, layerId) == AnimationAuthoringStatus::Ok,
				"The UI-independent model must author every window-owned graph collection.");
			// Make the new state reachable while preserving exactly one top-level entry.
			auto* candidate = service.MutableCandidate();
			candidate->transitions.back().conditionParameterId = 0U;
			AnimationAuthoringCompileRequest request;
			context.Expect(service.PrepareCommit(request) == AnimationAuthoringStatus::Ok,
				"Prepare must snapshot the complete model into a worker-safe request.");
			AnimationControllerCompileProduct product = AnimationControllerAuthoringService::CompileCandidate(request);
			context.Expect(product.authoring.succeeded
				&& service.CompleteCommit(product) == AnimationAuthoringStatus::Ok
				&& service.Generation() == 2U,
				"Only a fully decoded and compiled candidate may reach atomic publication.");

			AnimationControllerAsset onDisk;
			std::vector<std::uint8_t> committedBytes = ReadAll(controllerPath);
			context.Expect(DeserializeAnimationControllerAsset(committedBytes, 0U, onDisk) == AnimationStatus::Ok
				&& onDisk.parameters.size() == 1U && onDisk.nodes.size() == 2U
				&& onDisk.states.size() == 2U && onDisk.transitions.size() == 1U
				&& onDisk.layers.size() == 2U,
				"The actual file publication must contain the complete authored graph.");
			context.Expect(service.Undo() == AnimationAuthoringStatus::Ok
				&& service.Generation() == 3U && ReadAll(controllerPath) == originalBytes,
				"Undo must atomically republish the complete before document.");
			context.Expect(service.Redo() == AnimationAuthoringStatus::Ok
				&& service.Generation() == 4U && ReadAll(controllerPath) == committedBytes,
				"Redo must atomically republish the complete after document.");

			context.Expect(service.BeginEdit(service.AssetId(), service.Generation()) == AnimationAuthoringStatus::Ok,
				"A second detached edit should begin from the redone generation.");
			std::uint64_t canceledParameter = 0U;
			service.AddParameter("Canceled", AnimationControllerParameterType::Float, canceledParameter);
			AnimationAuthoringCompileRequest canceledRequest;
			service.PrepareCommit(canceledRequest);
			context.Expect(service.Cancel() == AnimationAuthoringStatus::Canceled
				&& canceledRequest.cancellationRequested->load()
				&& ReadAll(controllerPath) == committedBytes,
				"Cancellation must signal a dispatched worker and preserve the last good file.");
			std::filesystem::remove_all(workspace, error);
		});

		registry.Add("Editor.Animation.ControllerAuthoring.ExternalWriteRejectsPublication", [](TestContext& context) {
			using namespace TerraEngine::Animation;
			using namespace TerraEngine::Editor::Animation;
			const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
			const std::filesystem::path workspace = std::filesystem::temp_directory_path()
				/ ("TerraEngineControllerStale-" + std::to_string(unique));
			const std::filesystem::path path = workspace / "Controller.teanimctrl";
			std::error_code error;
			std::filesystem::create_directories(workspace, error);
			AnimationControllerAsset starter;
			std::vector<std::uint8_t> bytes;
			context.Expect(AnimationControllerAuthoringService::CreateStarterDocument(starter, bytes)
				== AnimationStatus::Ok,
				"The stale-publication fixture requires a valid starter controller.");
			WriteAll(path, bytes);
			AnimationControllerAuthoringService service;
			service.Open(path, 9U);
			service.BeginEdit(starter.assetId, 9U);
			std::uint64_t parameterId = 0U;
			service.AddParameter("Speed", AnimationControllerParameterType::Float, parameterId);
			AnimationAuthoringCompileRequest request;
			service.PrepareCommit(request);
			AnimationControllerCompileProduct product = AnimationControllerAuthoringService::CompileCandidate(request);
			const std::vector<std::uint8_t> externalBytes{'e', 'x', 't', 'e', 'r', 'n', 'a', 'l'};
			WriteAll(path, externalBytes);
			context.Expect(service.CompleteCommit(product) == AnimationAuthoringStatus::PublicationFailed
				&& service.Generation() == 9U && ReadAll(path) == externalBytes,
				"A changed source file must fail the byte compare and preserve the external writer's bytes.");
			std::filesystem::remove_all(workspace, error);
		});

		registry.Add("Editor.Animation.ControllerAuthoring.ConcurrentSameRevisionHasOneWinner", [](TestContext& context) {
			using namespace TerraEngine::Animation;
			using namespace TerraEngine::Editor::Animation;
			const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
			const std::filesystem::path workspace = std::filesystem::temp_directory_path()
				/ ("TerraEngineControllerConcurrent-" + std::to_string(unique));
			const std::filesystem::path path = workspace / "Controller.teanimctrl";
			std::error_code error;
			std::filesystem::create_directories(workspace, error);
			AnimationControllerAsset starter;
			std::vector<std::uint8_t> original;
			context.Expect(AnimationControllerAuthoringService::CreateStarterDocument(starter, original)
				== AnimationStatus::Ok && WriteAll(path, original),
				"The concurrent publication fixture requires one valid shared revision.");

			AnimationControllerAuthoringService first;
			AnimationControllerAuthoringService second;
			first.Open(path, 7U);
			second.Open(path, 7U);
			first.BeginEdit(starter.assetId, 7U);
			second.BeginEdit(starter.assetId, 7U);
			std::uint64_t firstParameter = 0U;
			std::uint64_t secondParameter = 0U;
			first.AddParameter("First", AnimationControllerParameterType::Float, firstParameter);
			second.AddParameter("Second", AnimationControllerParameterType::Float, secondParameter);
			AnimationAuthoringCompileRequest firstRequest;
			AnimationAuthoringCompileRequest secondRequest;
			first.PrepareCommit(firstRequest);
			second.PrepareCommit(secondRequest);
			const AnimationControllerCompileProduct firstProduct =
				AnimationControllerAuthoringService::CompileCandidate(firstRequest);
			const AnimationControllerCompileProduct secondProduct =
				AnimationControllerAuthoringService::CompileCandidate(secondRequest);

			std::atomic_bool start{false};
			auto publish = [&start](AnimationControllerAuthoringService& service,
				const AnimationControllerCompileProduct& product) {
				while(!start.load(std::memory_order_acquire)) std::this_thread::yield();
				return service.CompleteCommit(product);
			};
			auto firstFuture = std::async(std::launch::async, publish,
				std::ref(first), std::cref(firstProduct));
			auto secondFuture = std::async(std::launch::async, publish,
				std::ref(second), std::cref(secondProduct));
			start.store(true, std::memory_order_release);
			const AnimationAuthoringStatus firstStatus = firstFuture.get();
			const AnimationAuthoringStatus secondStatus = secondFuture.get();
			const int successCount = static_cast<int>(firstStatus == AnimationAuthoringStatus::Ok)
				+ static_cast<int>(secondStatus == AnimationAuthoringStatus::Ok);
			const int staleCount = static_cast<int>(firstStatus == AnimationAuthoringStatus::PublicationFailed)
				+ static_cast<int>(secondStatus == AnimationAuthoringStatus::PublicationFailed);
			AnimationControllerAsset published;
			const std::vector<std::uint8_t> publishedBytes = ReadAll(path);
			context.Expect(successCount == 1 && staleCount == 1,
				"Exactly one controller publisher may win a same-revision compare-and-replace race.");
			context.Expect(DeserializeAnimationControllerAsset(publishedBytes, 0U, published)
				== AnimationStatus::Ok && published.parameters.size() == 1U,
				"The winning concurrent publication must leave one complete decodable controller.");
			std::filesystem::remove_all(workspace, error);
		});
	}
}
