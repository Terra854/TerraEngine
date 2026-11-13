// Bounded child fixture for the soak script's process/JSON boundary. It writes
// deliberately unsupported or malformed protocol inputs, never terrain metrics.
// Build this standalone C++20 file and pass its executable to TestTerrainSoakFailFast.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

int main(int argc, char** argv)
{
	std::filesystem::path output;
	std::string route;
	for (int index = 1; index + 1 < argc; ++index)
	{
		const std::string flag = argv[index];
		if (flag == "--output") output = argv[++index];
		else if (flag == "--route") route = argv[++index];
	}
	if (output.empty()) return 3;
	const bool generator = std::filesystem::path(argv[0]).filename().string().find("FixtureGenerator") != std::string::npos;
	if (const char* counter = std::getenv("TERRAENGINE_SOAK_PROTOCOL_CALLS"))
	{
		std::ofstream calls(counter, std::ios::app);
		calls << (generator ? "generator" : "benchmark") << '\n';
	}
	const char* configured = std::getenv("TERRAENGINE_SOAK_PROTOCOL_MODE");
	const std::string mode = configured ? configured : "unavailable";
	if (!generator && mode == "exit-failure") return 7;
	if (!generator && mode == "missing-output") return 0;
	std::filesystem::create_directories(output.parent_path());
	std::ofstream result(output, std::ios::trunc);
	if (generator) result << R"({"qualification":"protocol-only sparse fixture"})";
	else if (mode == "malformed") result << '{';
	else if (mode == "missing-status") result << R"({"route":"protocol-only","distanceKilometres":null})";
	else if (mode == "invalid-route") result << R"({"status":"available","route":"wrong-protocol-route","distanceKilometres":0,"rawMetrics":null})";
	else
	{
		// A null route distance is intentional: this fixture must never manufacture
		// a successful 1000 km traversal, even for a script-unit positive control.
		result << "{\"status\":\"unavailable\",\"route\":\"" << route << "\",\"distanceKilometres\":null";
		if (mode != "unavailable-no-qualification") result << R"(,"qualification":"protocol fixture; no live terrain route executed")";
		result << '}';
	}
	return result.good() ? 0 : 8;
}
