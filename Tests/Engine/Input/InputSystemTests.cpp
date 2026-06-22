#include "TestHarness.h"

#include "Input/InputBackend.h"
#include "Input/InputSystem.h"
#include "Window/WindowBackend.h"
#include "Window/WindowSystem.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <array>
#include <cstddef>

namespace TerraEngine::Tests {
	namespace {
		class TestWindowBackend final : public IWindowBackend {
		public:
			std::string BackendName() const override { return "TestWindow"; }

			void Initialize(const WindowConfig&) override
			{
				// The fake native handle only needs to be non-null so InputSystem
				// can complete the same initialization path used by exported builds.
				nativeHandle_ = this;
			}

			void PollEvents() override {}
			void SwapBuffers() override {}
			bool ShouldClose() const override { return false; }
			void* GetNativeHandle() const override { return nativeHandle_; }
			void SetCloseCallback(std::function<void()> onClose) override { closeCallback_ = std::move(onClose); }
			void SetResizeCallback(std::function<void(int, int)> onResize) override { resizeCallback_ = std::move(onResize); }

			void Shutdown() override
			{
				nativeHandle_ = nullptr;
				closeCallback_ = nullptr;
				resizeCallback_ = nullptr;
			}

		private:
			void* nativeHandle_ = nullptr;
			std::function<void()> closeCallback_;
			std::function<void(int, int)> resizeCallback_;
		};

		class QueuedInputBackend final : public IInputBackend {
		public:
			std::string BackendName() const override { return "QueuedInput"; }
			void SetWindowHandle(void* windowHandle) override { windowHandle_ = windowHandle; }
			void Initialize() override {}

			void Update() override
			{
				// Emit every queued event inside one backend update to mirror a
				// quick export-runtime click that GLFW reports during one poll.
				for (const auto& [button, state] : queuedMouseEvents_)
				{
					mouseState_[static_cast<std::size_t>(button)] = state;
					if (mouseButtonCallback_)
					{
						mouseButtonCallback_(button, state);
					}
				}
				queuedMouseEvents_.clear();
			}

			void Shutdown() override
			{
				windowHandle_ = nullptr;
				queuedMouseEvents_.clear();
			}

			void SetKeyCallback(std::function<void(Key, KeyState)> onKeyEvent) override { keyCallback_ = std::move(onKeyEvent); }
			void SetMouseButtonCallback(std::function<void(MouseButton, KeyState)> onMouseButton) override { mouseButtonCallback_ = std::move(onMouseButton); }
			void SetCursorPositionCallback(std::function<void(double, double)> onCursorMove) override { cursorCallback_ = std::move(onCursorMove); }
			void SetScrollCallback(std::function<void(double, double)> onScroll) override { scrollCallback_ = std::move(onScroll); }

			bool IsKeyPressed(Key) const override { return false; }

			bool IsMouseButtonPressed(MouseButton button) const override
			{
				const std::size_t index = static_cast<std::size_t>(button);
				return index < mouseState_.size()
					&& (mouseState_[index] == KeyState::Pressed || mouseState_[index] == KeyState::Held);
			}

			std::pair<double, double> GetCursorPosition() const override { return cursorPosition_; }
			void SetCursorPosition(double x, double y) override { cursorPosition_ = { x, y }; }

			void SetHardwareCursorPosition(double x, double y)
			{
				// This simulates a platform cursor position that can be polled
				// even when no cursor-move callback has fired this frame.
				cursorPosition_ = { x, y };
			}

			void QueueMouseEvent(MouseButton button, KeyState state)
			{
				queuedMouseEvents_.push_back({ button, state });
			}

		private:
			void* windowHandle_ = nullptr;
			std::function<void(Key, KeyState)> keyCallback_;
			std::function<void(MouseButton, KeyState)> mouseButtonCallback_;
			std::function<void(double, double)> cursorCallback_;
			std::function<void(double, double)> scrollCallback_;
			std::array<KeyState, MouseButtonCount()> mouseState_{};
			std::pair<double, double> cursorPosition_{ 0.0, 0.0 };
			std::vector<std::pair<MouseButton, KeyState>> queuedMouseEvents_;
		};

		void ResetInputAndWindowSystems()
		{
			// Singletons are reused across the whole engine test process, so each
			// input test tears them down before installing isolated fake backends.
			InputSystem::Instance().Shutdown();
			WindowSystem::Instance().Shutdown();
		}
	}

	void RegisterInputSystemTests(TestRegistry& registry)
	{
		registry.Add("Engine.InputSystem.LatchesQuickMousePressReleasedInSameFrame", [](TestContext& context) {
			ResetInputAndWindowSystems();

			WindowSystem& windowSystem = WindowSystem::Instance();
			windowSystem.Configure(std::make_unique<TestWindowBackend>(), WindowConfig{}, []() {});
			windowSystem.Initialize();

			auto inputBackend = std::make_unique<QueuedInputBackend>();
			QueuedInputBackend* inputBackendPointer = inputBackend.get();

			InputSystem& inputSystem = InputSystem::Instance();
			inputSystem.Configure(std::move(inputBackend));
			inputSystem.Initialize();

			inputBackendPointer->QueueMouseEvent(MouseButton::Left, KeyState::Pressed);
			inputBackendPointer->QueueMouseEvent(MouseButton::Left, KeyState::Released);
			inputSystem.Update(0.016f);

			context.Expect(!inputSystem.IsMouseButtonPressed(MouseButton::Left), "A complete click should leave the physical button released.");
			context.Expect(inputSystem.WasMouseButtonPressedThisFrame(MouseButton::Left), "A press followed by release in one update should remain visible for UI hit testing.");

			inputSystem.Update(0.016f);
			context.Expect(!inputSystem.WasMouseButtonPressedThisFrame(MouseButton::Left), "The frame press latch should clear when no new press is observed.");

			ResetInputAndWindowSystems();
		});

		registry.Add("Engine.InputSystem.PollsCursorPositionWithoutMoveCallback", [](TestContext& context) {
			ResetInputAndWindowSystems();

			WindowSystem& windowSystem = WindowSystem::Instance();
			windowSystem.Configure(std::make_unique<TestWindowBackend>(), WindowConfig{}, []() {});
			windowSystem.Initialize();

			auto inputBackend = std::make_unique<QueuedInputBackend>();
			QueuedInputBackend* inputBackendPointer = inputBackend.get();

			InputSystem& inputSystem = InputSystem::Instance();
			inputSystem.Configure(std::move(inputBackend));
			inputSystem.Initialize();

			inputBackendPointer->SetHardwareCursorPosition(320.0, 180.0);
			inputSystem.Update(0.016f);

			const auto [cursorX, cursorY] = inputSystem.GetCursorPosition();
			context.ExpectEqual(320.0, cursorX, "InputSystem should poll the backend cursor position each frame.");
			context.ExpectEqual(180.0, cursorY, "InputSystem should publish polled cursor Y without requiring a move callback.");

			ResetInputAndWindowSystems();
		});
	}
}
