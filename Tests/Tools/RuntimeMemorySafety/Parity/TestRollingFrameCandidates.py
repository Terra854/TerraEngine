"""Bounded CPU tests of the actual parity host's extracted candidate/join helper.

Synthetic frame metadata is unit input, never a native/GPU capture receipt.
The helper is also consumed by the staged actual host caller; no Python policy
model duplicates the implementation. A C++20 compiler and vendored JSON suffice.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys


def main():
	parser=argparse.ArgumentParser(description=__doc__)
	parser.add_argument('--source-root',type=Path,required=True)
	parser.add_argument('--evidence-directory',type=Path,required=True)
	parser.add_argument('--helper-header',type=Path)
	parser.add_argument('--cxx',default='cl')
	args=parser.parse_args()
	root=args.source_root.resolve();proof=args.evidence_directory.resolve();proof.mkdir(parents=True,exist_ok=True)
	header=(args.helper_header or root/'Tests/Tools/RuntimeMemorySafety/Parity/RuntimeMemorySafetyFrameCandidates.h').resolve()
	if not header.is_file():
		result={'status':'Unverified','reason':'Approved actual-host CPU helper is not available; no policy model substituted.','selectedCases':0,'nativeAcceptance':False}
		(proof/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result));return 2
	compiler=shutil.which(args.cxx)
	if compiler is None:
		raise RuntimeError('A configured C++20 developer shell is required for the bounded CPU helper driver.')
	driver=proof/'frame-candidate-driver.cpp'
	driver.write_text('#include "'+header.as_posix()+'"\n'+r'''
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <algorithm>
using Json=nlohmann::json;
int main(int argc,char**argv){
    if(argc!=2)return 2;
    std::ifstream stream(argv[1]);Json input;stream>>input;
    if(input.value("mode","")=="rollingArtifacts") {
        // Tiny owned CPU files test bookkeeping, never lighting/GPU measurements.
        namespace fs=std::filesystem;
        using namespace TerraEngine::Validation::RuntimeMemorySafety;
        const auto root=fs::path(argv[1]).parent_path()/"owned-row-artifacts";
        if(fs::exists(root))return 2;
        fs::create_directory(root);
        const auto foreign=root/"row-foreign.color.bin";
        const auto outside=root.parent_path()/"foreign-outside.bin";
        std::ofstream(foreign)<<"foreign-inside";std::ofstream(outside)<<"foreign-outside";
        ParityFrameArtifactRows owners(root);Json rows=Json::array();
        std::map<std::uint64_t,Json> outputs;
        std::map<std::uint64_t,std::map<std::pair<std::uint64_t,bool>,Json>> metadata;
        std::map<std::uint64_t,std::set<std::uint64_t>> commands;
        std::set<std::string> allPaths;
        std::size_t peakRows=0,peakFiles=0,totalRows=0;bool contiguous=true,unique=true,exact=true;
        std::uint64_t selected=0;
        for(std::uint64_t frame=1;frame<=320;++frame) {
            CanCaptureParityFrameCandidate(outputs,[&](auto retired,const auto&) {
                owners.Retire(retired,rows);metadata.erase(retired);commands.erase(retired);
            });
            outputs.emplace(frame,Json{{"frameId",frame}});
            // Equal-frame purposes arrive as separate rows, both retained together.
            for(unsigned row=0;row<32;++row) {
                const auto paths=owners.Allocate(frame);
                for(const auto&path:paths) {
                    unique=allPaths.insert(path.string()).second&&unique;
                    if(fs::exists(path))return 3;
                    std::ofstream(path)<<frame<<":"<<row;
                }
                rows.push_back({{"observedOrdinal",rows.size()},{"frameSerial",frame},
                    {"commandSerial",row/2+1},{"analysisPurpose",bool(row%2)},
                    {"ownedPaths",{paths[0].string(),paths[1].string(),paths[2].string()}},
                    {"untrustedReceiptPath",foreign.string()}});
                ++totalRows;
            }
            std::size_t files=0;for(const auto&entry:fs::directory_iterator(root))if(entry.path()!=foreign)++files;
            exact=exact&&files==3*rows.size();
            for(std::size_t i=0;i<rows.size();++i)contiguous=contiguous&&rows[i].at("observedOrdinal")==i;
            peakRows=std::max(peakRows,rows.size());peakFiles=std::max(peakFiles,files);
            // A completed frame joins only when its delayed second purpose arrives.
            if(frame==320) {
                for(unsigned i=0;i<31;++i)metadata[315][{i/2+1,bool(i%2)}]=Json::object();
                for(unsigned i=1;i<=16;++i)commands[315].insert(i);
                if(SelectCompleteParityFrameCandidate(metadata,commands,outputs,16)!=0)return 4;
                metadata[315][{16,true}]=Json::object();
                selected=SelectCompleteParityFrameCandidate(metadata,commands,outputs,16);
            }
        }
        std::size_t selectedRows=0;bool selectedFiles=true;
        for(const auto&row:rows)if(row.at("frameSerial")==selected) {
            ++selectedRows;
            for(const auto&path:row.at("ownedPaths")) {
                std::ifstream file(path.get<std::string>());std::string content;file>>content;
                selectedFiles=selectedFiles&&content.starts_with("315:");
            }
        }
        for(const auto&[frame,output]:outputs)owners.Retire(frame,rows);
        bool foreignPreserved=fs::is_regular_file(foreign)&&fs::is_regular_file(outside);
        std::size_t remaining=0;for(const auto&entry:fs::directory_iterator(root))++remaining;
        // Missing owned files must produce an ordinary failure, never silent success.
        const auto missingPaths=owners.Allocate(700);
        for(const auto&path:missingPaths)std::ofstream(path)<<"control";
        fs::remove(missingPaths[0]);bool missingRejected=false;
        try{owners.Retire(700,rows);}catch(const std::runtime_error&){missingRejected=true;}
        std::cout<<Json{{"totalRows",totalRows},{"peakRows",peakRows},{"peakFiles",peakFiles},
            {"uniqueArtifactPaths",unique},{"generatedPathCount",allPaths.size()},
            {"contiguousOrdinals",contiguous},{"exactFileCount",exact},
            {"selectedFrame",selected},{"selectedRows",selectedRows},{"selectedFilesPreserved",selectedFiles},
            {"foreignPreserved",foreignPreserved},{"remainingOwnedFiles",remaining-1},
            {"rowsAfterRetirement",rows.size()},{"missingOwnedFileRejected",missingRejected},
            {"nativeAcceptance",false},{"performanceAcceptance",false}}.dump();
        return 0;
    }
    if(!input.is_object()||!input.at("events").is_array()||input.at("events").size()>64)return 2;
    const auto required=input.at("expectedCommands").get<std::size_t>();if(required>8)return 2;
    std::map<std::uint64_t,Json> normalOutputs;
    std::map<std::uint64_t,std::map<std::pair<std::uint64_t,bool>,Json>> commandMetadata;
    std::map<std::uint64_t,std::set<std::uint64_t>> completeCommands;
    std::uint64_t selected=0;std::size_t peak=0;
    for(const auto&event:input.at("events")){
        const auto frame=event.at("frame").get<std::uint64_t>();if(frame==0)return 2;
        if(TerraEngine::Validation::RuntimeMemorySafety::CanCaptureParityFrameCandidate(normalOutputs))
            normalOutputs.emplace(frame,Json{{"qualification","CPU-unit-candidate-only"}});
        peak=std::max(peak,normalOutputs.size());
        for(const auto&row:event.at("metadata")){
            const auto target=row.at("frame").get<std::uint64_t>();
            const auto count=row.at("rows").get<unsigned>(),commands=row.at("completedCommands").get<unsigned>();
            if(count>16||commands>8)return 2;
            for(unsigned index=0;index<count;++index)commandMetadata[target][{index/2+1,bool(index%2)}]=Json{{"qualification","CPU-unit-metadata-only"}};
            for(unsigned index=0;index<commands;++index)completeCommands[target].insert(index+1);
        }
        selected=TerraEngine::Validation::RuntimeMemorySafety::SelectCompleteParityFrameCandidate(commandMetadata,completeCommands,normalOutputs,required);
        if(selected)break;
    }
    std::cout<<Json{{"status",selected?"UnitExactJoin":"NoCompleteJoin"},{"selectedFrame",selected},{"retainedCandidates",normalOutputs.size()},{"peakCandidates",peak},{"nativeAcceptance",false},{"performanceAcceptance",false}}.dump();
}
'''.replace('\\"','"'))
	executable=proof/'frame-candidate-driver.exe'
	command=[compiler,'/nologo','/std:c++20','/EHsc','/MD','/DNDEBUG','/I'+str(root/'External/json/single_include'),str(driver),'/Fo'+str(proof/'frame-candidate-driver.obj'),'/Fe'+str(executable)]
	compiled=subprocess.run(command,capture_output=True,text=True,timeout=45)
	(proof/'compile.json').write_text(json.dumps({'command':command,'exitCode':compiled.returncode,'stdout':compiled.stdout,'stderr':compiled.stderr},indent=2))
	if compiled.returncode:
		raise RuntimeError(compiled.stdout+compiled.stderr)
	def events(count,observed_at=None,completed=1,target=17):
		return [{'frame':index,'metadata':([{'frame':target,'rows':2,'completedCommands':completed}] if index==observed_at else [])} for index in range(1,count+1)]
	fixtures=[
		('FrameCandidates.EmptyInputCannotSelect',[],0,1),
		('FrameCandidates.AllRejectedCannotSelect',[{'frame':index,'metadata':[{'frame':index,'rows':2,'completedCommands':0}]} for index in range(1,33)],0,1),
		('FrameCandidates.EarlyExactFrameJoin',events(4,observed_at=2,target=2),2,1),
		('FrameCandidates.DelayedMetadataBeyondFirst16StillJoins',events(32,observed_at=22,target=17),17,1),
		('FrameCandidates.MetadataWithoutExactCandidateCannotSelect',events(32,observed_at=22,target=999),0,1),
		('FrameCandidates.RetainedWindowRemainsBounded',events(64),0,1),
	]
	rows=[]
	for index,(name,data,expected,required) in enumerate(fixtures):
		path=proof/('input-'+str(index)+'.json');path.write_text(json.dumps({'qualification':'CPU unit input, not GPU evidence','events':data,'expectedCommands':required}))
		result=subprocess.run([str(executable),str(path)],capture_output=True,text=True,timeout=10)
		observed=json.loads(result.stdout) if result.returncode==0 else None
		okay=result.returncode==0 and observed['selectedFrame']==expected and observed['peakCandidates']<=16 and observed['nativeAcceptance'] is False
		if expected==0:okay=okay and observed['status']=='NoCompleteJoin'
		rows.append({'name':name,'status':'passed-unit-contract' if okay else 'failed-contract','expectedFrame':expected,'observed':observed,'command':[str(executable),str(path)],'exitCode':result.returncode,'stderr':result.stderr})
	# One bounded actual-helper run creates 10,240 tiny CPU rows (30,720 files over
	# its lifetime), exceeding the comparator's lifetime cap while retaining 512.
	path=proof/'rolling-artifacts.json';path.write_text(json.dumps({'mode':'rollingArtifacts'}))
	command=[str(executable),str(path)]
	execution=subprocess.run(command,capture_output=True,text=True,timeout=60)
	observed=json.loads(execution.stdout) if execution.returncode==0 else {}
	checks=[
		('FrameArtifacts.ThousandsOfRowsStayBounded',observed.get('totalRows')==10240 and observed.get('peakRows')==512 and observed.get('peakFiles')==1536),
		('FrameArtifacts.MonotonicIdentitiesNeverReusePrunedNames',observed.get('uniqueArtifactPaths') is True and observed.get('generatedPathCount')==30720),
		('FrameArtifacts.ExactOwnedFilesOnlyRetire',observed.get('exactFileCount') is True and observed.get('foreignPreserved') is True and observed.get('remainingOwnedFiles')==0),
		('FrameArtifacts.RetainedReceiptOrdinalsRemainContiguous',observed.get('contiguousOrdinals') is True and observed.get('rowsAfterRetirement')==0),
		('FrameArtifacts.LaterEqualFramePurposeSelectsRetainedProvenance',observed.get('selectedFrame')==315 and observed.get('selectedRows')==32 and observed.get('selectedFilesPreserved') is True),
		('FrameArtifacts.MissingOwnedBinaryFailsClosed',observed.get('missingOwnedFileRejected') is True),
	]
	for name,okay in checks:
		rows.append({'name':name,'status':'passed-unit-contract' if execution.returncode==0 and okay else 'failed-contract','observed':observed,'command':command,'exitCode':execution.returncode,'stderr':execution.stderr})
	failed=sum(row['status']!='passed-unit-contract' for row in rows)
	result={'qualification':'Actual extracted production CPU helper only; staged host wiring; no GPU/frame-producer acceptance','selectedCases':len(rows),'failedCases':failed,'cases':rows}
	(proof/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
	return 1 if failed else 0


if __name__=='__main__':raise SystemExit(main())
