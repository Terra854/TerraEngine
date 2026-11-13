// Validation-only thread-local preallocation observation; no class ABI changes.
#pragma once
#include "../Core/Export.h"
#include "../File Formats/TerraEngineMeshFile.h"
namespace TerraEngine::Resource::Review {
	struct DecodeBoundaryObserver {
		bool (*beforeAllocation)(const TemfBlockDescriptor&, void*) noexcept = nullptr;
		void* user = nullptr;
	};
	// Caller owns context and restores the prior observer on this same thread.
	// Returning false safely aborts only this explicit review decode invocation.
	TERRAENGINE_API DecodeBoundaryObserver SetDecodeBoundaryObserver(DecodeBoundaryObserver observer) noexcept;
}
