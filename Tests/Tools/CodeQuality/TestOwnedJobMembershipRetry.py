"""Windows-only bounded protocol for the actual owned-job membership loop.

Run from an x64 Visual Studio developer shell after configuring BUILD_TESTING:
python TestOwnedJobMembershipRetry.py --source-root <repo> --evidence-directory
<fresh-owned-output> --cxx <configured-cl.exe>. Only the kernel membership query
is substituted; real process-tree/handle settlement uses the native test lanes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument('--source-root', type=Path, required=True)
	parser.add_argument('--evidence-directory', type=Path, required=True)
	parser.add_argument('--cxx', required=True)
	args = parser.parse_args()
	root = args.source_root.resolve()
	proof = args.evidence_directory.resolve()
	proof.mkdir(parents=True, exist_ok=False)
	source = root / 'Engine/Utilities/OwnedProcess.cpp'
	driver = proof / 'membership-driver.cpp'
	driver.write_text(r'''
#define NOMINMAX
#include <Windows.h>
#include <chrono>
#include <iostream>
#include <vector>
namespace Fixture {
	enum class Mode { PartialFits, MoreDataWithoutCount, LargerCount, Continuous, Refused };
	Mode mode;
	unsigned calls = 0;
	std::vector<DWORD> capacities;
	BOOL WINAPI Query(HANDLE, JOBOBJECTINFOCLASS, LPVOID data, DWORD bytes, LPDWORD) {
		++calls; capacities.push_back(bytes);
		if (mode == Mode::Refused) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
		auto* list = static_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(data);
		if (calls == 1 || mode == Mode::Continuous) {
			list->NumberOfAssignedProcesses = mode == Mode::LargerCount ? 120 : 2;
			list->NumberOfProcessIdsInList = 1;
			if (mode == Mode::MoreDataWithoutCount) {
				list->NumberOfAssignedProcesses = list->NumberOfProcessIdsInList = 0;
				SetLastError(ERROR_MORE_DATA); return FALSE;
			}
			return TRUE;
		}
		// Empty complete membership avoids inventing retained OS process handles.
		list->NumberOfAssignedProcesses = list->NumberOfProcessIdsInList = 0;
		return TRUE;
	}
}
#define QueryInformationJobObject Fixture::Query
#include "@SOURCE@"
#undef QueryInformationJobObject
int main() {
	unsigned selected = 0, failed = 0;
	const auto check = [&](const char* name, bool passed) {
		++selected; failed += !passed;
		std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << '\n';
	};
	for (const auto mode : {Fixture::Mode::PartialFits, Fixture::Mode::MoreDataWithoutCount, Fixture::Mode::LargerCount}) {
		Fixture::mode = mode; Fixture::calls = 0; Fixture::capacities.clear();
		OwnedJobProcesses owner;
		const auto result = owner.Capture(nullptr);
		check(mode == Fixture::Mode::PartialFits ? "Membership.PartialWithinCapacityRetries"
			: mode == Fixture::Mode::MoreDataWithoutCount ? "Membership.MoreDataWithoutCountRetries"
			: "Membership.LargerCountReservesHeadroom",
			result == ERROR_SUCCESS && Fixture::calls == 2 && Fixture::capacities[1] > Fixture::capacities[0]
			&& (mode != Fixture::Mode::LargerCount || Fixture::capacities[1] >= 122 * sizeof(ULONG_PTR)));
	}
	Fixture::mode = Fixture::Mode::Continuous; Fixture::calls = 0;
	{ OwnedJobProcesses owner; check("Membership.ContinuousChurnStopsAfterEightAttempts", owner.Capture(nullptr) == ERROR_MORE_DATA && Fixture::calls == 8); }
	Fixture::mode = Fixture::Mode::Refused; Fixture::calls = 0;
	{ OwnedJobProcesses owner; check("Membership.OtherKernelErrorsFailImmediately", owner.Capture(nullptr) == ERROR_ACCESS_DENIED && Fixture::calls == 1); }
	Fixture::calls = 0;
	{ OwnedJobProcesses owner; check("Membership.ExpiredSharedDeadlinePreventsQuery", owner.Capture(nullptr, std::chrono::steady_clock::now()) == ERROR_TIMEOUT && Fixture::calls == 0); }
	std::cout << selected << " selected tests, " << failed << " failures\n";
	return failed ? 1 : 0;
}
'''.replace('@SOURCE@', source.as_posix()), encoding='utf-8')
	executable = proof / 'membership-driver.exe'
	command = [args.cxx, '/nologo', '/std:c++20', '/EHsc', '/MD', '/DTERRAENGINE_ENGINE_BUILD',
		'/I' + str(root / 'Engine'), str(driver), '/Fe' + str(executable)]
	compiled = subprocess.run(command, cwd=proof, capture_output=True, text=True, timeout=60)
	(proof / 'compile.json').write_text(json.dumps({'command': command, 'exitCode': compiled.returncode,
		'stdout': compiled.stdout, 'stderr': compiled.stderr}, indent=2))
	if compiled.returncode:
		raise RuntimeError(compiled.stdout + compiled.stderr)
	result = subprocess.run([str(executable)], cwd=proof, capture_output=True, text=True, timeout=10)
	rows = [line for line in result.stdout.splitlines() if line.startswith(('[PASS]', '[FAIL]'))]
	failed = sum(line.startswith('[FAIL]') for line in rows)
	if len(rows) != 6 or result.returncode != 0:
		failed = max(failed, 1)
	(proof / 'result.json').write_text(json.dumps({'qualification': 'Actual production loop with explicit kernel-query substitute; not native handle-settlement acceptance',
		'producer': str(source), 'producerSha256': hashlib.sha256(source.read_bytes()).hexdigest(),
		'selectedCases': len(rows), 'failedCases': failed, 'cases': rows,
		'exitCode': result.returncode, 'stderr': result.stderr}, indent=2))
	print(result.stdout, end='')
	return 1 if failed else 0


if __name__ == '__main__':
	raise SystemExit(main())
