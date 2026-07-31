// Deterministic stage-cache inspection and guarded maintenance tool.
// Mutations require an explicit target and --confirm, validate a private
// staging file, retain the previous record, and publish via atomic rename.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem; using Json = nlohmann::ordered_json;
static void usage(){std::cout<<"VirtualGeometryAssetTool --inspect|--invalidate|--migrate|--repair --target PATH [--confirm]\n";}
static fs::path safe(const std::string& raw){fs::path p=fs::absolute(raw).lexically_normal(),root=fs::current_path();auto r=root.generic_string();auto s=p.generic_string();if(s.rfind(r+"/",0)!=0&&s!=r)throw std::runtime_error("target escapes repository root");return p;}
static Json read(const fs::path&p){std::ifstream in(p);if(!in)throw std::runtime_error("stage record missing");Json j;in>>j;if(!j.is_object()||j.value("schemaVersion",0)!=1)throw std::runtime_error("unsupported or malformed stage record");return j;}
static void publish(const fs::path&p,Json j){fs::path stage=p;stage+=".staging";fs::path previous=p;previous+=".previous";{std::ofstream out(stage);if(!out)throw std::runtime_error("cannot create private staging");out<<j.dump(2)<<'\n';}Json check=read(stage);if(check.value("schemaVersion",0)!=1)throw std::runtime_error("staging validation failed");if(fs::exists(p)){std::error_code ec;fs::copy_file(p,previous,fs::copy_options::overwrite_existing,ec);if(ec)throw std::runtime_error("cannot retain .previous: "+ec.message());}std::error_code ec;fs::rename(stage,p,ec);if(ec)throw std::runtime_error("atomic replacement failed: "+ec.message());}
int main(int argc,char**argv){try{if(argc<2){usage();return 2;}std::string command=argv[1],target;bool confirm=false;for(int i=2;i<argc;++i){std::string a=argv[i];if(a=="--target"&&i+1<argc)target=argv[++i];else if(a=="--confirm")confirm=true;else if(a=="--help"){usage();return 0;}}if(command=="--help"){usage();return 0;}if(target.empty())throw std::runtime_error("--target is required");fs::path p=safe(target);Json j=read(p);if(command=="--inspect"){j["provenance"]={{"command","inspect"},{"path",p.generic_string()}};std::cout<<j.dump(2)<<'\n';return 0;}if(command!="--invalidate"&&command!="--migrate"&&command!="--repair")throw std::runtime_error("unknown command");if(!confirm)throw std::runtime_error("mutating commands require --confirm");j["provenance"]={{"command",command.substr(2)},{"path",p.generic_string()}};j["status"]=command=="--invalidate"?"invalidated":command=="--migrate"?"migrated":"repaired";publish(p,j);Json result={{"schemaVersion",1},{"status",j["status"]},{"target",p.generic_string()},{"previous",p.generic_string()+".previous"}};std::cout<<result.dump(2)<<'\n';return 0;}catch(const std::exception&e){std::cerr<<"VirtualGeometryAssetTool: "<<e.what()<<'\n';return 2;}}
