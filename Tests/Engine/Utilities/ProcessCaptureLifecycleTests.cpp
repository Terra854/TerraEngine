// Actual process owner/OpenUrl lifecycle regressions. The native fixture does
// not fork, open browsers, or emit unbounded data. Editor fork safety CR093
// remains a separately qualified POSIX integration requirement, not a fake test.
#include "TestHarness.h"
#include "Utilities/OwnedProcess.h"
#include "Utilities/PlatformUtils.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#ifndef _WIN32
#include <cerrno>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace TerraEngine::Tests {
namespace {
	using Utilities::OwnedProcess; using Utilities::OwnedProcessRequest;
	std::string Utf8(const std::filesystem::path& path) {const auto text=path.u8string();return {reinterpret_cast<const char*>(text.data()),text.size()};}
	std::string Quote(const std::string& value) {
#ifdef _WIN32
		std::string result="\"";std::size_t slashes=0;
		for(char c:value){if(c=='\\'){++slashes;continue;}result.append(c=='\"'?slashes*2U+1U:slashes,'\\');result.push_back(c);slashes=0;}result.append(slashes*2U,'\\');return result+'\"';
#else
		std::string result="'";for(char c:value)result+=c=='\''?"'\\''":std::string(1,c);return result+'\'';
#endif
	}
	std::filesystem::path Child(TestContext& context) {
		const auto file=PlatformUtils::GetExecutableDirectory()/
#ifdef _WIN32
			"TerraEngineProcessLifecycleChild.exe";
#else
			"TerraEngineProcessLifecycleChild";
#endif
		context.Expect(std::filesystem::is_regular_file(file),"Build/install the native lifecycle child beside this actual runner; no build-tree fallback is allowed.");return file;
	}
	void Capture(TestContext& context,std::size_t bytes,std::chrono::seconds timeout) {
		OwnedProcessRequest request;request.commandLine=Quote(Utf8(Child(context)))+" --burst "+std::to_string(bytes);request.timeout=timeout;
		const std::atomic_bool canceled{false};const auto result=OwnedProcess::Run(request,canceled);
		context.Expect(result.started && !result.timedOut && !result.canceled && result.exitCode==0,"A bounded native writer must finish without capture-induced timeout.");
		context.Expect(result.output==std::string(bytes,'x')+"\nLIFECYCLE-DONE\n","Captured bytes must match the independent known complete stream and terminal marker.");
	}
#ifndef _WIN32
	class Environment {
		std::string name;std::optional<std::string> previous;
	public:
		Environment(std::string key,const std::string& value):name(std::move(key)){if(const char* old=std::getenv(name.c_str()))previous=old;if(setenv(name.c_str(),value.c_str(),1)!=0)throw std::runtime_error("Cannot set child-local fixture environment.");}
		~Environment(){if(previous)setenv(name.c_str(),previous->c_str(),1);else unsetenv(name.c_str());}
	};
	struct Root {
		std::filesystem::path path;
		Root(){path=std::filesystem::temp_directory_path()/("TerraOpenUrl-"+std::to_string(getpid())+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));if(!std::filesystem::create_directory(path))throw std::runtime_error("Cannot reserve isolated OpenUrl fixture.");}
		~Root(){std::error_code error;std::filesystem::remove_all(path,error);}
	};
	struct OwnedPid {
		pid_t value=0;
		~OwnedPid(){if(value>0){int status=0;while(waitpid(value,&status,0)<0 && errno==EINTR){}}}
	};
	struct Release {
		std::filesystem::path gate;
		void Now()const{std::ofstream file(gate);file<<"release\n";}
		~Release(){Now();}
	};
	void CheckOpenUrl(TestContext& context) {
		constexpr const char* name="Utilities.ProcessCapture.PosixExternalOpenReapsOwnedStub";
		const char* watched=std::getenv("TERRAENGINE_PROCESS_LIFECYCLE_CHILD");
		if(!watched || std::string_view(watched)!=name) {
			OwnedProcessRequest request;request.commandLine=Quote(Utf8(PlatformUtils::GetExecutablePath()));request.workingDirectory=PlatformUtils::GetExecutableDirectory();request.timeout=std::chrono::seconds(30);
			request.environment={{"TERRAENGINE_PROCESS_LIFECYCLE_CHILD",name},{"TERRAENGINE_TEST_FILTER",name},{"TERRAENGINE_TEST_EXCLUDE_FILTER",""}};
			const std::atomic_bool canceled{false};const auto result=OwnedProcess::Run(request,canceled);
			context.Expect(result.started && !result.timedOut && result.exitCode==0,"The isolated real OpenUrl/POSIX child must prove nonblocking launch and reaping: "+result.output);return;
		}
		Root root;OwnedPid child;
		std::filesystem::copy_file(Child(context),root.path/"xdg-open");
		std::filesystem::permissions(root.path/"xdg-open",std::filesystem::perms::owner_exec,std::filesystem::perm_options::add);
		const auto gate=root.path/"release",pidFile=root.path/"pid";
		const char* inherited=std::getenv("PATH");Environment path("PATH",Utf8(root.path)+":"+(inherited?inherited:""));
		Environment gateEnvironment("TERRA_PROCESS_OPEN_GATE",Utf8(gate)),pidEnvironment("TERRA_PROCESS_OPEN_PID",Utf8(pidFile));
		auto caller=std::async(std::launch::async,[]{return PlatformUtils::OpenUrl("https://example.invalid/validation-stub");});
		// Destruct before the future so an assertion cannot strand a synchronous
		// broken implementation behind its own native child gate.
		Release release{gate};const auto startupDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
		while(child.value<=0 && std::chrono::steady_clock::now()<startupDeadline){std::ifstream file(pidFile);file>>child.value;if(child.value<=0)std::this_thread::sleep_for(std::chrono::milliseconds(5));}
		const bool nonblocking=caller.wait_for(std::chrono::seconds(5))==std::future_status::ready;release.Now();const bool started=caller.get();
		context.Expect(child.value>0 && started && nonblocking,"OpenUrl must return while its actual external child remains gated.");
		bool reaped=false;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
		while(std::chrono::steady_clock::now()<deadline){siginfo_t info{};errno=0;const int result=waitid(P_PID,static_cast<id_t>(child.value),&info,WEXITED|WNOHANG|WNOWAIT);if(result<0 && errno==ECHILD){reaped=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(5));}
		// WNOWAIT observes without accidentally fixing the old missing-reaper bug.
		// OwnedPid reaps only this retained child on failure after the observation.
		context.Expect(reaped,"The production external-open owner must reap its exited child without leaving an accumulated zombie.");
	}
#endif
}
void RegisterProcessCaptureLifecycleTests(TestRegistry& registry) {
	registry.Add("Utilities.ProcessCapture.ShortNativeWriterControl",[](TestContext& context){Capture(context,1024U,std::chrono::seconds(8));});
	registry.Add("Utilities.ProcessCapture.DrainsTwoMiBWithoutThrottleTimeout",[](TestContext& context){
		// Current Windows loop spends roughly25.6seconds in root-alive waits for
		//2MiB; the8-second bound leaves generous normal IO slack yet exposes it.
		Capture(context,2U*1024U*1024U,std::chrono::seconds(8));
	});
#ifndef _WIN32
	registry.Add("Utilities.ProcessCapture.PosixExternalOpenReapsOwnedStub",[](TestContext& context){CheckOpenUrl(context);});
#endif
}
}
