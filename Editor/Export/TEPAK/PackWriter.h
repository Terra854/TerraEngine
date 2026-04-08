#pragma once

#include "TEPAK/PackTypes.h"

namespace tepak {

// PackWriter performs the offline archive build. It discovers files, plans the
// directory and extent layout, and then streams compressed extents to disk.
class PackWriter final {
public:
	// Creates a new pack from the requested input tree and returns the final
	// descriptor plus the metadata that was written into the archive.
	static PackWriteResult Create(const PackagingRequest& request);
};

} // namespace tepak
