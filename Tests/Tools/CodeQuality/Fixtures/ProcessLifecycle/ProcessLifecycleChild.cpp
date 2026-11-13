// Native validation fixture; no fork/descendants/UI/network. Usage:
// ProcessLifecycleChild --burst <bytes<=2097152>; installed POSIX xdg-open alias
// uses captured gate/PID environment and exits after release or a 15-second guard.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif
int main(int argc,char** argv) {
#ifdef _WIN32
	// Compare exact capture bytes across platforms; CRT text translation must
	// not change the independent marker from LF to CRLF on Windows pipes.
	if(_setmode(_fileno(stdout),_O_BINARY)==-1) return 6;
#endif
	if(argc==3 && std::string(argv[1])=="--burst") {
		const auto count=std::stoull(argv[2]); if(count>2097152U) return 2;
		std::array<char,65536> block; block.fill('x');
		for(std::size_t sent=0;sent<count;) {const auto amount=std::min<std::size_t>(block.size(),count-sent);std::cout.write(block.data(),static_cast<std::streamsize>(amount));if(!std::cout)return 3;sent+=amount;}
		std::cout<<"\nLIFECYCLE-DONE\n"<<std::flush; return std::cout ? 0 : 3;
	}
#ifndef _WIN32
	if(std::filesystem::path(argv[0]).filename()=="xdg-open") {
		const char* gate=std::getenv("TERRA_PROCESS_OPEN_GATE"); const char* pid=std::getenv("TERRA_PROCESS_OPEN_PID"); if(!gate || !pid) return 4;
		{std::ofstream receipt(pid);receipt<<getpid()<<'\n';if(!receipt)return 5;}
		const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
		while(std::chrono::steady_clock::now()<deadline) {if(std::filesystem::exists(gate))return 0;std::this_thread::sleep_for(std::chrono::milliseconds(5));}
		return 124;
	}
#endif
	return 2;
}
