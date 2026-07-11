#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Core/ViewportTextureRegistry.h"

namespace TerraEngine::EditorTests
{
	void RegisterViewportTextureRetirementTests(TestRegistry& registry)
	{
		registry.Add("Editor.ViewportTextureRetirement.GenerationAndFenceOwnership", [](TestContext& context) {
			TerraEngine::Editor::ViewportTextureRegistry registryUnderTest;
			std::uintptr_t nextDescriptor = 100;
			auto add = [&](void*, void*) { return reinterpret_cast<void*>(nextDescriptor++); };
			std::size_t releases = 0;
			auto release = [&](void*) { ++releases; return 0; };
			registryUnderTest.Refresh(TerraEngine::Editor::ViewportTextureKind::Scene,
				reinterpret_cast<void*>(1), reinterpret_cast<void*>(2), 1, 0, add);
			void* oldDescriptor = registryUnderTest.ActiveDescriptor(TerraEngine::Editor::ViewportTextureKind::Scene);
			registryUnderTest.BeginDrawDataUsage();
			registryUnderTest.MarkDrawDataUse(oldDescriptor);
			registryUnderTest.Refresh(TerraEngine::Editor::ViewportTextureKind::Scene,
				reinterpret_cast<void*>(1), reinterpret_cast<void*>(2), 2, 4, add);
			registryUnderTest.CommitDrawDataUsage(7);
			context.ExpectEqual(static_cast<std::size_t>(0), registryUnderTest.ReleaseCompleted(6, release), "A descriptor used by an in-flight UI submission must remain alive.");
			context.ExpectEqual(static_cast<std::size_t>(1), registryUnderTest.ReleaseCompleted(7, release), "Fence completion should release it exactly once.");
			context.ExpectEqual(static_cast<std::size_t>(1), releases, "Retirement must be exactly once.");
		});

		registry.Add("Editor.ViewportTextureRetirement.RegistrationFailureKeepsActive", [](TestContext& context) {
			TerraEngine::Editor::ViewportTextureRegistry registryUnderTest;
			registryUnderTest.Refresh(TerraEngine::Editor::ViewportTextureKind::Game,
				reinterpret_cast<void*>(1), reinterpret_cast<void*>(2), 1, 0,
				[](void*, void*) { return reinterpret_cast<void*>(20); });
			void* previous = registryUnderTest.ActiveDescriptor(TerraEngine::Editor::ViewportTextureKind::Game);
			const bool replaced = registryUnderTest.Refresh(TerraEngine::Editor::ViewportTextureKind::Game,
				reinterpret_cast<void*>(3), reinterpret_cast<void*>(4), 2, 0,
				[](void*, void*) -> void* { return nullptr; });
			context.Expect(!replaced, "Failed registration must be reported.");
			context.ExpectEqual(previous, registryUnderTest.ActiveDescriptor(TerraEngine::Editor::ViewportTextureKind::Game), "The last valid pair must stay active.");
		});
	}
}
