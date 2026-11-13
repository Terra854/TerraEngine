// Real subprocess fixture: report whether a parent-only event was inherited.
// Parent CMake builds this Windows-only helper; it never compiles an asset or
// claims shader/GPU acceptance. All waits have a terminal timeout.
#if defined(_WIN32)
#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int wmain(int argc, wchar_t** argv) {
	if(argc != 6) return 2;
	const auto sentinelValue = std::wcstoull(argv[1], nullptr, 10);
	const std::filesystem::path receipt(argv[2]);
	const std::wstring readyName(argv[3]), releaseName(argv[4]), marker(argv[5]);
	bool inheritedSentinel = false;
	if(sentinelValue != 0) {
		// A successful SetEvent alone could target a coincident child-local
		// handle; the parent independently checks its original event state.
		inheritedSentinel = SetEvent(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(sentinelValue))) != FALSE;
	}
	if(readyName != L"-") {
		HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName.c_str());
		HANDLE release = OpenEventW(SYNCHRONIZE, FALSE, releaseName.c_str());
		if(!ready || !release) { if(ready) CloseHandle(ready); if(release) CloseHandle(release); return 3; }
		SetEvent(ready);
		const auto waitResult = WaitForSingleObject(release, 60000);
		CloseHandle(ready); CloseHandle(release);
		if(waitResult != WAIT_OBJECT_0) return 4;
	}
	std::ofstream out(receipt, std::ios::binary);
	out << "{\"inheritedSentinel\":" << (inheritedSentinel ? "true" : "false")
		<< ",\"marker\":\"" << (marker == L"A" ? "A" : "B") << "\"}\n";
	out.close();
	std::cout << "compiler fixture output " << (marker == L"A" ? "A" : "B") << '\n';
	return out ? 0 : 5;
}
#endif
