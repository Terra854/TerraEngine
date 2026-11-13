#include "TestHarness.h"
#include "Tests/Support/AuthoringFrameFixture.h"
#include "Tests/Support/InspectorNameObservation.h"
#include "Editor/TerraEngine-Editor/Windows/HierarchyWindow.h"
#include "Editor/TerraEngine-Editor/Windows/InspectorWindow.h"
#include "Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.h"
#include "Editor/TerraEngine-Editor/Subsystems/UndoRedoSystem.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"

#include <cstring>
#include <string>

namespace TerraEngine::EditorTests {
	namespace {
		struct SelectionFixture final {
			MainEditorSubsystem editor;
			ObjectManager& objects = ObjectManager::instance();
			Object object;
			TerraEngine::UUID uuid{};
			SelectionFixture() {
				ComponentRegistry::RegisterAllComponents();
				UndoRedoSystem::Instance().Clear();
				object = objects.createObject("Input routing selected object");
				uuid = objects.getUUID(object);
				editor.SetSelectedEntity(object.EntityId());
			}
			~SelectionFixture() {
				editor.SetSelectedEntity(std::nullopt);
				UndoRedoSystem::Instance().Clear();
				if (const auto remaining = objects.getObject(uuid)) objects.removeObject(*remaining);
			}
		};
		struct NameObservation final {
			static inline bool observed = false;
			NameObservation() {
				observed = false;
				SetInspectorNameObserver([](std::span<const char, 256> buffer) {
					observed = true;
					// Fail before InputText can read past the actual production buffer.
					if (!std::memchr(buffer.data(), '\0', buffer.size())) throw TestFailure("Inspector name input must be terminated within its bounded storage.");
				});
			}
			~NameObservation() { SetInspectorNameObserver(nullptr); }
		};
	}
	void RegisterHierarchyEditInputTests(TestRegistry& registry) {
		registry.Add("Editor.Hierarchy.AuthoringFixtureOwnsItsContextAcrossCallbacksAndExceptions", [](TestContext& context) {
			// The outer fixture supplies a real caller context and restores the test
			// harness even if a later assertion fails.
			AuthoringFrameFixture caller;
			auto* parent = ImGui::GetCurrentContext();
			ImGui::GetIO().DisplaySize = ImVec2(333.0F, 222.0F);
			{
				AuthoringFrameFixture nested;
				auto* child = ImGui::GetCurrentContext();
				context.Expect(child != parent && parent->IO.DisplaySize.x == 333.0F, "Nested initialization must configure its own IO without changing the caller.");
				ImGui::SetCurrentContext(parent);
				nested.Frame([&] {
					context.Expect(ImGui::GetCurrentContext() == child, "Frame must explicitly select its private owner.");
					ImGui::SetCurrentContext(parent);
				});
				context.Expect(!child->WithinFrameScope && !parent->WithinFrameScope, "Callback context switches cannot end or start the caller's frame.");
			}
			context.Expect(ImGui::GetCurrentContext() == parent, "Ordinary teardown restores the caller context.");
			bool caught = false;
			try {
				AuthoringFrameFixture interrupted;
				interrupted.Frame([&] {
					ImGui::Begin("Interrupted private window");
					// Keep several nested stacks open to exercise assertion-enabled recovery.
					ImGui::BeginGroup(); ImGui::PushID("interrupted-group");
					ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5F);
					ImGui::SetCurrentContext(parent);
					throw TestFailure("Expected callback interruption");
				});
			} catch (const TestFailure&) { caught = true; }
			context.Expect(caught && ImGui::GetCurrentContext() == parent && !parent->WithinFrameScope, "Exceptional teardown unwinds only the private frame and restores its caller.");
		});
		registry.Add("Editor.Hierarchy.DeleteInOtherTextWindowPreservesSelection", [](TestContext& context) {
			AuthoringFrameFixture frames;
			SelectionFixture selection;
			HierarchyWindow hierarchy(&selection.editor);
			hierarchy.SetVisible(true);
			char text[64] = "editable";
			bool editing = false;
			auto draw = [&] {
				AuthoringFrameFixture::PlaceWindow(); hierarchy.Draw();
				ImGui::SetNextWindowPos(ImVec2(850.0F, 20.0F), ImGuiCond_Always);
				ImGui::SetNextWindowSize(ImVec2(300.0F, 180.0F), ImGuiCond_Always);
				ImGui::Begin("Other text editor");
				ImGui::SetWindowFocus();
				ImGui::SetKeyboardFocusHere();
				ImGui::InputText("Text", text, sizeof(text));
				editing = ImGui::IsItemActive();
				ImGui::End();
			};
			frames.Frame(draw); frames.Frame(draw);
			context.Expect(editing, "A real text field in another window must own focus.");
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Delete, true); frames.Frame(draw);
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Delete, false); frames.Frame(draw);
			context.Expect(selection.objects.getObject(selection.object.EntityId()).has_value(), "Delete routed to another window's text input must preserve the selected scene object.");
		});
		registry.Add("Editor.Hierarchy.FocusedDeleteRemainsUndoable", [](TestContext& context) {
			AuthoringFrameFixture frames;
			SelectionFixture selection;
			HierarchyWindow hierarchy(&selection.editor);
			hierarchy.SetVisible(true);
			auto draw = [&] {
				AuthoringFrameFixture::PlaceWindow();
				ImGui::SetNextWindowFocus();
				hierarchy.Draw();
			};
			frames.Frame(draw); frames.Frame(draw);
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Delete, true); frames.Frame(draw);
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Delete, false); frames.Frame(draw);
			context.Expect(!selection.objects.getObject(selection.uuid), "Focused hierarchy Delete must still delete its selection.");
			context.Expect(UndoRedoSystem::Instance().CanUndo(), "Hierarchy deletion must retain an undo command.");
			UndoRedoSystem::Instance().Undo();
			context.Expect(selection.objects.getObject(selection.uuid).has_value(), "Undo must restore the deleted stable object identity.");
		});
		for (const std::size_t size : {255U, 256U, 1024U}) {
			registry.Add("Editor.Inspector.NameBufferBounded." + std::to_string(size), [size](TestContext& context) {
				AuthoringFrameFixture frames;
				SelectionFixture selection;
				const std::string original(size, 'N');
				selection.objects.setObjectName(selection.object, original);
				InspectorWindow inspector(&selection.editor);
				inspector.SetVisible(true);
				NameObservation observation;
				frames.Frame([&] { AuthoringFrameFixture::PlaceWindow(); inspector.Draw(); });
				context.Expect(NameObservation::observed, "Actual Inspector draw must submit the selected object's name buffer.");
				context.ExpectEqual(original, selection.objects.getObjectName(selection.object), "Viewing a long name must preserve the authored name until an edit.");
				const auto draw = [&] { AuthoringFrameFixture::PlaceWindow(); inspector.Draw(); };
				auto* nativeWindow = ImGui::FindWindowByName("Inspector");
				context.Expect(nativeWindow != nullptr, "Actual Inspector window must exist.");
				// Click the real name field, select its text, and type a replacement.
				// Separate event frames mirror the ordinary keyboard routing contract.
				const ImVec2 click(nativeWindow->DC.CursorStartPos.x + 100.0F, nativeWindow->DC.CursorStartPos.y + 8.0F);
				ImGui::GetIO().AddMousePosEvent(click.x, click.y);
				ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true); frames.Frame(draw);
				ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false); frames.Frame(draw);
				ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true); frames.Frame(draw);
				ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true); frames.Frame(draw);
				ImGui::GetIO().AddKeyEvent(ImGuiKey_A, false);
				ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false); frames.Frame(draw);
				ImGui::GetIO().AddInputCharactersUTF8("Renamed"); frames.Frame(draw);
				context.ExpectEqual(std::string("Renamed"), selection.objects.getObjectName(selection.object), "A real text edit must produce a valid bounded object name.");
			});
		}
	}
}
