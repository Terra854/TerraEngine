// Idempotency and atomic rejection tests for animation automation.

#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Animation/AnimationAssetAuthoringDecode.h"
#include "Editor/TerraEngine-Editor/Assets/AssetDatabase.h"
#include "Editor/TerraEngine-Editor/Automation/AnimationControllerOperations.h"
#include "Editor/TerraEngine-Editor/Automation/AnimationAutomationTransactions.h"
#include "Engine/Animation/Assets/AnimationControllerAsset.h"
#include "Engine/Animation/Assets/SkeletonAsset.h"
#include "Utilities/UUID.h"

#include <filesystem>
#include <fstream>
#include <vector>

namespace TerraEngine::EditorTests
{
	namespace
	{
		using namespace TerraEngine::Animation;

		struct TemporaryAnimationAutomationProject
		{
			TemporaryAnimationAutomationProject()
				: root(std::filesystem::temp_directory_path()
					/ ("TerraEngine-AnimationAutomation-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID())))
			{
				std::filesystem::create_directories(root);
				AssetDatabase::Instance().Initialize(root);
			}

			~TemporaryAnimationAutomationProject()
			{
				AssetDatabase::Instance().Shutdown();
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
			}

			std::filesystem::path root;
		};

		AnimationGuid MakeGuid()
		{
			return AnimationGuid{TerraEngine::GenerateUUID()};
		}

		AnimationControllerAsset MakeAutomationController()
		{
			AnimationControllerAsset controller;
			controller.assetId = MakeGuid();
			controller.skeletonAssetId = MakeGuid();
			controller.compatibilityFingerprint = 0x4343U;
			controller.parameters = {{10U, "Speed", AnimationControllerParameterType::Float, 1.0F}};
			controller.nodes = {{20U, AnimationControllerNodeKind::Clip, 0U,
				MakeGuid(), 1U, controller.compatibilityFingerprint}};
			controller.states = {{30U, 20U, 0U, true, false, false}};
			controller.layers = {{40U, 30U, {}, 0U, 0U, false, 1.0F}};
			return controller;
		}

		SkeletonAsset MakeAutomationSkeleton()
		{
			SkeletonAsset skeleton;
			skeleton.assetId = MakeGuid();
			skeleton.compatibilityFingerprint = 0x4343000011223344ULL;
			skeleton.referencePoseHash = 0x4343556677889900ULL;
			skeleton.rootJointIndex = 0U;
			SkeletonJoint root;
			root.stableId = 43U;
			root.parentIndex = -1;
			root.displayName = "Root";
			root.normalizedSourcePath = "/root";
			root.semanticRole = "hips";
			skeleton.joints.push_back(root);
			AnimationMatrix4x4 inverseBind{};
			inverseBind.values = {1.0F, 0.0F, 0.0F, 0.0F,
				0.0F, 1.0F, 0.0F, 0.0F,
				0.0F, 0.0F, 1.0F, 0.0F,
				0.0F, 0.0F, 0.0F, 1.0F};
			skeleton.inverseBindMatrices.push_back(inverseBind);
			return skeleton;
		}

		bool WriteBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
		{
			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			if(!stream) return false;
			stream.write(reinterpret_cast<const char*>(bytes.data()),
				static_cast<std::streamsize>(bytes.size()));
			return static_cast<bool>(stream);
		}

		std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path)
		{
			std::ifstream stream(path, std::ios::binary | std::ios::ate);
			if(!stream) return {};
			const std::streamoff size = stream.tellg();
			if(size < 0) return {};
			std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
			stream.seekg(0, std::ios::beg);
			if(!bytes.empty())
			{
				stream.read(reinterpret_cast<char*>(bytes.data()),
					static_cast<std::streamsize>(bytes.size()));
			}
			return stream ? bytes : std::vector<std::uint8_t>{};
		}
	}

	void RegisterAnimationAutomationMutationTests(TestRegistry& registry)
	{
		registry.Add("Editor.Animation.AuthoringDecode.AcceptsCheckedEmbeddedSkeletonFingerprint", [](TestContext& context) {
			using TerraEngine::Editor::Animation::DeserializeAnimationAssetForAuthoring;
			const SkeletonAsset source = MakeAutomationSkeleton();
			std::vector<std::uint8_t> bytes;
			SkeletonAsset decoded;
			const AnimationStatus serialized = SerializeSkeletonAsset(source, bytes);
			const AnimationStatus zeroFingerprint = serialized == AnimationStatus::Ok
				? DeserializeSkeletonAsset(bytes, 0U, decoded)
				: serialized;
			const AnimationStatus authoringDecoded = serialized == AnimationStatus::Ok
				? DeserializeAnimationAssetForAuthoring(
					bytes, decoded, DeserializeSkeletonAsset)
				: serialized;
			context.Expect(serialized == AnimationStatus::Ok
				&& zeroFingerprint == AnimationStatus::IncompatibleAsset
				&& authoringDecoded == AnimationStatus::Ok
				&& decoded.assetId == source.assetId
				&& decoded.compatibilityFingerprint == source.compatibilityFingerprint,
				"Editor authoring without an external binding must take the checked envelope fingerprint before decoding a generated skeleton.");
		});

		registry.Add("Editor.Animation.Automation.Mutation.AtMostOnceAndStaleAtomic", [](TestContext& context) {
			using namespace Editor::Automation;
			AnimationAutomationTransactions transactions;
			AnimationAutomationMutation mutation{
				"animation.controller.edit",
				"key",
				"correlation-one",
				3U,
				false,
				{{"node", "Idle"}, {"correlationId", "correlation-one"}}
			};
			const auto first = transactions.Execute(mutation, 3U);
			// The bridge generates a fresh correlation ID when a retry did not supply
			// one. That transport identity must not change the semantic fingerprint.
			mutation.correlationId = "correlation-two";
			mutation.payload["correlationId"] = "correlation-two";
			const auto retry = transactions.Execute(mutation, 3U);
			context.Expect(
				first.ok && retry.ok
					&& first.data.value("idempotencyDisposition", "") == "new"
					&& retry.data.value("idempotencyDisposition", "") == "replay"
					&& retry.data.value("generation", 0U) == first.data.value("generation", 0U),
				"The same idempotency key replays the retained generation without publishing twice.");

			mutation.payload = {{"node", "Run"}, {"correlationId", "correlation-three"}};
			const auto conflict = transactions.Execute(mutation, 3U);
			context.Expect(
				!conflict.ok && conflict.error == ErrorCategory::Conflict && transactions.RetainedCount() == 1U,
				"Reusing a key for different work is rejected without replacing the first receipt.");

			mutation.idempotencyKey = "stale";
			const auto stale = transactions.Execute(mutation, 4U);
			context.Expect(
				!stale.ok && stale.error == ErrorCategory::StaleTarget && transactions.RetainedCount() == 1U,
				"A stale request is rejected without retaining a partial mutation.");
		});

		registry.Add("Editor.Animation.Automation.Mutation.TypedCommitFailureIsRetryable", [](TestContext& context) {
			using namespace Editor::Automation;
			AnimationAutomationTransactions transactions;
			const AnimationAutomationMutation mutation{
				"animation.runtime.parameters",
				"typed-commit",
				"typed-correlation",
				5U,
				false,
				{{"commands", nlohmann::json::array({{{"kind", "play"}}})}}
			};
			std::uint32_t attempts = 0U;
			const auto commit = [&attempts]() {
				++attempts;
				AutomationDispatchResult result;
				result.ok = attempts == 2U;
				result.error = result.ok ? ErrorCategory::Internal : ErrorCategory::QueueFull;
				result.message = result.ok ? "Committed once." : "The typed queue rejected this attempt.";
				result.data = {{"attempt", attempts}};
				return result;
			};

			const AutomationDispatchResult rejected = transactions.ExecuteWithCommit(mutation, 5U, commit);
			const AutomationDispatchResult accepted = transactions.ExecuteWithCommit(mutation, 5U, commit);
			const AutomationDispatchResult replayed = transactions.ExecuteWithCommit(mutation, 5U, commit);
			context.Expect(!rejected.ok && rejected.error == ErrorCategory::QueueFull
				&& accepted.ok && replayed.ok && attempts == 2U,
				"A failed typed commit must not be retained, while the first success must execute exactly once.");
			context.Expect(transactions.RetainedCount() == 1U
				&& accepted.data.value("attempt", 0U) == 2U
				&& replayed.data.value("idempotencyDisposition", std::string{}) == "replay",
				"The durable receipt must describe the successful typed commit and reconcile later retries.");
		});

		registry.Add("Editor.Animation.Automation.Mutation.ControllerPublishesRealAssetAndRejectsNoOp", [](TestContext& context) {
			using namespace Editor::Automation;
			TemporaryAnimationAutomationProject project;
			AssetDatabase& assets = AssetDatabase::Instance();
			const std::filesystem::path source = assets.GetAssetRoot() / "Automation.teanimctrl";

			AnimationControllerAsset controller = MakeAutomationController();
			std::vector<std::uint8_t> bytes;
			context.Expect(SerializeAnimationControllerAsset(controller, bytes) == AnimationStatus::Ok
				&& WriteBytes(source, bytes),
				"The controller fixture must serialize before AssetDatabase registration.");
			assets.ImportAsset(source, true);
			const auto registered = assets.GetAssets().find(std::filesystem::absolute(source));
			context.Expect(registered != assets.GetAssets().end(),
				"The cooked controller fixture must be registered synchronously.");
			if(registered == assets.GetAssets().end()) return;

			// Loose cooked assets use the AssetDatabase GUID as their runtime identity.
			// Re-envelop the fixture with that GUID before exercising the protected edit.
			controller.assetId = AnimationGuid{registered->second.guid};
			bytes.clear();
			context.Expect(SerializeAnimationControllerAsset(controller, bytes) == AnimationStatus::Ok
				&& WriteBytes(source, bytes),
				"The fixture envelope must match its registered asset identity.");
			assets.ImportAsset(source, true);

			AnimationAutomationTransactions transactions;
			const nlohmann::json edit = {
				{"assetId", TerraEngine::ToHexString(registered->second.guid)},
				{"idempotencyKey", "controller-real-edit"},
				{"correlationId", "controller-real-edit-correlation"},
				{"expectedGeneration", 1U},
				{"payload", {
					{"action", "set_parameter_default"},
					{"stableId", 10U},
					{"floatValue", 2.5F}
				}}
			};
			const AutomationDispatchResult published = ExecuteAnimationControllerOperation(
				"animation.controller.edit", edit, transactions, 1U, &assets);
			const std::filesystem::path compiled = assets.GetBuildRoot() / registered->second.compiledPath;
			AnimationControllerAsset restored;
			const std::vector<std::uint8_t> publishedBytes = ReadBytes(compiled);
			context.Expect(published.ok
				&& DeserializeAnimationControllerAsset(publishedBytes, 0U, restored) == AnimationStatus::Ok
				&& std::get<float>(restored.parameters.front().defaultValue) == 2.5F,
				"A successful controller edit must publish changed checked bytes through AssetDatabase.");

			// Reproduce the installed-editor ordering where the watcher has already
			// started a worker compile for the first authored save before the next
			// MCP request reaches its protected publication boundary.
			assets.ImportAssetWithProgress(source, true);
			nlohmann::json sequentialEdit = edit;
			sequentialEdit["idempotencyKey"] = "controller-sequential-edit";
			sequentialEdit["correlationId"] = "controller-sequential-edit-correlation";
			sequentialEdit["expectedGeneration"] = 2U;
			sequentialEdit["payload"]["floatValue"] = 3.5F;
			const AutomationDispatchResult sequentialPublished = ExecuteAnimationControllerOperation(
				"animation.controller.edit", sequentialEdit, transactions, 2U, &assets);
			const std::vector<std::uint8_t> sequentialBytes = ReadBytes(compiled);
			context.Expect(sequentialPublished.ok
				&& !assets.IsAssetCompiling(source)
				&& DeserializeAnimationControllerAsset(sequentialBytes, 0U, restored) == AnimationStatus::Ok
				&& std::get<float>(restored.parameters.front().defaultValue) == 3.5F,
				"A sequential authored edit must finalize the matching watcher worker and publish its complete candidate.");

			nlohmann::json noOp = sequentialEdit;
			noOp["idempotencyKey"] = "controller-no-op";
			noOp["expectedGeneration"] = 3U;
			const std::vector<std::uint8_t> beforeNoOp = ReadBytes(source);
			const AutomationDispatchResult rejected = ExecuteAnimationControllerOperation(
				"animation.controller.edit", noOp, transactions, 3U, &assets);
			context.Expect(!rejected.ok && rejected.error == ErrorCategory::Conflict
				&& ReadBytes(source) == beforeNoOp,
				"A no-op controller request must not advance a generation or rewrite the source document.");
		});
	}
}
