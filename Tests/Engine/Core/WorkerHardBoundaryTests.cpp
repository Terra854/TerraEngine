#include "../TestHarness.h"

#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Core/Scheduling/WorkerSafety.h"
#include "Input/InputSystem.h"
#include "Window/WindowBackend.h"
#include "Window/WindowSystem.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>

namespace TerraEngine::Tests {
	namespace {
		struct WorkerSafetyFixtureComponent {
			int value = 0;
		};

		class WorkerBoundaryWindowBackend final : public IWindowBackend
		{
		public:
			std::string BackendName() const override { return "WorkerBoundaryWindowBackend"; }
			void Initialize(const WindowConfig&) override { initialized = true; }
			void PollEvents() override { ++pollCount; }
			void SwapBuffers() override { ++swapCount; }
			bool ShouldClose() const override { return closeRequested; }
			void* GetNativeHandle() const override { return nativeHandle; }
			void SetCloseCallback(std::function<void()> onClose) override { closeCallback = std::move(onClose); }
			void SetResizeCallback(std::function<void(int, int)> onResize) override { resizeCallback = std::move(onResize); }
			void Shutdown() override { initialized = false; }

			void* nativeHandle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xA11CEu));
			std::function<void()> closeCallback;
			std::function<void(int, int)> resizeCallback;
			int pollCount = 0;
			int swapCount = 0;
			bool closeRequested = false;
			bool initialized = false;
		};

		const TerraEngine::Scheduling::HardBoundary* FindBoundary(const std::vector<TerraEngine::Scheduling::HardBoundary>& boundaries, const std::string& name)
		{
			const auto found = std::find_if(boundaries.begin(), boundaries.end(), [&](const TerraEngine::Scheduling::HardBoundary& boundary) {
				return boundary.boundaryName == name;
			});
			return found == boundaries.end() ? nullptr : &(*found);
		}
	}

	void RegisterWorkerHardBoundaryTests(TestRegistry& registry)
	{
		registry.Add("Engine.WorkerHardBoundary.InitialClassificationsMatchPlan", [](TestContext& context) {
			const auto boundaries = TerraEngine::Scheduling::InitialWorkerHardBoundaries();

			context.ExpectEqual(static_cast<std::size_t>(4), boundaries.size(), "The feature should report all four named hard boundaries.");
			context.Expect(FindBoundary(boundaries, "ManagedGameplayCallbacks")->currentClassification == TerraEngine::Scheduling::HardBoundaryClassification::PartiallyWorkerSafe, "Managed callbacks should be partially worker-safe.");
			context.Expect(FindBoundary(boundaries, "LiveGpuSubmission")->currentClassification == TerraEngine::Scheduling::HardBoundaryClassification::PartiallyWorkerSafe, "Live GPU submission should be partially worker-safe.");
			context.Expect(FindBoundary(boundaries, "WindowInputPolling")->currentClassification == TerraEngine::Scheduling::HardBoundaryClassification::Protected, "Window/input polling should stay protected.");
			context.Expect(FindBoundary(boundaries, "EditorReflectionMutation")->currentClassification == TerraEngine::Scheduling::HardBoundaryClassification::Protected, "Editor reflection mutation should stay protected.");
		});

		registry.Add("Engine.WorkerHardBoundary.WindowAndInputSnapshotsAreWorkerReadableAfterProtectedPolling", [](TestContext& context) {
			WindowSystem& windowSystem = WindowSystem::Instance();
			WindowConfig config;
			config.width = 1920;
			config.height = 1080;
			config.title = "Worker Snapshot";
			windowSystem.Shutdown();
			windowSystem.Configure(std::make_unique<WorkerBoundaryWindowBackend>(), config, []() {});
			windowSystem.Initialize();

			windowSystem.Update(0.016f);
			const WindowSystem::WindowPollingBoundaryDiagnostics diagnostics = windowSystem.GetPollingBoundaryDiagnostics();
			const WindowSystem::WindowWorkerSnapshot windowSnapshot = windowSystem.GetLatestWorkerSnapshot();
			const std::string inputBoundarySummary = InputSystem::Instance().GetPollingBoundarySummary();

			context.Expect(diagnostics.classification == "protected", "Window polling should remain a protected coordinator boundary.");
			context.Expect(diagnostics.lastPollRan && diagnostics.lastSwapRan, "Protected polling and swap bookkeeping should run before publishing the snapshot.");
			context.Expect(windowSnapshot.workerReadable, "Window snapshots should become worker-readable after protected polling.");
			context.ExpectEqual(static_cast<std::uint64_t>(1), windowSnapshot.frameRevision, "Window snapshot should publish the protected poll revision.");
			context.ExpectEqual(1920, windowSnapshot.width, "Window snapshot should copy immutable width.");
			context.ExpectEqual(1080, windowSnapshot.height, "Window snapshot should copy immutable height.");
			context.Expect(inputBoundarySummary.find("protected") != std::string::npos, "Input boundary contract should describe protected polling.");

			windowSystem.Shutdown();
		});

		registry.Add("Engine.WorkerHardBoundary.FindingsNameSafeAndProtectedPortions", [](TestContext& context) {
			for (const TerraEngine::Scheduling::ConversionFinding& finding : TerraEngine::Scheduling::InitialWorkerConversionFindings())
			{
				context.Expect(!finding.targetName.empty(), "Findings should name their target.");
				context.Expect(!finding.evidence.empty(), "Findings should record worker-safe evidence or safe subwork.");
				context.Expect(!finding.blockingReason.empty(), "Findings should record protected blocking reasons.");
				context.Expect(!finding.recommendedAction.empty(), "Findings should record reevaluation conditions.");
			}
		});

		registry.Add("Engine.WorkerHardBoundary.DirectWorkerObjectCreateIsDiagnosed", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			(void)TerraEngine::Scheduling::DrainWorkerSafetyViolations();
			bool threw = false;
			{
				TerraEngine::Scheduling::WorkerSafetyScope scope("ObjectCreateWorker");
				try
				{
					(void)objects.createObject("Unsafe");
				}
				catch (const std::runtime_error&)
				{
					threw = true;
				}
			}
			const auto violations = TerraEngine::Scheduling::DrainWorkerSafetyViolations();

			context.Expect(threw, "Direct worker object creation should be blocked.");
			context.Expect(!violations.empty(), "Direct worker object creation should report a safety violation.");
			context.Expect(violations.front().operationName == "ObjectManager.createObject", "Violation should name the object creation API.");
			objects.clear();
		});

		registry.Add("Engine.WorkerHardBoundary.DirectWorkerHierarchyAndDeleteAreDiagnosed", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object parent = objects.createObject("Parent");
			Object child = objects.createObject("Child");
			(void)TerraEngine::Scheduling::DrainWorkerSafetyViolations();
			bool parentChanged = true;
			{
				TerraEngine::Scheduling::WorkerSafetyScope scope("HierarchyWorker");
				parentChanged = objects.setParent(child, parent);
				objects.removeObject(child);
			}
			const auto violations = TerraEngine::Scheduling::DrainWorkerSafetyViolations();

			context.Expect(!parentChanged, "Direct worker reparenting should be blocked.");
			context.Expect(objects.getObject(child.EntityId()).has_value(), "Blocked worker delete should leave the object alive.");
			context.Expect(violations.size() >= 2, "Hierarchy and delete attempts should both report safety violations.");
			objects.clear();
		});

		registry.Add("Engine.WorkerHardBoundary.DirectWorkerComponentMutationIsDiagnosed", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object object = objects.createObject("ComponentTarget");
			(void)TerraEngine::Scheduling::DrainWorkerSafetyViolations();
			bool addBlocked = false;
			{
				TerraEngine::Scheduling::WorkerSafetyScope scope("ComponentWorker");
				addBlocked = objects.AddComponent<WorkerSafetyFixtureComponent>(object.EntityId()) == nullptr;
				objects.RemoveComponent<Transform>(object.EntityId());
			}
			const auto violations = TerraEngine::Scheduling::DrainWorkerSafetyViolations();

			context.Expect(addBlocked, "Direct worker component add should be blocked.");
			context.Expect(objects.GetComponent<Transform>(object.EntityId()) != nullptr, "Blocked worker component removal should leave existing components intact.");
			context.Expect(violations.size() >= 2, "Component add and remove attempts should both report safety violations.");
			objects.clear();
		});

		registry.Add("Engine.WorkerHardBoundary.SafetyViolationSnapshotDoesNotDrain", [](TestContext& context) {
			(void)TerraEngine::Scheduling::DrainWorkerSafetyViolations();
			{
				TerraEngine::Scheduling::WorkerSafetyScope scope("InjectedViolationWorker");
				TerraEngine::Scheduling::ReportWorkerSafetyViolation(
					TerraEngine::Scheduling::WorkerSafetySeverity::Error,
					"InjectedUnsafeMutation",
					"Injected test violation should remain available after snapshot reads.",
					"Drain violations only at explicit validation boundaries.");
			}

			const auto snapshot = TerraEngine::Scheduling::SnapshotWorkerSafetyViolations();
			const auto drained = TerraEngine::Scheduling::DrainWorkerSafetyViolations();

			context.ExpectEqual(static_cast<std::size_t>(1), snapshot.size(), "Snapshot should expose the injected violation.");
			context.ExpectEqual(static_cast<std::size_t>(1), drained.size(), "Snapshot should not consume the violation log.");
			context.ExpectEqual(std::string("InjectedUnsafeMutation"), drained.front().operationName, "Drained violation should preserve operation details after snapshot.");
		});
	}
}
