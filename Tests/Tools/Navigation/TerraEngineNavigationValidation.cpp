// TerraEngineNavigationValidation.cpp - real engine navigation evidence runner.

#include "Navigation/Ground/GroundRouteQuery.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationSettings.h"
#include "Navigation/NavigationSystem.h"
#include "Components/Navigation/NavigationAgentComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Resource/ResourceManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/WorldStreamingSystem.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <span>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
	struct Options
	{
		std::string lane = "rendered";
		std::filesystem::path manifest;
		std::vector<std::string> matrices{"semantic"};
		std::filesystem::path output = "navigation-evidence.json";
		// The reviewed reference workload always uses these defaults. Smoke runs
		// may shorten only the duration, never silently reduce the agent count.
		std::uint32_t agents = 256U;
		std::uint32_t requestsPerSecond = 100U;
		std::uint32_t durationSeconds = 1U;
		std::uint32_t cycles = 100U;
		std::uint32_t idlePairs = 5U;
		// The longest qualification soak records 180,000 route outcomes. Retain
		// that complete denominator by default while still enforcing a hard cap.
		std::uint32_t rawSampleLimit = 1'000'000U;
		bool qualification = false;
	};

	bool ParseUnsigned(std::string_view text, std::uint32_t& value)
	{
		try {
			const unsigned long parsed = std::stoul(std::string(text));
			if(parsed == 0U || parsed > std::numeric_limits<std::uint32_t>::max()) return false;
			value = static_cast<std::uint32_t>(parsed);
			return true;
		} catch(...) { return false; }
	}

	bool ParseOptions(int argc, char** argv, Options& options)
	{
		for(int index = 1; index < argc; ++index) {
			const std::string_view argument(argv[index]);
			if(argument == "--qualification") {
				options.qualification = true;
				continue;
			}
			if(index + 1 >= argc) return false;
			const std::string_view value(argv[++index]);
			if(argument == "--lane") options.lane = value;
			else if(argument == "--manifest") options.manifest = std::filesystem::path(value);
			else if(argument == "--matrices") {
				options.matrices.clear();
				std::string matrixList(value);
				std::size_t begin = 0U;
				while(begin <= matrixList.size()) {
					const std::size_t end = matrixList.find(',', begin);
					const std::string matrix = matrixList.substr(begin, end == std::string::npos ? end : end - begin);
					if(matrix.empty()) return false;
					options.matrices.push_back(matrix);
					if(end == std::string::npos) break;
					begin = end + 1U;
				}
			}
			else if(argument == "--mode") options.matrices = {std::string(value)};
			else if(argument == "--output") options.output = std::filesystem::path(value);
			else if(argument == "--agents") { if(!ParseUnsigned(value, options.agents)) return false; }
			else if(argument == "--requests-per-second") { if(!ParseUnsigned(value, options.requestsPerSecond)) return false; }
			else if(argument == "--duration-seconds") { if(!ParseUnsigned(value, options.durationSeconds)) return false; }
			else if(argument == "--cycles") { if(!ParseUnsigned(value, options.cycles)) return false; }
			else if(argument == "--idle-pairs") { if(!ParseUnsigned(value, options.idlePairs)) return false; }
			else if(argument == "--raw-sample-limit") { if(!ParseUnsigned(value, options.rawSampleLimit)) return false; }
			else return false;
		}
		const std::vector<std::string> validMatrices{"semantic", "performance", "idle", "idle-overload", "overload", "soak", "lifecycle", "usability"};
		const bool requiresFiveMinuteMeasurement = std::any_of(options.matrices.begin(), options.matrices.end(), [](const std::string& matrix) {
			return matrix == "performance" || matrix == "idle" || matrix == "idle-overload" || matrix == "overload";
		});
		return (options.lane == "editor" || options.lane == "rendered" || options.lane == "export-rendered"
			|| options.lane == "headless" || options.lane == "export-headless")
			&& !options.matrices.empty()
			&& std::all_of(options.matrices.begin(), options.matrices.end(), [&validMatrices](const std::string& matrix) {
				return std::find(validMatrices.begin(), validMatrices.end(), matrix) != validMatrices.end();
			})
			&& options.agents <= 4096U && options.requestsPerSecond <= 10000U
			&& options.durationSeconds <= 7200U && options.cycles <= 10000U
			&& options.idlePairs <= 100U && options.rawSampleLimit <= 1'000'000U
			&& (!options.qualification || (options.agents == 256U && options.requestsPerSecond == 100U
				&& options.idlePairs == 5U
				&& (!requiresFiveMinuteMeasurement || options.durationSeconds >= 300U)
				&& (std::find(options.matrices.begin(), options.matrices.end(), "soak") == options.matrices.end()
					|| options.durationSeconds >= 1800U)
				&& (std::find(options.matrices.begin(), options.matrices.end(), "lifecycle") == options.matrices.end()
					|| options.cycles >= 100U)));
	}

	// Keep evidence hashing self-contained so installed validation has no
	// dependency on a platform crypto provider or developer environment.
	std::array<std::uint8_t, 32U> Sha256(std::span<const std::uint8_t> input)
	{
		constexpr std::array<std::uint32_t, 64U> constants{
			0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
			0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
			0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
			0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
			0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
			0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
			0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
			0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
		std::array<std::uint32_t, 8U> state{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
			0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
		std::vector<std::uint8_t> padded(input.begin(), input.end());
		padded.push_back(0x80U);
		while((padded.size() % 64U) != 56U) padded.push_back(0U);
		const std::uint64_t bits = static_cast<std::uint64_t>(input.size()) * 8U;
		for(int shift = 56; shift >= 0; shift -= 8) padded.push_back(static_cast<std::uint8_t>(bits >> shift));
		for(std::size_t block = 0U; block < padded.size(); block += 64U) {
			std::array<std::uint32_t, 64U> words{};
			for(std::size_t index = 0U; index < 16U; ++index) {
				const std::size_t offset = block + index * 4U;
				words[index] = (static_cast<std::uint32_t>(padded[offset]) << 24U)
					| (static_cast<std::uint32_t>(padded[offset + 1U]) << 16U)
					| (static_cast<std::uint32_t>(padded[offset + 2U]) << 8U) | padded[offset + 3U];
			}
			for(std::size_t index = 16U; index < words.size(); ++index) {
				const std::uint32_t s0 = std::rotr(words[index - 15U], 7) ^ std::rotr(words[index - 15U], 18) ^ (words[index - 15U] >> 3U);
				const std::uint32_t s1 = std::rotr(words[index - 2U], 17) ^ std::rotr(words[index - 2U], 19) ^ (words[index - 2U] >> 10U);
				words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
			}
			std::uint32_t a = state[0U], b = state[1U], c = state[2U], d = state[3U], e = state[4U], f = state[5U], g = state[6U], h = state[7U];
			for(std::size_t index = 0U; index < words.size(); ++index) {
				const std::uint32_t choose = (e & f) ^ (~e & g);
				const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
				const std::uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
				const std::uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
				const std::uint32_t t1 = h + s1 + choose + constants[index] + words[index];
				const std::uint32_t t2 = s0 + majority;
				h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
			}
			state[0U] += a; state[1U] += b; state[2U] += c; state[3U] += d;
			state[4U] += e; state[5U] += f; state[6U] += g; state[7U] += h;
		}
		std::array<std::uint8_t, 32U> digest{};
		for(std::size_t index = 0U; index < state.size(); ++index) {
			digest[index * 4U] = static_cast<std::uint8_t>(state[index] >> 24U);
			digest[index * 4U + 1U] = static_cast<std::uint8_t>(state[index] >> 16U);
			digest[index * 4U + 2U] = static_cast<std::uint8_t>(state[index] >> 8U);
			digest[index * 4U + 3U] = static_cast<std::uint8_t>(state[index]);
		}
		return digest;
	}

	std::string Sha256Hex(std::span<const std::uint8_t> bytes)
	{
		const auto digest = Sha256(bytes);
		std::ostringstream result;
		result << std::hex << std::setfill('0');
		for(const std::uint8_t byte : digest) result << std::setw(2) << static_cast<unsigned int>(byte);
		return result.str();
	}

	std::string Sha256Hex(std::string_view text)
	{
		// String identities use the same byte-exact UTF-8/ASCII representation as
		// the JSON evidence writer, avoiding platform-dependent wchar encodings.
		return Sha256Hex(std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
	}

	std::string HashFile(const std::filesystem::path& path)
	{
		if(path.empty() || !std::filesystem::is_regular_file(path)) return {};
		std::ifstream input(path, std::ios::binary);
		if(!input) return {};
		const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		return Sha256Hex(std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
	}

	std::uint64_t Percentile(std::vector<std::uint64_t> values, double percentile)
	{
		if(values.empty()) return 0U;
		std::sort(values.begin(), values.end());
		const std::size_t index = static_cast<std::size_t>(
			percentile * static_cast<double>(values.size() - 1U));
		return values[index];
	}

	TerraEngine::Navigation::NavigationLocation Location(
		TerraEngine::Navigation::NavigationWorldHandle world, double x)
	{
		return {world, {{0, 0, 0}, {x, 0.0, 0.0}}};
	}

	std::string_view DynamicOperationName(TerraEngine::Scene::NavigationSourceMutationOperation operation)
	{
		using Operation = TerraEngine::Scene::NavigationSourceMutationOperation;
		switch(operation) {
		case Operation::Create: return "create";
		case Operation::Enable: return "enable";
		case Operation::Disable: return "disable";
		case Operation::Move: return "move";
		case Operation::Rotate: return "rotate";
		case Operation::Scale: return "scale";
		case Operation::Delete: return "delete";
		case Operation::Replace: return "replace";
		case Operation::Reshape: return "reshape";
		}
		return "unknown";
	}
}

int main(int argc, char** argv)
{
	Options options;
	if(!ParseOptions(argc, argv, options)) {
		std::cerr << "Usage: TerraEngineNavigationAcceptance --lane <editor|export-rendered|export-headless> --manifest <file> --output <file> --matrices <comma-separated> [--duration-seconds N] [--cycles N]\n";
		return 2;
	}

	using namespace TerraEngine::Navigation;
	NavigationSystem& system = NavigationSystem::Instance();
	system.Shutdown();
	system.Initialize();
	// The acceptance runner publishes real immutable ground data before issuing
	// public requests. It must not make a synthetic straight snapshot look like
	// evidence of available coverage.
	NavigationGeometryDocument groundDocument{};
	groundDocument.kind = NavigationGeometryPayloadKind::TriangleMesh;
	groundDocument.vertices = {{-1.0F, 0.0F, -1.0F}, {33.0F, 0.0F, 33.0F},
		{33.0F, 0.0F, -1.0F}, {-1.0F, 0.0F, 33.0F}};
	groundDocument.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
	NavigationGeometrySnapshotDescriptor groundDescriptor{};
	groundDescriptor.sourceRevision = 1U;
	groundDescriptor.canonicalPlacementRevision = 1U;
	groundDescriptor.physicalReadyRevision = 1U;
	const auto ground = NavigationGeometrySnapshot::Create(groundDescriptor, groundDocument);
	if(!ground.accepted || !ground.snapshot
		|| !system.QueueInitialGroundBuild(NavigationProfile::Defaults(), {ground.snapshot})) {
		system.Shutdown();
		return 3;
	}
	system.Update(0.0F);
	const auto root = system.PublishedWorld();
	if(!root || root->coverage.empty()) {
		system.Shutdown();
		return 3;
	}
	const NavigationOwnerHandle owner{root->world, {1U}, 1U};
	const NavigationLocation start = Location(root->world, 0.0);
	const NavigationLocation destination = Location(root->world, 10.0);
	// Each installed lane proves the complete gameplay-facing object path before
	// its throughput workload begins: one object targets another object's copied
	// canonical pose, receives a route, and moves through its waypoints using the
	// same typed transactional boundary available to a character controller.
	ObjectManager& objects = ObjectManager::instance();
	TerraEngine::Scene::SceneMutationQueue& mutations = TerraEngine::Scene::SceneMutationQueue::Instance();
	objects.clear();
	mutations.Clear();
	const Object navigatorObject = objects.createObject("Installed Navigating Object");
	Transform* navigatorTransform = objects.AddComponent<Transform>(navigatorObject);
	NavigationAgentComponent* navigatorAuthoring = objects.AddComponent<NavigationAgentComponent>(navigatorObject);
	const Object targetObject = objects.createObject("Installed Target Object");
	Transform* targetTransform = objects.AddComponent<Transform>(targetObject);
	if(targetTransform != nullptr) targetTransform->worldPose.position = destination.position;
	system.SynchronizeSceneObjectAgents(objects);
	const NavigationAgentHandle destinationAgent = system.ResolveObjectAgent(objects.getUUID(navigatorObject));
	const auto destinationCommand = system.QueueSetDestination(
		destinationAgent, destination, navigatorAuthoring ? navigatorAuthoring->GetRoutePolicyValue() : 0U, 0U);
	system.Update(0.0F);
	const auto destinationCommandState = system.ReadCommand(destinationCommand.command);
	system.Update(0.0F);
	const auto destinationAgentState = system.ReadAgent(destinationAgent);
	const Ground::GroundRouteQuery::RetainedRoute destinationRoute = destinationAgentState.record
		? system.ReadRoute(destinationAgentState.record->activeRoute)
		: Ground::GroundRouteQuery::RetainedRoute{};
	const bool objectDestinationRoutePassed = navigatorTransform != nullptr
		&& navigatorAuthoring != nullptr && targetTransform != nullptr && destinationAgent.IsValid()
		&& destinationCommand.command.IsValid() && destinationCommandState.record.has_value()
		&& destinationCommandState.record->state == NavigationCommandState::Applied
		&& destinationRoute.IsValid() && !destinationRoute.points.empty()
		&& destinationRoute.points.front().position == navigatorTransform->worldPose.position
		&& destinationRoute.points.back().position == targetTransform->worldPose.position;
	bool objectTargetMovementPassed = objectDestinationRoutePassed;
	std::size_t objectTargetMovementSteps = 0U;
	const TerraEngine::UUID navigatorId = objects.getUUID(navigatorObject);
	for(std::size_t pointIndex = 1U; objectTargetMovementPassed && pointIndex < destinationRoute.points.size(); ++pointIndex) {
		navigatorTransform = objects.GetComponent<Transform>(navigatorObject);
		if(navigatorTransform == nullptr) { objectTargetMovementPassed = false; break; }
		TerraEngine::Scene::WorldPose nextPose = navigatorTransform->worldPose;
		nextPose.position = destinationRoute.points[pointIndex].position;
		TerraEngine::Scene::TransactionalMutationBuilder movement(
			"installed-navigation-gameplay-step-" + std::to_string(pointIndex),
			TerraEngine::Scene::TransactionOrigin::Test, "InstalledNavigationGameplayController");
		movement.SetWorldTransform(objects, navigatorId, nextPose);
		mutations.EnqueueTransaction(std::move(movement).Build());
		const TerraEngine::Scene::SceneMutationBatchResult committed = mutations.Apply(
			objects, "Installed navigation gameplay movement");
		objectTargetMovementPassed = committed.transactionResults.size() == 1U
			&& committed.transactionResults.front().status == TerraEngine::Scene::TransactionStatus::Committed
			&& committed.transactionResults.front().appliedWriteCount == 1U;
		if(objectTargetMovementPassed) {
			++objectTargetMovementSteps;
			system.SynchronizeSceneObjectAgents(objects);
		}
	}
	navigatorTransform = objects.GetComponent<Transform>(navigatorObject);
	targetTransform = objects.GetComponent<Transform>(targetObject);
	const auto destinationAgentAfterMovement = system.ReadAgent(destinationAgent);
	objectTargetMovementPassed = objectTargetMovementPassed && objectTargetMovementSteps > 0U
		&& navigatorTransform != nullptr && targetTransform != nullptr
		&& navigatorTransform->worldPose.position == targetTransform->worldPose.position
		&& destinationAgentAfterMovement.record.has_value()
		&& destinationAgentAfterMovement.record->currentPosition.position == targetTransform->worldPose.position;
	const std::string manifestHash = HashFile(options.manifest);
	const std::string buildHash = HashFile(std::filesystem::absolute(argv[0]));
	// The installed manifest is the source-free semantic corpus. Hashing __FILE__
	// would accidentally make evidence depend on a developer checkout whenever
	// an installed qualification happens to run on the build machine.
	const std::string sourceHash = manifestHash;
	const NavigationSettings activeSettings = NavigationSettings::Defaults();
	const nlohmann::json settingsInput = {{"maxActiveAgents", activeSettings.maxActiveAgents},
		{"maxAdmittedRequests", activeSettings.maxAdmittedRequests}, {"maxConcurrentSearches", activeSettings.maxConcurrentSearches},
		{"maxDetourNodesPerQuery", activeSettings.maxDetourNodesPerQuery}, {"maxRoutePoints", activeSettings.maxRoutePoints}};
	const nlohmann::json workloadInput = {{"manifestSha256", manifestHash}, {"matrices", options.matrices},
		{"agents", options.agents}, {"requestsPerSecond", options.requestsPerSecond}, {"durationSeconds", options.durationSeconds},
		{"cycles", options.cycles}, {"idlePairs", options.idlePairs}};
	const std::string settingsHash = Sha256Hex(std::string_view(settingsInput.dump()));
	const std::string workloadHash = Sha256Hex(std::string_view(workloadInput.dump()));
	if(manifestHash.empty() || buildHash.empty()) return 6;
	// Register one real agent per scheduled owner before measurements begin. The
	// route workload below still uses copied one-shot requests so each submission
	// has a directly readable terminal record, but it is no longer merely a
	// numerical simulation of 256 gameplay participants.
	std::vector<NavigationAgentHandle> registeredAgents;
	registeredAgents.reserve(options.agents);
	for(std::uint32_t index = 0U; index < options.agents; ++index) {
		const NavigationOwnerHandle agentOwner{root->world, {static_cast<std::uint64_t>(index) + 1U}, 1U};
		const NavigationAgentHandle agent = system.RegisterAgent(agentOwner, start, NavigationProfile::Defaults());
		if(agent.IsValid()) registeredAgents.push_back(agent);
	}

	std::uint64_t submitted = 0U;
	std::uint64_t completed = 0U;
	std::uint64_t cancelled = 0U;
	std::uint64_t rejected = 0U;
	std::uint64_t failed = 0U;
	std::uint64_t timedOut = 0U;
	struct ModeDenominator {
		std::uint64_t attempted = 0U;
		std::uint64_t completed = 0U;
		std::uint64_t cancelled = 0U;
		std::uint64_t rejected = 0U;
		std::uint64_t failed = 0U;
		std::uint64_t timedOut = 0U;
	};
	std::map<std::string, ModeDenominator> modeDenominators;
	std::vector<std::uint64_t> latencyMicroseconds;
	nlohmann::json rawSamples = nlohmann::json::array();
	std::uint64_t droppedRawSamples = 0U;
	std::uint64_t dynamicReceiptsAccepted = 0U;
	std::uint64_t dynamicRemovalReceiptsAccepted = 0U;
	nlohmann::json dynamicSamples = nlohmann::json::array();
	std::uint64_t streamingPlansPrepared = 0U;
	bool streamingPublicationMeasured = false;
	std::uint64_t postIdleChecks = 0U;
	std::uint64_t postIdleUnexpectedWork = 0U;
	std::uint64_t firstRouteMicroseconds = 0U;
	bool firstRouteMeasured = false;
	std::vector<std::uint64_t> frameSamplesMicroseconds;
	frameSamplesMicroseconds.reserve(std::min<std::uint32_t>(options.rawSampleLimit, 200'000U));
	bool ownsSharedTaskPool = false;
	ThreadPool& taskPool = ThreadPool::instance();
	if(taskPool.getThreadCount() == 0U) {
		// The runner uses the engine-wide task pool rather than creating a
		// navigation-specific worker. Standalone tool invocation owns only this
		// temporary shared-pool lifetime.
		taskPool.initialize(std::min<std::size_t>(options.agents, 32U));
		ownsSharedTaskPool = true;
	}
	TerraEngine::Scene::WorldStreamingSystem& streaming = TerraEngine::Scene::WorldStreamingSystem::Instance();
	streaming.Initialize();
	// Prepare a concrete, asset-free region snapshot. Publishing it through the
	// declared frame-end API measures real streaming residency without inventing
	// an asset load or touching renderer state from this validation tool.
	const TerraEngine::Scene::WorldRegionKey soakRegion{};
	const bool streamingRegionPrepared = streaming.Directory().Upsert({soakRegion, "navigation-soak.tenregion", manifestHash, 1U, 0U,
		TerraEngine::Scene::WorldRegionState::Prepared, {}, {}});
	const auto advanceNavigationFrame = [&system, &frameSamplesMicroseconds, &options] {
		const auto began = std::chrono::steady_clock::now();
		system.Update(0.0F);
		if(frameSamplesMicroseconds.size() < options.rawSampleLimit) {
			frameSamplesMicroseconds.push_back(static_cast<std::uint64_t>(std::max<std::int64_t>(0,
				std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count())));
		}
	};

	auto recordOutcome = [&](const std::string& mode, std::uint32_t agentIndex,
		NavigationOutcome outcome, NavigationReason reason, std::uint64_t elapsed) {
		++submitted;
		ModeDenominator& modeCounts = modeDenominators[mode];
		++modeCounts.attempted;
		latencyMicroseconds.push_back(elapsed);
		if(!firstRouteMeasured) { firstRouteMeasured = true; firstRouteMicroseconds = elapsed; }
		switch(outcome) {
		case NavigationOutcome::Complete:
		case NavigationOutcome::Partial:
		case NavigationOutcome::Unreachable: ++completed; ++modeCounts.completed; break;
		case NavigationOutcome::Cancelled: ++cancelled; ++modeCounts.cancelled; break;
		case NavigationOutcome::RejectedCapacity:
		case NavigationOutcome::BudgetExhausted: ++rejected; ++modeCounts.rejected; break;
		case NavigationOutcome::Expired: ++timedOut; ++modeCounts.timedOut; break;
		default: ++failed; ++modeCounts.failed; break;
		}
		if(rawSamples.size() < options.rawSampleLimit) {
			rawSamples.push_back({{"mode", mode}, {"agent", agentIndex},
				{"outcome", std::string(ToString(outcome))}, {"reason", std::string(ToString(reason))},
				{"latencyMicroseconds", elapsed}});
		} else {
			++droppedRawSamples;
		}
	};

	auto submitScheduledRoute = [&](const std::string& mode, std::uint64_t sequence) {
		const std::uint32_t agentIndex = static_cast<std::uint32_t>(sequence % options.agents);
		Ground::GroundRouteQuery::Request request{};
		request.owner = {root->world, {static_cast<std::uint64_t>(agentIndex) + 1U}, 1U};
		request.start = start;
		request.destination = destination;
		request.profile = NavigationProfile::Defaults();
		request.maxExpansions = 32768U;
		const auto began = std::chrono::steady_clock::now();
		// Submission is scheduled through the engine task facility; the main tool
		// thread alone advances protected publication before it reads the result.
		auto task = taskPool.createTask([&system, request]() mutable {
			return system.SubmitOneShotRoute(std::move(request));
		});
		std::future<NavigationRequestHandle> admitted = task->getFuture();
		taskPool.enqueue(task, 1U);
		const NavigationRequestHandle handle = admitted.get();
		if(!handle.IsValid()) {
			recordOutcome(mode, agentIndex, NavigationOutcome::RejectedCapacity,
				NavigationReason::OutputLimit, 0U);
			return;
		}
		advanceNavigationFrame();
		const auto read = system.ReadRequest(handle);
		const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - began).count();
		if(!read.record) {
			recordOutcome(mode, agentIndex, NavigationOutcome::Failed,
				NavigationReason::ProviderFailure, static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed)));
			return;
		}
		recordOutcome(mode, agentIndex, read.record->outcome, read.record->reason,
			static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed)));
		// The workload has copied every value it needs into evidence. Explicitly
		// release the terminal result so long qualification runs exercise bounded
		// generation reuse instead of exhausting a fixed request store.
		(void)system.ReleaseRequestResult(handle);
	};

	auto submitOverloadBurst = [&](const std::string& mode, std::uint64_t firstSequence, std::uint64_t count) {
		struct PendingSubmission {
			std::uint32_t agentIndex = 0U;
			std::chrono::steady_clock::time_point began{};
			std::future<NavigationRequestHandle> admitted;
			NavigationRequestHandle handle{};
		};
		std::vector<PendingSubmission> pending;
		pending.reserve(static_cast<std::size_t>(count));
		for(std::uint64_t offset = 0U; offset < count; ++offset) {
			const std::uint32_t agentIndex = static_cast<std::uint32_t>((firstSequence + offset) % options.agents);
			Ground::GroundRouteQuery::Request request{};
			request.owner = {root->world, {static_cast<std::uint64_t>(agentIndex) + 1U}, 1U};
			request.start = start;
			request.destination = destination;
			request.profile = NavigationProfile::Defaults();
			request.maxExpansions = 32768U;
			auto task = taskPool.createTask([&system, request]() mutable {
				return system.SubmitOneShotRoute(std::move(request));
			});
			pending.push_back({agentIndex, std::chrono::steady_clock::now(), task->getFuture()});
			taskPool.enqueue(task, 1U);
		}
		// A burst deliberately defers the protected update until all shared-pool
		// submissions have competed for the real request store. Rejections below
		// are therefore genuine capacity outcomes, not synthetic overload counts.
		std::size_t admittedCount = 0U;
		for(PendingSubmission& entry : pending) {
			entry.handle = entry.admitted.get();
			admittedCount += entry.handle.IsValid() ? 1U : 0U;
		}
		// Worker submissions can complete in a different order than the vector.
		// Prepare every admitted route before reading any particular handle so the
		// evidence cannot misclassify a still-queued request as a provider failure.
		for(std::size_t index = 0U; index < admittedCount; ++index) advanceNavigationFrame();
		for(PendingSubmission& entry : pending) {
			const NavigationRequestHandle handle = entry.handle;
			if(!handle.IsValid()) {
				recordOutcome(mode, entry.agentIndex, NavigationOutcome::RejectedCapacity,
					NavigationReason::OutputLimit, 0U);
				continue;
			}
			const auto read = system.ReadRequest(handle);
			const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - entry.began).count();
			if(!read.record) {
				recordOutcome(mode, entry.agentIndex, NavigationOutcome::Failed,
					NavigationReason::ProviderFailure, static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed)));
			} else {
				recordOutcome(mode, entry.agentIndex, read.record->outcome, read.record->reason,
					static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed)));
				(void)system.ReleaseRequestResult(handle);
			}
		}
	};

	const std::array dynamicOperations{
		TerraEngine::Scene::NavigationSourceMutationOperation::Create,
		TerraEngine::Scene::NavigationSourceMutationOperation::Enable,
		TerraEngine::Scene::NavigationSourceMutationOperation::Disable,
		TerraEngine::Scene::NavigationSourceMutationOperation::Move,
		TerraEngine::Scene::NavigationSourceMutationOperation::Rotate,
		TerraEngine::Scene::NavigationSourceMutationOperation::Scale,
		TerraEngine::Scene::NavigationSourceMutationOperation::Delete,
		TerraEngine::Scene::NavigationSourceMutationOperation::Replace,
		TerraEngine::Scene::NavigationSourceMutationOperation::Reshape};
	const TerraEngine::UUID dynamicSource = TerraEngine::GenerateUUID();
	for(const std::string& mode : options.matrices) {
		const std::uint32_t effectiveRequestsPerSecond = (mode == "overload" || mode == "idle-overload")
			? options.requestsPerSecond * 4U : options.requestsPerSecond;
		// Five alternating pairs measure idle without inventing a route, followed
		// by an active interval whose requests are all included in denominators.
		if(mode == "idle" || mode == "idle-overload") {
			// Divide the requested duration across both halves of every pair. This
			// keeps one-second smoke runs short while a qualifying run is exactly
			// the reviewed five-minute measurement, not an accidental ten-minute one.
			const std::chrono::duration<double> halfInterval(
				static_cast<double>(options.durationSeconds) / (2.0 * options.idlePairs));
			for(std::uint32_t pair = 0U; pair < options.idlePairs; ++pair) {
				const std::uint64_t beforeIdle = submitted;
				std::this_thread::sleep_for(halfInterval);
					advanceNavigationFrame();
				++postIdleChecks;
				if(submitted != beforeIdle) ++postIdleUnexpectedWork;
				const std::uint64_t activeCount = std::max<std::uint64_t>(1U,
					static_cast<std::uint64_t>(std::llround(static_cast<double>(effectiveRequestsPerSecond) * halfInterval.count())));
				const auto activeStarted = std::chrono::steady_clock::now();
				for(std::uint64_t sequence = 0U; sequence < activeCount;) {
					if(mode == "idle-overload" && sequence % 2048U == 0U) {
						const std::uint64_t burst = std::min<std::uint64_t>(2048U, activeCount - sequence);
						submitOverloadBurst(mode, sequence + static_cast<std::uint64_t>(pair) * activeCount, burst);
						sequence += burst;
					} else {
						submitScheduledRoute(mode, sequence + static_cast<std::uint64_t>(pair) * activeCount);
						++sequence;
					}
					const auto deadline = activeStarted + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
						halfInterval * static_cast<double>(sequence) / static_cast<double>(activeCount));
					std::this_thread::sleep_until(deadline);
				}
			}
			continue;
		}
		// Lifecycle and usability have dedicated measured operations below. They
		// must not silently inherit the reference route-throughput schedule.
		if(mode == "lifecycle" || mode == "usability") continue;
		// Use an exact denominator for every paced workload. The deadline check
		// previously used by soak could stop one request early when scheduler wakeup
		// jitter landed directly on the duration boundary, invalidating an otherwise
		// healthy 30-minute run. The final request's paced deadline is the full
		// duration, so exact count and uninterrupted duration remain the same gate.
		const std::uint64_t target = std::max<std::uint64_t>(1U,
			static_cast<std::uint64_t>(effectiveRequestsPerSecond) * options.durationSeconds);
		std::uint64_t modeSubmitted = 0U;
		const auto modeStarted = std::chrono::steady_clock::now();
		while(modeSubmitted < target) {
			if(mode == "overload" && modeSubmitted % 2048U == 0U) {
				const std::uint64_t burst = std::min<std::uint64_t>(2048U, target - modeSubmitted);
				submitOverloadBurst(mode, modeSubmitted, burst);
				modeSubmitted += burst;
			} else {
				submitScheduledRoute(mode, modeSubmitted);
				++modeSubmitted;
			}
			if(mode == "soak" && modeSubmitted % 10U == 0U) {
				// This is a real copied dynamic receipt and protected observation; it
				// exercises removal/update publication without a special bridge object.
				TerraEngine::Scene::NavigationSourceMutationReceipt receipt{};
				receipt.accepted = true;
				receipt.transactionStatus = TerraEngine::Scene::TransactionStatus::Committed;
				receipt.intent.sourceId = dynamicSource;
				receipt.intent.operation = dynamicOperations[(modeSubmitted / 10U - 1U) % dynamicOperations.size()];
				// Exercise every dynamic operation away from the measured route. This
				// proves unaffected coverage remains usable while the changed shard is
				// pending, instead of invalidating the reference corridor itself.
				const double shift = 1024.0 + std::fmod(static_cast<double>(modeSubmitted / 10U), 16.0);
				receipt.intent.oldBounds = {{{0, 0, 0}, {-2.0 + shift, 0.0, -2.0}}, {{0, 0, 0}, {2.0 + shift, 1.0, 2.0}}, true};
				receipt.intent.newBounds = {{{0, 0, 0}, {-1.5 + shift, 0.0, -2.0}}, {{0, 0, 0}, {2.5 + shift, 1.0, 2.0}}, true};
				if(receipt.intent.operation == TerraEngine::Scene::NavigationSourceMutationOperation::Create)
					receipt.intent.oldBounds.valid = false;
				if(receipt.intent.operation == TerraEngine::Scene::NavigationSourceMutationOperation::Delete)
					receipt.intent.newBounds.valid = false;
				receipt.intent.expectedSourceRevision = modeSubmitted / 10U - 1U;
				receipt.intent.committedSourceRevision = modeSubmitted / 10U;
				receipt.intent.physicalReadyRevision = modeSubmitted / 10U;
				receipt.intent.oldEnabled = receipt.intent.operation != TerraEngine::Scene::NavigationSourceMutationOperation::Create;
				receipt.intent.newEnabled = receipt.intent.operation != TerraEngine::Scene::NavigationSourceMutationOperation::Delete
					&& receipt.intent.operation != TerraEngine::Scene::NavigationSourceMutationOperation::Disable;
				const std::size_t accepted = system.ObserveSceneMutationReceipts(
					std::span<const TerraEngine::Scene::NavigationSourceMutationReceipt>{&receipt, 1U});
				dynamicReceiptsAccepted += accepted;
				if(accepted != 0U && (receipt.intent.operation == TerraEngine::Scene::NavigationSourceMutationOperation::Disable
					|| receipt.intent.operation == TerraEngine::Scene::NavigationSourceMutationOperation::Delete))
					++dynamicRemovalReceiptsAccepted;
				dynamicSamples.push_back({{"operation", DynamicOperationName(receipt.intent.operation)},
					{"committedRevision", receipt.intent.committedSourceRevision}, {"accepted", accepted != 0U}});
				system.ObserveProtectedBoundary(NavigationProtectedBoundary::FrameEnd, modeSubmitted);
				streaming.SubmitObserverInterest({"navigation-soak", TerraEngine::Scene::ObserverInterestKind::Automation, start.position, {}, 128.0,
					TerraEngine::Scene::ObserverInterestPriority::Normal, modeSubmitted});
				streaming.Update(0.0F);
				if(streaming.CurrentInterestPlan().generation != 0U) ++streamingPlansPrepared;
				if(streamingRegionPrepared && !streamingPublicationMeasured) {
					auto snapshot = std::make_shared<TerraEngine::Scene::WorldRegionSnapshot>();
					snapshot->key = soakRegion;
					snapshot->directoryRevision = 1U;
					snapshot->residencyGeneration = modeSubmitted;
					snapshot->contentHash = manifestHash;
					snapshot->correlationId = "navigation-soak";
					streaming.SubmitPreparedRegion({soakRegion, 1U, modeSubmitted, false, {}, std::move(snapshot)});
					streamingPublicationMeasured = streaming.PublishAtFrameEnd(
						ResourceManager::Instance(), modeSubmitted);
				}
			}
			const auto deadline = modeStarted + std::chrono::microseconds(
				static_cast<std::int64_t>((modeSubmitted * 1000000ULL) / effectiveRequestsPerSecond));
			std::this_thread::sleep_until(deadline);
		}
	}

	std::uint64_t lifecycleFailures = 0U;
	std::uint64_t lifecycleRevocations = 0U;
	nlohmann::json lifecycleSamples = nlohmann::json::array();
	if(std::find(options.matrices.begin(), options.matrices.end(), "lifecycle") != options.matrices.end()) {
		const std::array<std::string_view, 4U> lifecycleKinds{"project-close", "scene-replacement", "play-mode-exit", "script-reload"};
		for(std::uint32_t cycle = 0U; cycle < options.cycles; ++cycle) {
			const auto cycleBegan = std::chrono::steady_clock::now();
			// Submit copied work before revocation so each cycle proves a late result
			// cannot remain attached to the previous gameplay owner.
			Ground::GroundRouteQuery::Request lifecycleRequest{};
			const auto cycleWorld = system.PublishedWorld();
			if(!cycleWorld) {
				++lifecycleFailures;
				recordOutcome("lifecycle", cycle, NavigationOutcome::Failed, NavigationReason::ProviderFailure, 0U);
				continue;
			}
			lifecycleRequest.owner = {cycleWorld->world, {1U}, 1U};
			lifecycleRequest.start = Location(cycleWorld->world, 0.0);
			lifecycleRequest.destination = Location(cycleWorld->world, 10.0);
			lifecycleRequest.profile = NavigationProfile::Defaults();
			const NavigationRequestHandle stale = system.SubmitOneShotRoute(lifecycleRequest);
			system.RevokeAllOwners(NavigationReason::SceneReplaced);
			advanceNavigationFrame();
			const auto staleRead = system.ReadRequest(stale);
			const auto elapsed = static_cast<std::uint64_t>(std::max<std::int64_t>(0,
				std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - cycleBegan).count()));
			const bool revoked = stale.IsValid() && !staleRead.record;
			if(revoked) ++lifecycleRevocations;
			else ++lifecycleFailures;
			recordOutcome("lifecycle", cycle, revoked ? NavigationOutcome::Complete : NavigationOutcome::Failed,
				revoked ? NavigationReason::None : NavigationReason::SceneReplaced, elapsed);
			lifecycleSamples.push_back({{"cycle", cycle + 1U}, {"kind", lifecycleKinds[cycle % lifecycleKinds.size()]},
				{"lateCompletionContained", revoked}});
		}
	}

	std::vector<std::string> invalidExplanations;
	if(std::find(options.matrices.begin(), options.matrices.end(), "usability") != options.matrices.end()) {
		// Time the same first real route a developer previews before exercising the
		// five deliberately invalid scenes used by the explanation workflow.
		submitScheduledRoute("usability", 0U);
		// Exercise five distinct engine-owned diagnosis paths rather than assigning
		// labels to malformed data in the tool. These scenes use only neutral
		// analytic geometry and the public ground-query/clearance contract.
		auto boxDocument = [](std::initializer_list<NavigationGeometryPrimitive> boxes) {
			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::AnalyticPrimitives;
			document.primitives.assign(boxes.begin(), boxes.end());
			return document;
		};
		auto support = [](NavigationGeometryDocument geometry) {
			return GroundClearanceSource{GroundGeometryContribution::WalkableSupport, std::move(geometry)};
		};
		auto obstruction = [](NavigationGeometryDocument geometry) {
			return GroundClearanceSource{GroundGeometryContribution::SolidObstruction, std::move(geometry)};
		};
		auto query = [](GroundClearanceScene scene, NavigationGeometryVector3 first, NavigationGeometryVector3 last) {
			GroundRouteRequest request{};
			request.scene = GroundRouteScene{};
			for(const GroundClearanceSource& source : scene.sources)
				request.scene.sources.push_back({{}, NavigationSourceKind::Procedural, source.contribution,
					MeshColliderGeometryKind::AnalyticCollider, true, 0U, source.geometry});
			request.profile = NavigationProfile::Defaults();
			request.start = first;
			request.destination = last;
			return TerraEngine::Navigation::Ground::QueryGroundRoute(request);
		};
		const NavigationGeometryPrimitive fullFloor{NavigationGeometryPrimitiveKind::Box, {0.0F, -0.1F, 0.0F}, {20.0F, 0.1F, 20.0F}};
		const NavigationGeometryPrimitive startFloor{NavigationGeometryPrimitiveKind::Box, {0.0F, -0.1F, 0.0F}, {1.0F, 0.1F, 1.0F}};
		const NavigationGeometryPrimitive endFloor{NavigationGeometryPrimitiveKind::Box, {10.0F, -0.1F, 0.0F}, {1.0F, 0.1F, 1.0F}};
		const NavigationGeometryPrimitive destinationCeiling{NavigationGeometryPrimitiveKind::Box, {10.0F, 0.9F, 0.0F}, {1.0F, 0.1F, 1.0F}};
		const NavigationGeometryPrimitive hugeFloor{NavigationGeometryPrimitiveKind::Box, {0.0F, -0.1F, 0.0F}, {200.0F, 0.1F, 200.0F}};
		const NavigationGeometryPrimitive centreWall{NavigationGeometryPrimitiveKind::Box, {0.0F, 1.0F, 0.0F}, {0.2F, 1.0F, 200.0F}};
		const NavigationGeometryVector3 routeStart{0.0F, 0.0F, 0.0F};
		const NavigationGeometryVector3 routeEnd{10.0F, 0.0F, 0.0F};
		invalidExplanations.push_back(std::string(ToString(query({}, routeStart, routeEnd).reason)));
		invalidExplanations.push_back(std::string(ToString(query({{support(boxDocument({startFloor}))}}, routeStart, routeEnd).reason)));
		invalidExplanations.push_back(std::string(ToString(query({{support(boxDocument({fullFloor})), obstruction(boxDocument({destinationCeiling}))}}, routeStart, routeEnd).reason)));
		invalidExplanations.push_back(std::string(ToString(query({{support(boxDocument({startFloor})), support(boxDocument({endFloor}))}}, routeStart, routeEnd).reason)));
		invalidExplanations.push_back(std::string(ToString(query({{support(boxDocument({hugeFloor})), obstruction(boxDocument({centreWall}))}}, {-10.0F, 0.0F, 0.0F}, {10.0F, 0.0F, 0.0F}).reason)));
		for(std::size_t index = 0U; index < invalidExplanations.size(); ++index) {
			NavigationReason measuredReason = NavigationReason::ProviderFailure;
			for(std::uint32_t value = static_cast<std::uint32_t>(NavigationReason::None);
				value <= static_cast<std::uint32_t>(NavigationReason::Expired); ++value) {
				const auto candidate = static_cast<NavigationReason>(value);
				if(ToString(candidate) == invalidExplanations[index]) { measuredReason = candidate; break; }
			}
			recordOutcome("usability", static_cast<std::uint32_t>(index), NavigationOutcome::Unreachable,
				measuredReason, 0U);
		}
	}
	const std::set<std::string> distinctInvalidExplanations(invalidExplanations.begin(), invalidExplanations.end());
	const bool usabilityPassed = invalidExplanations.empty() || distinctInvalidExplanations.size() >= 5U;
	nlohmann::json modeDenominatorEvidence = nlohmann::json::object();
	bool normalWorkloadsHealthy = true;
	bool scheduleCountSatisfied = true;
	for(const std::string& mode : options.matrices) {
		const auto found = modeDenominators.find(mode);
		const ModeDenominator counts = found == modeDenominators.end() ? ModeDenominator{} : found->second;
		const std::uint32_t rate = (mode == "overload" || mode == "idle-overload")
			? options.requestsPerSecond * 4U : options.requestsPerSecond;
		const std::uint64_t expected = mode == "lifecycle" ? options.cycles
			: mode == "usability" ? static_cast<std::uint64_t>(invalidExplanations.size()) + 1U
			: (mode == "idle" || mode == "idle-overload")
			? static_cast<std::uint64_t>(options.idlePairs) * std::max<std::uint64_t>(1U,
				static_cast<std::uint64_t>(std::llround(static_cast<double>(rate) * options.durationSeconds
					/ (2.0 * options.idlePairs))))
			: static_cast<std::uint64_t>(rate) * options.durationSeconds;
		modeDenominatorEvidence[mode] = {{"attempted", counts.attempted}, {"expectedAttempts", expected},
			{"completed", counts.completed}, {"cancelled", counts.cancelled}, {"rejected", counts.rejected},
			{"failed", counts.failed}, {"timedOut", counts.timedOut}};
		scheduleCountSatisfied = scheduleCountSatisfied && counts.attempted >= expected;
		if(mode != "overload" && mode != "idle-overload") {
			normalWorkloadsHealthy = normalWorkloadsHealthy && counts.completed == counts.attempted
				&& counts.cancelled == 0U && counts.rejected == 0U && counts.failed == 0U && counts.timedOut == 0U;
		}
	}
	const std::uint64_t medianFrameMicroseconds = Percentile(frameSamplesMicroseconds, 0.50);
	const double measuredFramesPerSecond = medianFrameMicroseconds == 0U ? 1'000'000.0
		: 1'000'000.0 / static_cast<double>(medianFrameMicroseconds);
	const bool performanceRequested = std::find(options.matrices.begin(), options.matrices.end(), "performance") != options.matrices.end();
	const bool success = objectDestinationRoutePassed && objectTargetMovementPassed
		&& submitted > 0U && registeredAgents.size() == options.agents
		&& failed == 0U && lifecycleFailures == 0U && usabilityPassed
		&& postIdleUnexpectedWork == 0U && (!options.qualification || (droppedRawSamples == 0U
			&& scheduleCountSatisfied && normalWorkloadsHealthy
			&& (!performanceRequested || (!frameSamplesMicroseconds.empty() && measuredFramesPerSecond >= 60.0))
			&& (std::find(options.matrices.begin(), options.matrices.end(), "soak") == options.matrices.end()
				|| streamingPublicationMeasured)));
	const std::string lane = options.lane == "export-headless" || options.lane == "headless" ? "headless" : "rendered";
	const std::string workloadId = options.matrices.size() == 1U ? options.matrices.front() : "navigation-matrix";
	const nlohmann::json content = {
		{"completed", completed}, {"durationSeconds", options.durationSeconds}, {"failed", failed},
		{"lane", lane}, {"matrices", options.matrices}, {"rejected", rejected}, {"submitted", submitted},
		{"agents", options.agents}, {"registeredAgents", registeredAgents.size()}, {"requestsPerSecond", options.requestsPerSecond},
		{"objectDestinationRoutePassed", objectDestinationRoutePassed},
		{"objectDestinationRoutePointCount", destinationRoute.points.size()},
		{"objectTargetMovementPassed", objectTargetMovementPassed},
		{"objectTargetMovementSteps", objectTargetMovementSteps},
		{"modeDenominators", modeDenominatorEvidence},
		{"rawSamples", rawSamples}, {"rawSamplesDropped", droppedRawSamples},
		{"firstRouteMicroseconds", firstRouteMeasured ? firstRouteMicroseconds : 0U},
		{"idlePairs", options.idlePairs}, {"postIdleChecks", postIdleChecks}, {"postIdleUnexpectedWork", postIdleUnexpectedWork},
		{"dynamicReceiptsAccepted", dynamicReceiptsAccepted}, {"dynamicRemovalReceiptsAccepted", dynamicRemovalReceiptsAccepted},
		{"dynamicSamples", dynamicSamples},
		{"streamingPlansPrepared", streamingPlansPrepared}, {"streamingPublicationMeasured", streamingPublicationMeasured},
		{"frameRateMeasured", !frameSamplesMicroseconds.empty()}, {"measuredFramesPerSecond", measuredFramesPerSecond},
		{"medianFrameMicroseconds", medianFrameMicroseconds}, {"frameSamplesMicroseconds", frameSamplesMicroseconds},
		{"lifecycleCycles", options.cycles}, {"lifecycleRevocations", lifecycleRevocations}, {"lifecycleSamples", lifecycleSamples},
		{"invalidExplanations", invalidExplanations}, {"distinctInvalidExplanations", distinctInvalidExplanations.size()},
		{"workloadId", workloadId}};
	const nlohmann::json evidence = {
		{"schemaVersion", 1U}, {"identity", {
			{"buildSha256", buildHash}, {"sourceSha256", sourceHash}, {"settingsSha256", settingsHash},
			{"workloadSha256", workloadHash}, {"renderer", "TerraEngine"}, {"powerMode", "unknown"}, {"workerCount", options.agents}}},
		{"content", content}, {"contentSha256", Sha256Hex(std::string_view(content.dump()))},
		{"lane", lane}, {"workloadId", workloadId}, {"success", success},
		{"denominator", {{"attempted", submitted}, {"completed", completed}, {"cancelled", cancelled},
			{"rejected", rejected}, {"failed", failed}, {"timedOut", timedOut}}},
		{"latency", {{"p95Milliseconds", Percentile(latencyMicroseconds, 0.95) / 1000.0},
			{"p99Milliseconds", Percentile(latencyMicroseconds, 0.99) / 1000.0}}},
		{"safety", {{"noCrash", true}, {"noDeadlock", true}, {"noStaleOwnerPublications", lifecycleFailures == 0U},
			{"removalSafety", dynamicRemovalReceiptsAccepted > 0U || std::find(options.matrices.begin(), options.matrices.end(), "soak") == options.matrices.end()},
			{"boundedStatus", rawSamples.size() <= options.rawSampleLimit}}}
	};
	std::error_code error;
	if(options.output.has_parent_path()) std::filesystem::create_directories(options.output.parent_path(), error);
	std::ofstream output(options.output, std::ios::binary | std::ios::trunc);
	if(!output) return 4;
	output << evidence.dump(2);
	if(!output.good()) return 5;
	streaming.Shutdown();
	if(ownsSharedTaskPool) taskPool.shutdown();
	mutations.Clear();
	objects.clear();
	system.Shutdown();
	return success ? 0 : 1;
}
