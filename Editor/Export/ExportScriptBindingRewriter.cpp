// ExportScriptBindingRewriter.cpp - Staged asset rewrite support for exported
// gameplay script bindings.

#include "ExportScriptBindingRewriter.h"

#include <fstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

std::uint64_t ComputeExportScriptBindingId(const std::string& className) {
	const std::uint64_t fnvOffsetBasis = 14695981039346656037ULL;
	const std::uint64_t fnvPrime = 1099511628211ULL;
	std::uint64_t hash = fnvOffsetBasis;
	for (unsigned char value : className) {
		hash ^= static_cast<std::uint64_t>(value);
		hash *= fnvPrime;
	}
	return hash;
}

namespace {
	bool RewriteBindingPayloads(nlohmann::json& node, std::unordered_map<std::uint64_t, std::string>& observedScriptIds, std::string& outError) {
		if (node.is_object()) {
			if (node.contains("components") && node["components"].is_object()) {
				nlohmann::json& components = node["components"];
				if (components.contains("Script") && components["Script"].is_object()) {
					nlohmann::json& scriptComponent = components["Script"];
					if (scriptComponent.contains("BindingsJson") && scriptComponent["BindingsJson"].is_string()) {
						nlohmann::json bindings = nlohmann::json::parse(scriptComponent["BindingsJson"].get<std::string>(), nullptr, false);
						if (bindings.is_discarded() || !bindings.is_array()) {
							outError = "Failed to parse Script.BindingsJson during export staging.";
							return false;
						}

						for (nlohmann::json& binding : bindings) {
							if (!binding.is_object()) {
								continue;
							}

							const std::string className = binding.value("className", std::string{});
							if (className.empty()) {
								if (binding.contains("scriptId") && binding["scriptId"].is_number_unsigned()) {
									continue;
								}

								outError = "Export staging found a script binding without a className.";
								return false;
							}

							const std::uint64_t scriptId = ComputeExportScriptBindingId(className);
							if (auto existing = observedScriptIds.find(scriptId); existing != observedScriptIds.end() && existing->second != className) {
								outError = "NativeAOT export failed because script hash collision " + std::to_string(scriptId) + " maps both '" + existing->second + "' and '" + className + "'.";
								return false;
							}

							observedScriptIds[scriptId] = className;
							binding["scriptId"] = scriptId;
							binding.erase("className");
						}

						scriptComponent["BindingsJson"] = bindings.dump();
					}
				}
			}

			for (auto& [_, child] : node.items()) {
				if (!RewriteBindingPayloads(child, observedScriptIds, outError)) {
					return false;
				}
			}
		}
		else if (node.is_array()) {
			for (nlohmann::json& child : node) {
				if (!RewriteBindingPayloads(child, observedScriptIds, outError)) {
					return false;
				}
			}
		}

		return true;
	}
}

bool RewriteStagedScriptBindings(const std::filesystem::path& stagingDir, std::string& outError) {
	std::unordered_map<std::uint64_t, std::string> observedScriptIds;

	for (const auto& entry : std::filesystem::recursive_directory_iterator(stagingDir)) {
		if (!entry.is_regular_file()) {
			continue;
		}

		const std::filesystem::path extension = entry.path().extension();
		if (extension != ".tescene" && extension != ".teprefab") {
			continue;
		}

		std::ifstream input(entry.path());
		if (!input) {
			outError = "Failed to open staged asset for script rewrite: " + entry.path().string();
			return false;
		}

		nlohmann::json assetJson = nlohmann::json::parse(input, nullptr, false);
		if (assetJson.is_discarded()) {
			outError = "Failed to parse staged asset JSON: " + entry.path().string();
			return false;
		}

		if (!RewriteBindingPayloads(assetJson, observedScriptIds, outError)) {
			outError += " File: " + entry.path().string();
			return false;
		}

		std::ofstream output(entry.path(), std::ios::trunc);
		if (!output) {
			outError = "Failed to write staged asset after script rewrite: " + entry.path().string();
			return false;
		}

		output << assetJson.dump(4);
		if (!output.good()) {
			outError = "Failed to flush staged asset after script rewrite: " + entry.path().string();
			return false;
		}
	}

	return true;
}
