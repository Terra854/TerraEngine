// Opt-in native platform regression; no Vulkan device or editor is created.
// Configure: cmake -S . -B <build> -DTERRAENGINE_BUILD_NATIVE_GLFW_RECOVERY_TEST=ON
// Build/install: cmake --build <build> --target install --config RelWithDebInfo
// Run: ctest --test-dir <build> -R ^Editor.RendererEfficiency.NativeGlfwRecovery$ --output-on-failure
// Installed executable: Build/Tests/Editor/NativeGlfwRecovery/TerraEngine-RendererEfficiencyGlfwRecoveryTests.exe
#include "Tests/Support/RendererEfficiency/Editor/RendererEfficiencyDeviceRecreation.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <iostream>
#include <stdexcept>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

namespace
{
	void Require(bool ready, const char* reason)
	{
		if(!ready) throw std::runtime_error(reason);
	}
	void KeyCallback(GLFWwindow*, int, int, int, int) {}
	void FocusCallback(GLFWwindow*, int) {}
	// Native resources are local and sequential because GLFW/ImGui platform
	// callbacks belong to their creating thread. No live engine mutation occurs.
	struct PlatformFixture
	{
		GLFWwindow* window = nullptr;
		ImGuiContext* context = nullptr;
		bool initialized = false;
		~PlatformFixture()
		{
			if(context)
			{
				ImGui::SetCurrentContext(context);
				if(ImGui::GetIO().BackendPlatformUserData) ImGui_ImplGlfw_Shutdown();
				ImGui::DestroyContext(context);
			}
			if(window) glfwDestroyWindow(window);
			if(initialized) glfwTerminate();
		}
	};
}

int main()
{
	try
	{
		PlatformFixture fixture;
		Require(glfwInit() == GLFW_TRUE, "GLFW initialization requires an available native desktop.");
		fixture.initialized = true;
		glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		fixture.window = glfwCreateWindow(64, 64, "TerraEngine hidden platform recovery test", nullptr, nullptr);
		Require(fixture.window != nullptr, "Hidden NO_API native window could not be created.");
		int ownerMarker = 17;
		glfwSetWindowUserPointer(fixture.window, &ownerMarker);
		glfwSetKeyCallback(fixture.window, KeyCallback);
		glfwSetWindowFocusCallback(fixture.window, FocusCallback);
#if defined(_WIN32)
		const auto nativeWindow = glfwGetWin32Window(fixture.window);
		const auto originalWndProc = GetWindowLongPtrW(nativeWindow, GWLP_WNDPROC);
#endif
		fixture.context = ImGui::CreateContext();
		ImGui::GetIO().IniFilename = nullptr; // The validation tool writes no UI state.
		ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
		Require(ImGui_ImplGlfw_InitForVulkan(fixture.window, false), "Actual initial GLFW platform backend failed.");
		auto* viewport = ImGui::GetMainViewport();
		Require(viewport->PlatformHandle == fixture.window && viewport->PlatformUserData, "Initial main viewport has no real window ownership.");
		// This is the actual main-viewport destruction performed by Vulkan backend
		// shutdown. The primary GLFW window remains alive because it is not owned
		// by ImGui, but its platform userdata/handle are intentionally cleared.
		ImGui::DestroyPlatformWindows();
		Require(!viewport->PlatformHandle && !viewport->PlatformUserData, "Actual platform destruction did not expose the old recovery failure.");
		const bool rebound = RebindRendererEfficiencyPlatformBackend(fixture.window,
			[] { ImGui_ImplGlfw_Shutdown(); },
			[&] { return ImGui_ImplGlfw_InitForVulkan(fixture.window, false); },
			[&] { return ImGui::GetIO().BackendPlatformUserData && viewport->PlatformUserData && viewport->PlatformHandle == fixture.window; });
		Require(rebound, "Production recovery did not restore actual main viewport handle/userdata.");
		Require(glfwGetWindowUserPointer(fixture.window) == &ownerMarker, "Recovery replaced the native window's external owner.");
		Require(glfwSetKeyCallback(fixture.window, KeyCallback) == KeyCallback, "Recovery replaced the editor-owned key callback.");
		Require(glfwSetWindowFocusCallback(fixture.window, FocusCallback) == FocusCallback, "Recovery replaced the editor-owned focus callback.");
		// Only call the real backend frame operation after actual ownership is
		// proven. The old code fails above rather than deliberately crashing here.
		ImGui_ImplGlfw_NewFrame();
		Require(ImGui::GetIO().DeltaTime > 0 && viewport->PlatformHandle == fixture.window, "Actual recovered GLFW NewFrame did not remain usable.");
		ImGui_ImplGlfw_Shutdown();
#if defined(_WIN32)
		Require(GetWindowLongPtrW(nativeWindow, GWLP_WNDPROC) == originalWndProc, "Cleanup did not restore the original native window procedure.");
#endif
		std::cout << "[PASS] Renderer.Efficiency.NativeGlfwRecovery.ActualMainViewportReboundAndNewFrameSafe\n";
		// These bounded failure checks exercise the same production helper, not
		// native success. A failed initializer cannot authorize the next frame.
		bool touched = false;
		Require(!RebindRendererEfficiencyPlatformBackend(static_cast<GLFWwindow*>(nullptr), [&] { touched = true; }, [] { return true; }, [] { return true; }) && !touched,
			"Missing native owner touched platform resources.");
		bool validated = false;
		Require(!RebindRendererEfficiencyPlatformBackend(fixture.window, [] {}, [] { return false; }, [&] { validated = true; return true; }) && !validated,
			"Failed initialization advertised usable native ownership.");
		Require(!RebindRendererEfficiencyPlatformBackend(fixture.window, [] {}, [] { return true; }, [] { return false; }),
			"Failed native readiness advertised completion.");
		std::cout << "[PASS] Renderer.Efficiency.NativeGlfwRecovery.InvalidOwnerInitAndReadinessFailClosed\n";
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << "[FAIL] Renderer.Efficiency.NativeGlfwRecovery: " << error.what() << '\n';
		return 1;
	}
}