// PackWriter.h — Public TEPAK archive writer interface
//
// Defines the entry point for creating new TEPAK archives from a source directory tree. it manages the end-to-end packaging process, returning the final archive descriptor and detailed diagnostic information about the built pack.

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
