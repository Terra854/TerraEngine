// Scroll enters at protected platform polling and leaves as a copied frame value.
#include "TestHarness.h"
#include "Input/InputSystem.h"
#include "Window/WindowSystem.h"
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
namespace {
	class ScrollWindow final : public IWindowBackend {
	public:
		std::string BackendName() const override { return "ScrollFixtureWindow"; }
		void Initialize(const WindowConfig&) override {} void PollEvents() override {} void SwapBuffers() override {}
		bool ShouldClose() const override { return false; } void* GetNativeHandle() const override { return const_cast<ScrollWindow*>(this); }
		void SetCloseCallback(std::function<void()>) override {} void SetResizeCallback(std::function<void(int,int)>) override {} void Shutdown() override {}
	};
	class ScrollBackend final : public IInputBackend {
	public:
		std::function<void(double,double)> scroll;
		std::vector<std::pair<double,double>> queued;
		std::string BackendName() const override { return "QueuedScroll"; }
		void SetWindowHandle(void*) override {} void Initialize() override {} void Shutdown() override { queued.clear(); }
		void Update() override { for(const auto [x,y] : queued) Emit(x,y); queued.clear(); }
		void Emit(double x,double y) { scroll(x,y); }
		void SetKeyCallback(std::function<void(Key,KeyState)>) override {} void SetMouseButtonCallback(std::function<void(MouseButton,KeyState)>) override {}
		void SetCursorPositionCallback(std::function<void(double,double)>) override {} void SetScrollCallback(std::function<void(double,double)> callback) override { scroll = std::move(callback); }
		bool IsKeyPressed(Key) const override { return false; } bool IsMouseButtonPressed(MouseButton) const override { return false; }
		std::pair<double,double> GetCursorPosition() const override { return {}; } void SetCursorPosition(double,double) override {}
	};
	struct Session {
		ScrollBackend* backend = nullptr;
		Session() {
			InputSystem::Instance().Shutdown(); WindowSystem::Instance().Shutdown();
			WindowSystem::Instance().Configure(std::make_unique<ScrollWindow>(), WindowConfig{}, [] {}); WindowSystem::Instance().Initialize();
			auto owned = std::make_unique<ScrollBackend>(); backend = owned.get();
			InputSystem::Instance().Configure(std::move(owned)); InputSystem::Instance().Initialize();
		}
		~Session() { InputSystem::Instance().Shutdown(); WindowSystem::Instance().Shutdown(); }
	};
}
void RegisterScrollFramePublicationTests(TestRegistry& registry) {
	registry.Add("Engine.Input.ScrollFrame.AccumulatesPlatformAndBackendEvents", [](TestContext& context) {
		Session session;
		// WindowSystem can deliver callbacks before InputSystem::Update; another
		// backend may deliver them inside Update. Both belong to the same frame.
		session.backend->Emit(0.5, 2.0); session.backend->Emit(-0.25, 1.0);
		session.backend->queued = {{1.0, -0.5}, {-0.25, 0.5}};
		InputSystem::Instance().Update(0.0F);
		const auto snapshot = InputSystem::Instance().GetLatestWorkerSnapshot();
		context.Expect(InputSystem::Instance().GetScrollDelta() == std::pair{1.0, 3.0} && snapshot.scrollDelta == std::pair{1.0, 3.0},
			"All signed/fractional events must contribute to the same published frame delta.");
		context.Expect(snapshot.workerReadable && !snapshot.boundarySummary.empty(), "Worker scroll must retain its declared protected polling publication.");
	});
	registry.Add("Engine.Input.ScrollFrame.IdleFrameResetsWithoutChangingRetainedSnapshot", [](TestContext& context) {
		Session session; session.backend->Emit(0.5, -2.0); InputSystem::Instance().Update(0.0F);
		const auto retained = InputSystem::Instance().GetLatestWorkerSnapshot();
		InputSystem::Instance().Update(0.0F);
		const auto idle = InputSystem::Instance().GetLatestWorkerSnapshot();
		context.Expect(idle.scrollDelta == std::pair{0.0, 0.0} && InputSystem::Instance().GetScrollDelta() == idle.scrollDelta,
			"An idle frame must publish zero instead of replaying the previous wheel tick.");
		context.Expect(retained.scrollDelta == std::pair{0.5, -2.0} && idle.frameRevision > retained.frameRevision,
			"Copied worker snapshots preserve the old event while publication advances.");
	});
}
}
