#pragma once

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace TerraEngine::Editor::Assets {
	// Package candidates are unpublished, mutable compiler storage. Match only
	// the reserved UUID/token shape so ordinary authored dot-folders stay visible.
	inline bool IsModelPackageTransactionPath(const std::filesystem::path& path) {
		for(const auto& part : path) {
			const std::string name = part.string();
			if((name.size() != 36U && name.size() != 20U) || !name.starts_with(".ta-")) continue;
			if(std::all_of(name.begin() + 4, name.end(), [](char value) {
				return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
			})) return true;
		}
		return false;
	}

	// Both browser listings use one admission boundary before asking whether an
	// entry is a directory. The probe retains ordinary filesystem error behavior.
	template<class DirectoryProbe>
	std::optional<bool> InspectProjectBrowserEntry(const std::filesystem::path& path, DirectoryProbe&& probe) {
		// Reject lexically before a status query: a compiler may be renaming or
		// deleting this private root, and it is never an independently editable asset.
		if(IsModelPackageTransactionPath(path)) return std::nullopt;
		return std::forward<DirectoryProbe>(probe)(path);
	}
}
