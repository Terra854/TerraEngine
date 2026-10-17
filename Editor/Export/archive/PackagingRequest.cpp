// PackagingRequest.cpp — TEPAK archive packaging request implementation
//
// Provides the implementation for building a packaging inventory from a user request. it validates core sizing and worker settings before initiating the filesystem walk and sample collection process.

#include "archive/ArchiveWorkflow.h"

#include "codec/CodecProvider.h"
#include "io/PathNormalizer.h"

#include <algorithm>
#include <stdexcept>

namespace tepak::archive {


model::PackagingInventory BuildPackagingInventory(const PackagingRequest& request) {
	// Validate the high-level request once up front so the discovery pass can
	// assume the core sizing and worker settings are already sane.

	if (request.inputRoot.empty()) {
		throw std::runtime_error("The packaging request must declare an input root.");
	}

	if (request.outputPackPath.empty()) {
		throw std::runtime_error("The packaging request must declare an output pack path.");
	}

	if (request.workerCount == 0U) {
		throw std::runtime_error("The packaging request must declare at least one worker.");
	}

	if (request.extentSizingBytes < 4096U) {
		throw std::runtime_error("The extent size must be at least 4096 bytes.");
	}

	if (request.peakMemoryBudgetBytes == 0U) {
		throw std::runtime_error("The peak memory budget must be greater than zero.");
	}

	// The filesystem walk lives in the io layer because it owns TEPAK's path
	// normalization rules and sample collection policy.
	return io::DiscoverPackagingInventory(request);
}

} // namespace tepak::archive
