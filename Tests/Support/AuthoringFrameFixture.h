#pragma once

#include <imgui.h>
#include <imgui_internal.h>

namespace TerraEngine::EditorTests {
	// Editor UI work is deliberately sequential on its creating thread. A private
	// context isolates focus/input and restores the caller's context on exceptions.
	class AuthoringFrameFixture final {
	public:
		AuthoringFrameFixture() : previous_(ImGui::GetCurrentContext()), context_(ImGui::CreateContext()) {
			// CreateContext restores a pre-existing context; select our owner before
			// configuring IO so nested fixtures never alter their caller's state.
			ImGui::SetCurrentContext(context_);
			auto& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.DisplaySize = ImVec2(1200.0F, 900.0F);
			io.DeltaTime = 1.0F / 60.0F;
			// Current ImGui builds fonts on demand during NewFrame when texture
			// requests are supported. These CPU-only draw tests never submit draw
			// data to a GPU, so the atlas keeps its pending texture requests local.
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
		}
		~AuthoringFrameFixture() {
			ImGui::SetCurrentContext(context_);
			if (context_->WithinFrameScope) {
				// A bounded observation can throw inside Draw before its End call.
				// Unwind only this private context's windows before ending the frame.
				// Recover groups, IDs and style stacks as well as windows. Recovery
				// diagnostics belong to this private context and must not assert in unwinding.
				context_->IO.ConfigErrorRecoveryEnableAssert = false;
				ImGui::ErrorRecoveryTryToRecoverState(&context_->StackSizesInNewFrame);
				ImGui::EndFrame();
			}
			ImGui::DestroyContext(context_);
			ImGui::SetCurrentContext(previous_);
		}
		AuthoringFrameFixture(const AuthoringFrameFixture&) = delete;
		AuthoringFrameFixture& operator=(const AuthoringFrameFixture&) = delete;
		template<typename Draw> void Frame(Draw&& draw, float delta = 1.0F / 60.0F) {
			ImGui::SetCurrentContext(context_);
			ImGui::GetIO().DeltaTime = delta;
			ImGui::NewFrame();
			try { draw(); }
			catch (...) { ImGui::SetCurrentContext(context_); throw; }
			// A draw callback may temporarily switch contexts. End only our frame;
			// exceptional window-stack cleanup remains owned by the destructor.
			ImGui::SetCurrentContext(context_);
			ImGui::EndFrame();
		}
		static void PlaceWindow() {
			ImGui::SetNextWindowPos(ImVec2(20.0F, 20.0F), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(800.0F, 650.0F), ImGuiCond_Always);
		}
	private:
		ImGuiContext* previous_;
		ImGuiContext* context_;
	};
}
