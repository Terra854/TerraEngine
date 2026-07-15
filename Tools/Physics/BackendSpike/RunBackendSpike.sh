#!/usr/bin/env bash
# Runs the same disposable comparison on Linux. Providers stay at caller paths;
# this script only creates a hermetic build/install/evidence tree beneath --work-root.
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: RunBackendSpike.sh --candidate {all|jolt55|jolt56|physx58} [options]

Required provider paths: --jolt55-source, --jolt56-source, --physx58-source
Jolt 5.6 is pinned by the approved plan. The optional --jolt56-revision
argument remains for reproducibility and must match that pin.

Options:
  --work-root PATH             Disposable output root (default: out/physics-backend-spike)
  --run-id NAME                Child output directory (default: backend-spike)
  --configuration NAME         Debug, Release, RelWithDebInfo, or ReleaseASAN (default: Release)
  --generator NAME             Explicit CMake generator; default prefers Ninja when available
  --fixture-command PATH       Optional executable for feature/lifecycle fixtures
  --fixture-argument VALUE     Argument forwarded to the fixture hook (repeatable)
  --measurement-command PATH   Optional executable for performance/query/large-world measurements
  --measurement-argument VALUE Argument forwarded to the measurement hook (repeatable)
  --fixture-repeats COUNT      Separate processes per worker limit (default: 3)
  --fixture-timeout-seconds N  Watchdog for each fixture process (default: 180)
  --double-precision           Configure this isolated Jolt run with double positions
  --dry-run                    Record commands without configuring providers
  --validate-harness-only      Configure/build/install the metadata harness without a provider
  --help                       Show this help
EOF
}

candidate=all; jolt55_source=; jolt56_source=; physx58_source=; jolt56_revision=e77f175595e64cb44218cc9d9d56fc365ad0e36a
work_root=out/physics-backend-spike; run_id=backend-spike; configuration=Release; generator=
fixture_command=; measurement_command=; fixture_repeats=3; fixture_timeout_seconds=180; double_precision=0; dry_run=0; validate_only=0
fixture_arguments=(); measurement_arguments=()
while [[ $# -gt 0 ]]; do
	case "$1" in
		--candidate) candidate="$2"; shift 2 ;;
		--jolt55-source) jolt55_source="$2"; shift 2 ;;
		--jolt56-source) jolt56_source="$2"; shift 2 ;;
		--physx58-source) physx58_source="$2"; shift 2 ;;
		--jolt56-revision) jolt56_revision="$2"; shift 2 ;;
		--work-root) work_root="$2"; shift 2 ;;
		--run-id) run_id="$2"; shift 2 ;;
		--configuration) configuration="$2"; shift 2 ;;
		--generator) generator="$2"; shift 2 ;;
		--fixture-command) fixture_command="$2"; shift 2 ;;
		--fixture-argument) fixture_arguments+=("$2"); shift 2 ;;
		--measurement-command) measurement_command="$2"; shift 2 ;;
		--measurement-argument) measurement_arguments+=("$2"); shift 2 ;;
		--fixture-repeats) fixture_repeats="$2"; shift 2 ;;
		--fixture-timeout-seconds) fixture_timeout_seconds="$2"; shift 2 ;;
		--double-precision) double_precision=1; shift ;;
		--dry-run) dry_run=1; shift ;;
		--validate-harness-only) validate_only=1; shift ;;
		--help|-h) usage; exit 0 ;;
		*) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
	esac
done

case "$candidate" in all|jolt55|jolt56|physx58) ;; *) echo "Invalid --candidate: $candidate" >&2; exit 2;; esac
case "$configuration" in Debug|Release|RelWithDebInfo|ReleaseASAN) ;; *) echo "Invalid --configuration: $configuration" >&2; exit 2;; esac
[[ "$run_id" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || { echo "Invalid --run-id." >&2; exit 2; }
[[ "$fixture_repeats" =~ ^[1-9][0-9]*$ && "$fixture_repeats" -le 10 ]] || { echo "--fixture-repeats must be 1-10." >&2; exit 2; }
[[ "$fixture_timeout_seconds" =~ ^[1-9][0-9]*$ && "$fixture_timeout_seconds" -ge 10 && "$fixture_timeout_seconds" -le 900 ]] || { echo "--fixture-timeout-seconds must be 10-900." >&2; exit 2; }

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd -P)
harness_source="$repo_root/Tools/Physics/BackendSpike"
absolute_path() { [[ "$1" = /* ]] && printf '%s\n' "$1" || printf '%s/%s\n' "$2" "$1"; }
work_root=$(absolute_path "$work_root" "$repo_root")
run_root="$work_root/$run_id"
[[ "$run_root" == "$work_root/"* ]] || { echo "Output escapes --work-root." >&2; exit 2; }

write_json() { printf '%s\n' "$2" > "$1"; }
run_logged() {
	local stage="$1" log="$2"; shift 2
	printf '{"stage":"%s","executable":"%s"}\n' "$stage" "$1" > "$log.command.json"
	if [[ "$dry_run" == 1 ]]; then printf 'DRY RUN:' > "$log"; printf ' %q' "$@" >> "$log"; printf '\n' >> "$log"; last_elapsed_ms=0; return; fi
	local start_ns end_ns
	start_ns=$(date +%s%N)
	"$@" >"$log" 2>&1
	end_ns=$(date +%s%N)
	last_elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
}
run_fixture_process() {
	# Run JSON-producing fixture separately from generic commands: stdout remains
	# parseable while /proc sampling records a best-effort peak RSS on Linux.
	local executable="$1" log="$2" workers="$3" start_ns end_ns pid status peak_kb=0
	start_ns=$(date +%s%N)
	timeout --signal=TERM "${fixture_timeout_seconds}s" "$executable" "--workers=$workers" >"$log" 2>"$log.stderr" &
	pid=$!
	while kill -0 "$pid" 2>/dev/null; do
		# The background PID belongs to `timeout`; its direct child is the actual
		# fixture. Sample both so the wrapper's tiny RSS cannot masquerade as the
		# provider process high-water mark.
		local sampled_pid sampled_kb child_pids=""
		[[ -r "/proc/$pid/task/$pid/children" ]] && child_pids=$(<"/proc/$pid/task/$pid/children")
		for sampled_pid in "$pid" $child_pids; do
			[[ -r "/proc/$sampled_pid/status" ]] || continue
			sampled_kb=$(awk '/^VmHWM:/ { print $2; found = 1; exit } /^VmRSS:/ { fallback = $2 } END { if (!found && fallback != "") print fallback }' "/proc/$sampled_pid/status" 2>/dev/null || true)
			[[ "$sampled_kb" =~ ^[0-9]+$ && "$sampled_kb" -gt "$peak_kb" ]] && peak_kb=$sampled_kb
		done
		sleep 0.02
	done
	if wait "$pid"; then status=0; else status=$?; fi
	end_ns=$(date +%s%N)
	last_elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
	last_peak_working_set_bytes=$(( peak_kb * 1024 ))
	[[ -s "$log.stderr" ]] || rm -f -- "$log.stderr"
	[[ "$status" == 0 ]] || { echo "ProviderFixture failed or timed out (exit $status); see $log" >&2; return "$status"; }
}
dependency_closure_audit() {
	# Under `set -u`, Bash expands every right-hand side in a compound `local`
	# declaration before assigning any of them. Initialize the base directory first
	# so constructing its child log paths cannot read an unbound variable.
	local executable="$1" install_dir="$2" provider_build_dir="$3" evidence_dir="$4"
	local ldd_log="$evidence_dir/fixture-ldd.raw.txt" readelf_log="$evidence_dir/fixture-readelf.raw.txt"
	local ldd_available=false readelf_available=false unexpected=""
	if command -v ldd >/dev/null 2>&1; then ldd_available=true; ldd "$executable" >"$ldd_log" 2>&1 || true; fi
	if command -v readelf >/dev/null 2>&1; then readelf_available=true; readelf -d "$executable" >"$readelf_log" 2>&1 || true; fi
	# Jolt installs a source-side include/Jolt/Jolt.cmake containing inactive GPU
	# branches. Audit only resolved package metadata plus the configured cache.
	unexpected=$({ [[ -f "$ldd_log" ]] && grep -Eih 'jolt|physx|vulkan|d3d|dxgi|metal|cpu[ _-]?compute' "$ldd_log" || true; [[ -f "$readelf_log" ]] && grep -Eih 'jolt|physx|vulkan|d3d|dxgi|metal|cpu[ _-]?compute' "$readelf_log" || true; find "$install_dir/lib/cmake/Jolt" "$provider_build_dir/CMakeCache.txt" -type f \( -name '*.cmake' -o -name 'CMakeCache.txt' \) -print0 2>/dev/null | xargs -0 -r grep -Eih 'VulkanSDK|VULKAN.*(PATH|DIR|ROOT)|D3D12.*(PATH|DIR)|DXGI.*(PATH|DIR)|METAL.*(PATH|DIR)|JPH_USE_(DX12|VK|MTL|CPU_COMPUTE):BOOL=ON|Jolt[^[:space:]]*\.so|PhysX[^[:space:]]*\.so' || true; })
	python3 - "$evidence_dir/dependency-closure.raw.json" "$executable" "$ldd_available" "$readelf_available" "$ldd_log" "$readelf_log" <<'PY'
import json, sys
json.dump({"schemaVersion":1,"platform":"linux","fixture":sys.argv[2],"lddAvailable":sys.argv[3] == "true","readelfAvailable":sys.argv[4] == "true","lddLog":sys.argv[5] if sys.argv[3] == "true" else None,"readelfLog":sys.argv[6] if sys.argv[4] == "true" else None,"staticLinkEvidence":"Runtime linker output plus installed JoltConfig and provider CMake cache were scanned."}, open(sys.argv[1], "w"), separators=(",", ":"))
PY
	[[ -z "$unexpected" ]] || { printf '%s\n' "$unexpected" > "$evidence_dir/dependency-closure.unexpected.txt"; echo "Dependency-closure audit rejected a global/provider SDK or dynamic provider dependency." >&2; return 1; }
}
source_revision() {
	[[ -d "$1" ]] || { echo "Provider source checkout does not exist: $1" >&2; return 1; }
	git -C "$1" rev-parse HEAD | tr '[:upper:]' '[:lower:]'
}
directory_hash() {
	# Sorted relative paths and file hashes make an OS-independent evidence digest.
	if [[ ! -d "$1" ]]; then printf '%s' "" | sha256sum | awk '{print $1}'; return; fi
	( cd "$1" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 -r sha256sum ) | sha256sum | awk '{print $1}'
}
json_escape() {
	# Provider paths are caller input. Escape them before retaining provenance in
	# raw JSON so evidence remains parseable on both Windows-mounted and Linux paths.
	printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/	/\\t/g' -e 's//\\r/g' -e 's/$/\\n/' | tr -d '\n' | sed 's/\\n$//'
}
cmake_generator=()
if [[ -n "$generator" ]]; then cmake_generator=(-G "$generator")
elif command -v ninja >/dev/null 2>&1; then cmake_generator=(-G Ninja)
fi

if [[ "$validate_only" == 1 ]]; then
	[[ ! -e "$run_root-validation" ]] || { echo "Validation output exists: $run_root-validation" >&2; exit 2; }
	mkdir -p "$run_root-validation"
	run_logged validation-configure "$run_root-validation/configure.log" cmake -S "$harness_source" -B "$run_root-validation/build" "${cmake_generator[@]}" -DCMAKE_INSTALL_PREFIX="$run_root-validation/install" -DBACKEND_SPIKE_PROVIDER=Jolt56 -DBACKEND_SPIKE_REVISION=e77f175595e64cb44218cc9d9d56fc365ad0e36a -DBACKEND_SPIKE_VALIDATE_ONLY=ON
	run_logged validation-build "$run_root-validation/build.log" cmake --build "$run_root-validation/build"
	run_logged validation-install "$run_root-validation/install.log" cmake --install "$run_root-validation/build"
	echo "Backend spike harness validation passed: $run_root-validation"; exit 0
fi

[[ ! -e "$run_root" ]] || { echo "Run output exists: $run_root (evidence is never overwritten)." >&2; exit 2; }
mkdir -p "$run_root"
for id in jolt55 jolt56 physx58; do
	[[ "$candidate" == all || "$candidate" == "$id" ]] || continue
	case "$id" in
		jolt55) source="$jolt55_source"; version=5.5.0; expected=23dadd0e603f1b321142d4c74df07fce85064989; cmake_id=Jolt55; cmake_subdirectory=Build; options=(-DBUILD_SHARED_LIBS=OFF -DENABLE_INSTALL=ON -DTARGET_UNIT_TESTS=OFF -DTARGET_HELLO_WORLD=OFF -DTARGET_PERFORMANCE_TEST=OFF -DTARGET_SAMPLES=OFF -DTARGET_VIEWER=OFF -DENABLE_OBJECT_STREAM=OFF -DDEBUG_RENDERER_IN_DEBUG_AND_RELEASE=ON -DDEBUG_RENDERER_IN_DISTRIBUTION=OFF -DPROFILER_IN_DEBUG_AND_RELEASE=OFF -DOVERRIDE_CXX_FLAGS=OFF -DENABLE_ALL_WARNINGS=OFF) ;;
		jolt56) source="$jolt56_source"; version=5.6.0; expected="$jolt56_revision"; cmake_id=Jolt56; cmake_subdirectory=Build; options=(-DJPH_BUILD_SHARED_LIBS=OFF -DENABLE_INSTALL=ON -DTARGET_UNIT_TESTS=OFF -DTARGET_HELLO_WORLD=OFF -DTARGET_PERFORMANCE_TEST=OFF -DTARGET_SAMPLES=OFF -DTARGET_VIEWER=OFF -DENABLE_OBJECT_STREAM=OFF -DJPH_USE_DX12=OFF -DJPH_USE_VK=OFF -DJPH_USE_MTL=OFF -DJPH_USE_CPU_COMPUTE=OFF -DDEBUG_RENDERER_IN_DEBUG_AND_RELEASE=ON -DDEBUG_RENDERER_IN_DISTRIBUTION=OFF -DPROFILER_IN_DEBUG_AND_RELEASE=OFF -DOVERRIDE_CXX_FLAGS=OFF -DENABLE_ALL_WARNINGS=OFF) ;;
		physx58) source="$physx58_source"; version=5.8.0; expected=fc1018a3745664a1db2b95ce03fb5e91eb585f2e; cmake_id=PhysX58; cmake_subdirectory=physx/compiler/public; options=(-DPX_GENERATE_GPU_PROJECTS=OFF -DPX_GENERATE_STATIC_LIBRARIES=ON) ;;
	esac
	[[ -n "$source" && -n "$expected" ]] || { echo "$id requires an explicit source and exact revision." >&2; exit 2; }
	[[ "$double_precision" == 0 || "$id" != physx58 ]] || { echo "--double-precision supports Jolt candidates only." >&2; exit 2; }
	source=$(absolute_path "$source" "$repo_root"); observed=$(source_revision "$source")
	[[ "$observed" == "$expected" ]] || { echo "$id source revision $observed does not match expected $expected." >&2; exit 2; }
	provider_source="$source/$cmake_subdirectory"
	[[ -f "$provider_source/CMakeLists.txt" ]] || { echo "$id CMake entry point is missing: $provider_source" >&2; exit 2; }
	root="$run_root/$id"; build="$root/provider-build"; fixture_build="$root/fixture-build"; install="$root/install"; evidence="$root/evidence"; logs="$root/logs"; harness_build="$root/harness-build"
	if [[ "$id" == physx58 ]]; then
		# PhysX's generator normally injects these settings after Packman.
		# The direct spike supplies them so no networked generator is needed.
		options+=("-DPHYSX_ROOT_DIR=$source/physx" "-DTARGET_BUILD_PLATFORM=$(if [[ "${OS:-}" == Windows_NT ]]; then printf windows; else printf linux; fi)" "-DPX_OUTPUT_LIB_DIR=$build" "-DPX_OUTPUT_BIN_DIR=$build")
	elif [[ "$double_precision" == 1 ]]; then
		options+=(-DDOUBLE_PRECISION=ON)
	fi
	mkdir -p "$build" "$fixture_build" "$install" "$evidence" "$logs"
	source_tree=$(git -C "$source" rev-parse 'HEAD^{tree}')
	source_tag=$(git -C "$source" describe --tags --exact-match HEAD 2>/dev/null || true)
	source_origin=$(git -C "$source" remote get-url origin 2>/dev/null || true)
	source_dirty_count=$(git -C "$source" status --porcelain --untracked-files=no | wc -l)
	source_acquisition_timestamp=$(stat -c '%w' "$source" 2>/dev/null || true)
	[[ -n "$source_acquisition_timestamp" && "$source_acquisition_timestamp" != '-' ]] || source_acquisition_timestamp=$(stat -c '%y' "$source")
	run_started_utc=$(date -u +'%Y-%m-%dT%H:%M:%SZ')
	source_file_count=$(find "$source" -type f | wc -l)
	source_bytes=$(find "$source" -type f -printf '%s\n' | awk '{ total += $1 } END { print total + 0 }')
	cmake_network_primitive_matches=$({ find "$provider_source" -type f \( -name CMakeLists.txt -o -name '*.cmake' \) -print0 | xargs -0 -r grep -Eih 'FetchContent|ExternalProject|file[[:space:]]*\([[:space:]]*DOWNLOAD' || true; } | wc -l)
	write_json "$evidence/checkout-manifest.raw.json" "{\"schemaVersion\":1,\"runStartedUtc\":\"$run_started_utc\",\"origin\":\"$(json_escape "$source_origin")\",\"tag\":\"$(json_escape "$source_tag")\",\"commit\":\"$observed\",\"gitTree\":\"$source_tree\",\"acquisitionTimestamp\":\"$(json_escape "$source_acquisition_timestamp")\",\"acquisitionTimestampBasis\":\"source-directory-creation-time\",\"cleanTrackedTree\":$(if [[ "$source_dirty_count" == 0 ]]; then printf true; else printf false; fi),\"trackedDirtyEntries\":$source_dirty_count,\"fileCount\":$source_file_count,\"bytes\":$source_bytes,\"cmakeNetworkPrimitiveMatches\":$cmake_network_primitive_matches}"
	# Environment policy is inherited by CMake and any FetchContent subprocess.
	export HTTP_PROXY=http://127.0.0.1:9 HTTPS_PROXY=http://127.0.0.1:9 ALL_PROXY=http://127.0.0.1:9
	export http_proxy=http://127.0.0.1:9 https_proxy=http://127.0.0.1:9 all_proxy=http://127.0.0.1:9 NO_PROXY= no_proxy=
	offline=(-DFETCHCONTENT_FULLY_DISCONNECTED=ON -DFETCHCONTENT_UPDATES_DISCONNECTED=ON -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=TRUE)
	run_logged harness-configure "$logs/harness-configure.log" cmake -S "$harness_source" -B "$harness_build" "${cmake_generator[@]}" -DCMAKE_INSTALL_PREFIX="$install" -DBACKEND_SPIKE_PROVIDER="$cmake_id" -DBACKEND_SPIKE_SOURCE_DIR="$source" -DBACKEND_SPIKE_REVISION="$observed" -DBACKEND_SPIKE_EVIDENCE_DIR="$evidence" "${offline[@]}"
	harness_configure_ms=$last_elapsed_ms
	run_logged harness-build "$logs/harness-build.log" cmake --build "$harness_build" --config "$configuration"
	harness_build_ms=$last_elapsed_ms
	run_logged harness-install "$logs/harness-install.log" cmake --install "$harness_build" --config "$configuration"
	harness_install_ms=$last_elapsed_ms
	run_logged provider-configure "$logs/provider-configure.log" cmake -S "$provider_source" -B "$build" "${cmake_generator[@]}" -DCMAKE_BUILD_TYPE="$configuration" -DCMAKE_INSTALL_PREFIX="$install" "${offline[@]}" "${options[@]}"
	provider_configure_ms=$last_elapsed_ms
	run_logged provider-build "$logs/provider-build.log" cmake --build "$build" --config "$configuration"
	provider_build_ms=$last_elapsed_ms
	run_logged provider-install "$logs/provider-install.log" cmake --install "$build" --config "$configuration"
	provider_install_ms=$last_elapsed_ms
	fixture_json='{}'
	if [[ "$id" == jolt55 || "$id" == jolt56 ]]; then
		# Build the checked-in probe only against this run's installed Jolt
		# package. NO_DEFAULT_PATH in the harness prevents global SDK fallback.
		run_logged fixture-configure "$logs/fixture-configure.log" cmake -S "$harness_source" -B "$fixture_build" "${cmake_generator[@]}" -DCMAKE_BUILD_TYPE="$configuration" -DCMAKE_INSTALL_PREFIX="$install" -DBACKEND_SPIKE_PROVIDER="$cmake_id" -DBACKEND_SPIKE_SOURCE_DIR="$source" -DBACKEND_SPIKE_REVISION="$observed" -DBACKEND_SPIKE_EVIDENCE_DIR="$evidence" -DBACKEND_SPIKE_BUILD_FIXTURE=ON -DBACKEND_SPIKE_PROVIDER_INSTALL_DIR="$install" "${offline[@]}"
		run_logged fixture-build "$logs/fixture-build.log" cmake --build "$fixture_build" --config "$configuration" --target BackendSpikeProviderFixture
		run_logged fixture-install "$logs/fixture-install.log" cmake --install "$fixture_build" --config "$configuration"
		fixture_executable="$fixture_build/ProviderFixture"
		[[ -x "$fixture_executable" ]] || fixture_executable="$fixture_build/$configuration/ProviderFixture"
		[[ "$dry_run" == 1 || -x "$fixture_executable" ]] || { echo "Built ProviderFixture executable was not found under $fixture_build." >&2; exit 1; }
		if [[ "$dry_run" == 0 ]]; then
			fixture_entries=
			processor_count=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf 1)
			for workers in $(printf '1\n2\n4\n%s\n' "$processor_count" | sort -n -u); do
				repeat_entries=
				for repeat in $(seq 1 "$fixture_repeats"); do
					fixture_log="$evidence/fixture-workers-$workers-repeat-$repeat.raw.json"
					run_fixture_process "$fixture_executable" "$fixture_log" "$workers"
					python3 -m json.tool "$fixture_log" >/dev/null
					repeat_entries="${repeat_entries}{\"repeat\":$repeat,\"elapsedMilliseconds\":$last_elapsed_ms,\"peakWorkingSetBytes\":$last_peak_working_set_bytes,\"result\":$(tr -d '\n' < "$fixture_log")},"
				done
				fixture_entries="${fixture_entries}\"workers$workers\":[${repeat_entries%,}],"
			done
			fixture_json="{${fixture_entries%,}}"
			printf '%s' "$fixture_json" > "$evidence/fixture-runs.raw.json"
			python3 - "$evidence/fixture-aggregation.raw.json" "$evidence/fixture-runs.raw.json" <<'PY'
import json, sys
runs = json.load(open(sys.argv[2]))
items = [entry for group in runs.values() for entry in group]
def values(path):
    out=[]
    for item in items:
        value=item['result']
        for key in path: value=value.get(key) if isinstance(value, dict) else None
        if value is not None: out.append(value)
    return sorted(set(out))
state=values(('determinism','backendStateHash'))
packet=values(('determinism','sortedEnginePacketHashFirst'))
query=values(('queryBounds','denseOrderingHash'))
batch_query=values(('queryBounds','batchOrderingHash'))
result={'schemaVersion':1,'processLaunches':len(items),'workerLimits':sorted({item['result']['workerCount'] for item in items}),'backendStateHashes':state,'sortedPacketHashes':packet,'queryOrderingHashes':query,'batchQueryOrderingHashes':batch_query,'backendStateMatchesAcrossRepeatsAndWorkerLimits':len(state)==1,'sortedPacketMatchesAcrossRepeatsAndWorkerLimits':len(packet)==1,'queryOrderingMatchesAcrossRepeatsAndWorkerLimits':len(query)==1,'batchQueryOrderingMatchesAcrossRepeatsAndWorkerLimits':len(batch_query)==1,'peakProcessWorkingSetBytes':max((item.get('peakWorkingSetBytes',0) for item in items), default=0)}
json.dump(result, open(sys.argv[1],'w'), separators=(',',':'))
if not all(result[key] for key in ('backendStateMatchesAcrossRepeatsAndWorkerLimits','sortedPacketMatchesAcrossRepeatsAndWorkerLimits','queryOrderingMatchesAcrossRepeatsAndWorkerLimits','batchQueryOrderingMatchesAcrossRepeatsAndWorkerLimits')):
    raise SystemExit('Cross-process determinism aggregation failed')
PY
			dependency_closure_audit "$fixture_executable" "$install" "$build" "$evidence"
		fi
	fi
	export TERRA_BACKEND_SPIKE_CANDIDATE="$cmake_id" TERRA_BACKEND_SPIKE_SOURCE="$source" TERRA_BACKEND_SPIKE_BUILD="$build" TERRA_BACKEND_SPIKE_INSTALL="$install" TERRA_BACKEND_SPIKE_EVIDENCE="$evidence"
	fixture_status=not-supplied; measurement_status=not-supplied
	if [[ -n "$fixture_command" ]]; then run_logged fixture-hook "$logs/fixture-hook.log" "$fixture_command" "${fixture_arguments[@]}"; fixture_status=completed; fi
	if [[ -n "$measurement_command" ]]; then run_logged measurement-hook "$logs/measurement-hook.log" "$measurement_command" "${measurement_arguments[@]}"; measurement_status=completed; fi
	install_hash=$(directory_hash "$install")
	build_bytes=$(find "$build" -type f -printf '%s\n' | awk '{ total += $1 } END { print total + 0 }')
	install_bytes=$(find "$install" -type f -printf '%s\n' | awk '{ total += $1 } END { print total + 0 }')
	logs_hash=$(directory_hash "$logs")
	fixture_aggregation_json=null; dependency_closure_json=null
	[[ -f "$evidence/fixture-aggregation.raw.json" ]] && fixture_aggregation_json=$(tr -d '\n' < "$evidence/fixture-aggregation.raw.json")
	[[ -f "$evidence/dependency-closure.raw.json" ]] && dependency_closure_json=$(tr -d '\n' < "$evidence/dependency-closure.raw.json")
	write_json "$evidence/environment-manifest.raw.json" "{\"schemaVersion\":1,\"kernel\":\"$(json_escape "$(uname -a)")\",\"cmake\":\"$(json_escape "$(cmake --version | head -n 1)")\",\"compiler\":\"$(json_escape "$(c++ --version | head -n 1)")\",\"logicalProcessors\":$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf 1),\"buildBytes\":$build_bytes,\"installBytes\":$install_bytes,\"installSha256\":\"$install_hash\",\"logsSha256\":\"$logs_hash\",\"loopbackProxyEnforced\":true,\"packageRegistriesDisabled\":true}"
	write_json "$evidence/raw-result.json" "{\"schemaVersion\":3,\"candidate\":\"$cmake_id\",\"version\":\"$version\",\"revision\":\"$observed\",\"source\":\"$(json_escape "$source")\",\"configuration\":\"$configuration\",\"doublePrecision\":$(if [[ "$double_precision" == 1 ]]; then printf true; else printf false; fi),\"networkPolicy\":\"FetchContent disconnected; CMake registries disabled; loopback-only proxy\",\"providerPrivateThreadPoolAllowed\":false,\"fixtureRuns\":$fixture_json,\"fixtureAggregation\":$fixture_aggregation_json,\"dependencyClosure\":$dependency_closure_json,\"lifecycle\":{\"teardownRaceHook\":\"fixture-command\",\"result\":\"$fixture_status\"},\"largeWorld\":{\"floatPositionHook\":\"built-in-fixture\",\"doublePositionHook\":\"built-in-fixture\",\"result\":\"$measurement_status\"},\"queryBounds\":{\"maximumCastDistance\":100000,\"maximumShapeExtent\":1000,\"maximumHitsPerQuery\":64,\"maximumRequestsPerBatch\":10000,\"retainedResultTicks\":2,\"status\":\"reported-by-built-in-fixture\"},\"performance\":{\"status\":\"reported-by-built-in-fixture\"},\"determinism\":{\"status\":\"cross-process aggregation in fixtureAggregation\"},\"timings\":{\"harness\":{\"configureMilliseconds\":$harness_configure_ms,\"buildMilliseconds\":$harness_build_ms,\"installMilliseconds\":$harness_install_ms},\"provider\":{\"configureMilliseconds\":$provider_configure_ms,\"buildMilliseconds\":$provider_build_ms,\"installMilliseconds\":$provider_install_ms}},\"installSha256\":\"$install_hash\"}"
	sha256sum "$evidence/raw-result.json" > "$evidence/raw-result.sha256"
done
echo "Backend spike completed. Raw evidence: $run_root"
