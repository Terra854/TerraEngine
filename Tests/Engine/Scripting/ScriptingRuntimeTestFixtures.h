#pragma once

#include "Components/ScriptComponent.h"
#include "Core/Object/Object.h"

#include <string_view>

namespace TerraEngine::Tests {
	class ScriptingRuntimeTestScope {
	public:
		ScriptingRuntimeTestScope();
		~ScriptingRuntimeTestScope();
	};

	Object CreateScriptedRuntimeObject(std::string_view objectName, std::string_view className);
	std::uint64_t CaptureObjectMetadataGeneration(const Object& object);
	std::uint64_t CaptureObjectComponentGeneration(const Object& object);
	std::uint64_t CaptureObjectBindingGeneration(const Object& object);
}
