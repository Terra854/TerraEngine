#pragma once

#if defined(TERRAENGINE_EDITOR_TESTS)
#include <span>
namespace TerraEngine::EditorTests {
	// A bounded observer can reject an unterminated production buffer before an
	// ImGui string read. It is installed only by serialized native editor tests.
	using InspectorNameObserver = void (*)(std::span<const char, 256>);
	void SetInspectorNameObserver(InspectorNameObserver observer) noexcept;
}
#endif
