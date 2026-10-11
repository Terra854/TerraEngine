#include "TestHarness.h"
#include "Renderer/ShadowWorkPlan.h"
#include <algorithm>
#include <limits>
#include <future>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace Renderer;
		ShadowViewKey View()
		{
			ShadowViewKey v; v.rendererSessionGeneration=1; v.lightId[0]=1; v.allocationGeneration=2;
			v.extent={512,512}; v.depthFormat=126; v.lightProjectionStamp.viewProjection[0]=1; return v;
		}
		ShadowBatchMember Member(unsigned ordinal)
		{
			ShadowBatchMember m; m.sourceOrdinal=ordinal; m.objectId[0]=static_cast<std::uint8_t>(ordinal+1); m.objectId[1]=static_cast<std::uint8_t>((ordinal+1)>>8); m.lifetimeGeneration=2; m.segmentId=ordinal;
			m.meshletCount=64; m.key.meshId[0]=1; m.key.materialId[0]=2; m.key.shaderId[0]=3;
			m.key.pipeline=4; m.key.layout=5; m.key.pipelineGeneration=m.key.layoutGeneration=6;
			m.key.descriptors={7,8}; m.key.descriptorGenerations={9,10};
			m.key.meshGeneration=m.key.materialGeneration=m.key.shaderGeneration=m.key.residencyGeneration=11;
			m.key.dispatchMeshletCount=64; m.key.pushRangeCount=1; m.key.pushRanges[0]={1,0,16};
			m.knownDefaultTaskAbi=m.supportedRoute=m.deformationBridgeComplete=true; return m;
		}
		ShadowBatchLimits Limits() { return {0,2048,1024,1024,65535,65535}; }
		std::vector<ShadowBatchMember> Members(unsigned count) { std::vector<ShadowBatchMember> m; for(unsigned i=0;i<count;++i)m.push_back(Member(i)); return m; }
		void Coverage(TestContext& t,const ShadowBatchPlan& p,unsigned count)
		{
			// A contribution index is independent of camera visibility and may
			// share its source ordinal with another selected page segment.
			std::vector<unsigned> visits(count); for(auto i:p.singletonIndices){t.Expect(i<count,"Singleton index is bounded.");if(i<count)++visits[i];}
			for(const auto& g:p.groups){t.Expect(g.instanceCount==g.memberIndices.size() && g.instanceCount>=2,"Group range covers every member.");for(auto i:g.memberIndices){t.Expect(i<count,"Group index is bounded.");if(i<count)++visits[i];}}
			t.Expect(p.coverageComplete && std::all_of(visits.begin(),visits.end(),[](auto n){return n==1;}),"Every selected contribution appears exactly once.");
			t.Expect(p.allSingletonIndices.size()==count,"Original fallback remains complete until actual upload admission.");
		}
	}
	void RegisterShadowBatchPlanTests(TestRegistry& r)
	{
		r.Add("Renderer.Efficiency.ShadowBatchPlan.CompatibleThousand",[](TestContext& t){auto m=Members(1000);auto p=BuildShadowBatchPlan(View(),m,Limits());Coverage(t,p,1000);t.Expect(p.groups.size()==1 && p.groups[0].instanceCount==1000 && p.groups[0].taskGroupsX==2000,"One task draw replaces 1000 exact compatible singleton contributions."); std::array<std::future<bool>,8> jobs;for(unsigned i=0;i<jobs.size();++i)jobs[i]=std::async(std::launch::async,[&m,i]{auto v=View();v.viewId=i;auto parallel=BuildShadowBatchPlan(v,m,Limits());return parallel.coverageComplete && parallel.view==v && parallel.groups.size()==1 && parallel.groups[0].memberIndices.size()==1000;});for(auto& job:jobs)t.Expect(job.get(),"Concurrent immutable inputs never mix qualified view owners.");});
		r.Add("Renderer.Efficiency.ShadowBatchPlan.ExactCompatibility",[](TestContext& t){for(unsigned lane=0;lane<28;++lane){auto m=Members(2);auto& k=m[1].key;switch(lane){case 0:k.pipeline++;break;case 1:k.layout++;break;case 2:k.pipelineGeneration++;break;case 3:k.layoutGeneration++;break;case 4:k.descriptors[0]++;break;case 5:k.descriptors[1]++;break;case 6:k.descriptorGenerations[0]++;break;case 7:k.descriptorGenerations[1]++;break;case 8:k.meshGeneration++;break;case 9:k.materialGeneration++;break;case 10:k.shaderGeneration++;break;case 11:k.residencyGeneration++;break;case 12:k.meshId[1]++;break;case 13:k.materialId[1]++;break;case 14:k.shaderId[1]++;break;case 15:k.materialSelector++;break;case 16:k.textureSelector++;break;case 17:k.materialFlags++;break;case 18:k.shadowPolicy++;break;case 19:k.geometryRoute++;break;case 20:k.deformationPolicy++;break;case 21:k.selectionFlags++;break;case 22:k.selectedLod++;break;case 23:k.dispatchMeshletCount++;m[1].meshletCount++;break;case 24:k.pushRanges[0][0]++;break;case 25:k.pushRanges[0][1]+=4;break;case 26:k.pushRanges[0][2]+=4;break;case 27:k.pushRangeCount=2;k.pushRanges[1]={1,16,16};break;}auto p=BuildShadowBatchPlan(View(),m,Limits());Coverage(t,p,2);t.Expect(p.groups.empty() && p.singletonIndices.size()==2,"No differing shared shader input may join a group.");}});
		r.Add("Renderer.Efficiency.ShadowBatchPlan.SelectedOffsetsRemainPerMember",[](TestContext& t){auto m=Members(2);m[1].firstMeshlet=128;m[1].firstCluster=32;m[1].clusterCount=4;m[1].pageId=99;m[1].payloadOffset=512;m[1].payloadSize=256;auto p=BuildShadowBatchPlan(View(),m,Limits());Coverage(t,p,2);t.Expect(p.groups.size()==1 && p.groups[0].memberIndices==std::vector<std::uint32_t>({0,1}),"Selected page/meshlet offsets remain distinct instance selectors.");});
		r.Add("Renderer.Efficiency.ShadowBatchPlan.RepeatedOrdinalDistinctSegments",[](TestContext& t){auto m=Members(2);m[1].sourceOrdinal=m[0].sourceOrdinal;m[1].objectId=m[0].objectId;m[1].segmentId=1;auto p=BuildShadowBatchPlan(View(),m,Limits());Coverage(t,p,2);t.Expect(p.groups.size()==1,"One source can contribute multiple exact selected segments.");});
		r.Add("Renderer.Efficiency.ShadowBatchPlan.DuplicateContribution",[](TestContext& t){auto m=Members(2);m[1]=m[0];auto p=BuildShadowBatchPlan(View(),m,Limits());t.Expect(!p.coverageComplete && p.failure==ShadowBatchFailure::DuplicateContribution && p.groups.empty() && p.allSingletonIndices.size()==2,"Duplicate segment invalidates optimized coverage without destroying fallback.");});
		r.Add("Renderer.Efficiency.ShadowBatchPlan.UnsupportedKeepsSingleton",[](TestContext& t){for(unsigned fault=0;fault<5;++fault){auto m=Members(3);if(fault==0)m[1].knownDefaultTaskAbi=false;if(fault==1)m[1].supportedRoute=false;if(fault==2)m[1].deformationBridgeComplete=false;if(fault==3)m[1].conservativeDeformation=true;if(fault==4)m[1].key.pipelineGeneration=0;auto p=BuildShadowBatchPlan(View(),m,Limits());Coverage(t,p,3);t.Expect(p.groups.size()==1 && p.singletonIndices==std::vector<std::uint32_t>({1}),"Custom, unproven or conservative owners do not suppress their singleton.");}});
		r.Add("Renderer.Efficiency.ShadowBatchPlan.DeterministicTaskSplitting",[](TestContext& t){auto m=Members(8);auto l=Limits();l.maxTaskGroupsX=6;l.maxTaskGroupsTotal=6;auto p=BuildShadowBatchPlan(View(),m,l);Coverage(t,p,8);t.Expect(p.groups.size()==3 && p.groups[0].memberIndices==std::vector<std::uint32_t>({0,1,2}) && p.groups[1].firstInstance==3 && p.groups[2].instanceCount==2,"Limits split stable members into bounded contiguous instance ranges."); m=Members(33); for(auto& member:m){member.meshletCount=std::numeric_limits<std::uint32_t>::max();member.key.dispatchMeshletCount=std::numeric_limits<std::uint32_t>::max();} l=Limits();l.maxTaskGroupsX=l.maxTaskGroupsTotal=std::numeric_limits<std::uint32_t>::max();p=BuildShadowBatchPlan(View(),m,l);Coverage(t,p,33);t.Expect(p.groups.size()==2 && p.groups[0].instanceCount==31 && p.groups[0].taskGroupsX==4160749568U,"Ceiling and product use wide checked arithmetic at UINT32 limits.");});
		r.Add("Renderer.Efficiency.ShadowBatchPlan.SingleRemainder",[](TestContext& t){auto m=Members(7);auto l=Limits();l.maxInstancesPerGroup=3;auto p=BuildShadowBatchPlan(View(),m,l);Coverage(t,p,7);t.Expect(p.groups.size()==2 && p.singletonIndices==std::vector<std::uint32_t>({6}),"An unpaired remainder uses its original singleton.");});
		r.Add("Renderer.Efficiency.ShadowBatchPlan.CapacityRestoresFullFallback",[](TestContext& t){auto m=Members(8);auto l=Limits();l.instanceCapacity=7;auto p=BuildShadowBatchPlan(View(),m,l);t.Expect(!p.coverageComplete && p.failure==ShadowBatchFailure::Capacity && p.groups.empty() && p.singletonIndices==p.allSingletonIndices && p.singletonIndices.size()==8,"Insufficient complete upload capacity cannot authorize partial suppression.");});
		r.Add("Renderer.Efficiency.ShadowBatchPlan.GroupCapacityAtomic",[](TestContext& t){auto m=Members(8);auto l=Limits();l.maxInstancesPerGroup=3;l.maxGroups=1;auto p=BuildShadowBatchPlan(View(),m,l);t.Expect(!p.coverageComplete && p.groups.empty() && p.singletonIndices.size()==8,"Failure after an earlier planned group restores the complete fallback.");});
		r.Add("Renderer.Efficiency.ShadowBatchPlan.InstanceOffsetOverflow",[](TestContext& t){auto m=Members(3);auto l=Limits();l.firstInstance=std::numeric_limits<std::uint32_t>::max()-1;auto p=BuildShadowBatchPlan(View(),m,l);t.Expect(!p.coverageComplete && p.groups.empty(),"Instance address overflow rejects before any suppression.");});
		r.Add("Renderer.Efficiency.ShadowBatchPlan.SelectedRangeOverflow",[](TestContext& t){auto m=Members(2);m[1].firstMeshlet=std::numeric_limits<std::uint64_t>::max();auto p=BuildShadowBatchPlan(View(),m,Limits());t.Expect(!p.coverageComplete && p.failure==ShadowBatchFailure::InvalidMember && p.groups.empty(),"Wrapped selected ranges cannot prove complete coverage.");});
		r.Add("Renderer.Efficiency.ShadowBatchPlan.QualifiedRegions",[](TestContext& t){auto m=Members(2);auto a=View();auto b=a;b.viewId=2;b.lightId[0]=2;b.allocationGeneration=3;auto pa=BuildShadowBatchPlan(a,m,Limits());auto pb=BuildShadowBatchPlan(b,m,Limits());Coverage(t,pa,2);Coverage(t,pb,2);t.Expect(pa.view==a && pb.view==b && pa.view!=pb.view,"Light and view owners never borrow another region's instance range."); a.resourceKind=static_cast<ShadowResourceKind>(1);a.lightProjectionStamp.cubeProjectionEvidenceComplete=true;for(auto& matrix:a.lightProjectionStamp.cubeViewProjections)matrix[0]=1;for(unsigned face=0;face<6;++face){a.faceIndex=face;auto facePlan=BuildShadowBatchPlan(a,m,Limits());Coverage(t,facePlan,2);t.Expect(facePlan.view.faceIndex==face,"Every cube face retains its independent qualified coverage.");}});
		r.Add("Renderer.Efficiency.ShadowBatchPlan.InvalidViewAndLimits",[](TestContext& t){auto m=Members(2);auto v=View();v.lightId={};auto p=BuildShadowBatchPlan(v,m,Limits());t.Expect(!p.coverageComplete && p.failure==ShadowBatchFailure::InvalidView,"Unknown physical region rejects optimized proof.");auto l=Limits();l.maxTaskGroupsX=0;p=BuildShadowBatchPlan(View(),m,l);t.Expect(!p.coverageComplete && p.failure==ShadowBatchFailure::InvalidLimits,"Unknown device limits cannot authorize a group.");});
		r.Add("Renderer.Efficiency.ShadowBatchPlan.EmptySelectedMembership",[](TestContext& t){auto p=BuildShadowBatchPlan(View(),{},Limits());Coverage(t,p,0);t.Expect(p.groups.empty() && p.singletonIndices.empty(),"An actually empty light cut stays empty.");});
	}
}
