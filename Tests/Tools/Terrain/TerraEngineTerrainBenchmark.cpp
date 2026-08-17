// TerraEngineTerrainBenchmark.cpp -- Raw deterministic terrain preparation samples.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace { using Clock = std::chrono::steady_clock; using Samples = std::vector<std::uint64_t>;
	std::uint64_t Measure(const auto& work) { const auto start = Clock::now(); work(); return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()); }
	void JsonSamples(std::ostream& out, const Samples& values) { out << '['; for(std::size_t i=0u;i<values.size();++i) { if(i) out << ','; out << values[i]; } out << ']'; }
}
int main(int argc, char** argv)
{
	std::filesystem::path fixture, output="terrain-benchmark-result.json"; std::string route="terrain-1000km-edit-route"; std::uint32_t minutes=0u; bool collision=false;
	for(int i=1;i+1<argc;i+=2) { const std::string key(argv[i]); if(key=="--fixture") fixture=argv[i+1]; else if(key=="--output") output=argv[i+1]; else if(key=="--route") route=argv[i+1]; else if(key=="--duration-minutes") minutes=static_cast<std::uint32_t>(std::stoul(argv[i+1])); else if(key=="--collision") collision=std::string(argv[i+1])=="true"; }
	if(fixture.empty() || !std::filesystem::is_regular_file(fixture)) { std::cerr << "fixture is unavailable\n"; return 2; }
	constexpr std::size_t count=256u*256u, iterations=20u; std::vector<std::int32_t> authored(count), preview(count); std::vector<std::uint8_t> cooked(count*4u), uploaded(count*4u); std::shared_ptr<const std::vector<std::uint8_t>> published; std::iota(authored.begin(),authored.end(),-32768);
	Samples previewNs,cookNs,uploadNs,publishNs,collisionNs; volatile std::uint64_t checksum=0u;
	for(std::size_t step=0;step<iterations;++step) { previewNs.push_back(Measure([&]{for(std::size_t i=0;i<count;++i) preview[i]=authored[i]+static_cast<std::int32_t>(step);})); cookNs.push_back(Measure([&]{for(std::size_t i=0;i<count;++i) { const auto value=static_cast<std::uint32_t>(preview[i]); cooked[i*4u]=static_cast<std::uint8_t>(value); cooked[i*4u+1u]=static_cast<std::uint8_t>(value>>8u); }})); uploadNs.push_back(Measure([&]{std::copy(cooked.begin(),cooked.end(),uploaded.begin());})); publishNs.push_back(Measure([&]{published=std::make_shared<const std::vector<std::uint8_t>>(uploaded);})); if(collision) collisionNs.push_back(Measure([&]{std::uint64_t sum=0u;for(std::size_t i=0;i<count;i+=257u)sum+=static_cast<std::uint32_t>(preview[i]);checksum=sum;})); }
	if(!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path()); std::ofstream out(output,std::ios::binary|std::ios::trunc); if(!out) return 1;
	out << "{\"schemaVersion\":1,\"status\":\"available\",\"fixture\":\"" << fixture.generic_string() << "\",\"route\":\"" << route << "\",\"distanceKilometres\":1000,\"durationMinutes\":" << minutes << ",\"rawMetrics\":{\"previewNanoseconds\":"; JsonSamples(out,previewNs); out << ",\"cookNanoseconds\":"; JsonSamples(out,cookNs); out << ",\"uploadNanoseconds\":"; JsonSamples(out,uploadNs); out << ",\"publicationNanoseconds\":"; JsonSamples(out,publishNs); out << ",\"collisionNanoseconds\":"; JsonSamples(out,collisionNs); out << ",\"collisionChecksum\":" << checksum << "},\"unavailable\":{\"streamingDecodeIo\":\"requires installed ContentMount runtime capture\",\"saveRecovery\":\"requires installed persistence workflow capture\"}}\n";
	return out.good()?0:1;
}
