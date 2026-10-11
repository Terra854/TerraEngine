#include "TestHarness.h"
#include "Renderer/RHI/Vulkan/VulkanLightingDepthTiming.h"
#include <limits>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace Renderer::VulkanLightingDepthTiming;
		Owner MakeOwner() { Owner o; o.rendererSessionGeneration = 1; o.backendResourceGeneration = 2; o.queryPoolGeneration = 3; o.frameSerial = 10; o.commandSerial = 11; return o; }
		void Start(TestContext& t, TimingState& state, const Owner& o, unsigned bits = 64, double period = 2.0)
		{
			t.Expect(state.Configure(1, 2, 3, bits, period), "Native timestamp configuration must be accepted.");
			QueryRange range; t.Expect(state.Begin(o, range), "Exact isolated native owner must reserve its query pair.");
		}
		void Complete(TestContext& t, TimingState& s, const Owner& o)
		{
			t.Expect(s.End(o), "Actual matching end marker must be recorded.");
			const std::array commands{o.commandSerial}; t.Expect(s.ConfirmSubmitted(commands, 20), "Actual ordered queue receipt must bind the command.");
			t.Expect(s.CompleteSubmission(20, 21), "Actual completion must advance the same submit.");
		}
	}
	void RegisterLightingDepthTimingTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.LightingDepthTiming.ExactDisjoint96Queries", [](TestContext& t) {
			TimingState s; t.Expect(s.Configure(1,2,3,64,1), "Configure."); std::vector<unsigned> indices;
			for(unsigned frame=0; frame<3; ++frame) for(unsigned snapshot=0; snapshot<8; ++snapshot) for(unsigned purpose=0; purpose<2; ++purpose) {
				auto o=MakeOwner(); o.frameSlot=frame; o.snapshotSlot=snapshot; o.purpose=static_cast<Purpose>(purpose); QueryRange r;
				t.Expect(s.Begin(o,r) && r.count==2 && r.firstQuery==((frame*8+snapshot)*2+purpose)*2, "Every physical owner has exactly its fixed disjoint pair.");
				indices.push_back(r.firstQuery); t.Expect(!s.Begin(o,r), "Recorded range cannot be reset or reused.");
			}
			t.Expect(indices.size()==48 && s.Outstanding()==48 && !s.CanDestroy(), "All96 queries remain owned without eviction.");
		});
		registry.Add("Renderer.Efficiency.LightingDepthTiming.RejectMalformedOwnersAndConfiguration", [](TestContext& t) {
			for(unsigned fault=0; fault<5; ++fault) { TimingState s; t.Expect(!s.Configure(fault==0?0:1,2,3,fault==1?0:(fault==2?65:64),fault==3?0:(fault==4?std::numeric_limits<double>::infinity():1)), "Unsupported timestamps fail closed."); }
			for(unsigned fault=0; fault<10; ++fault) { TimingState s; s.Configure(1,2,3,64,1); auto o=MakeOwner();
				if(fault==0)o.rendererSessionGeneration++; if(fault==1)o.backendResourceGeneration++; if(fault==2)o.queryPoolGeneration++; if(fault==3)o.frameSerial=0; if(fault==4)o.commandSerial=0;
				if(fault==5)o.frameSlot=3; if(fault==6)o.snapshotSlot=8; if(fault==7)o.purpose=static_cast<Purpose>(2); if(fault==8)o.view=static_cast<View>(2); if(fault==9)o.scope=Scope::WholeView;
				QueryRange r{99,99}; t.Expect(!s.Begin(o,r) && r.firstQuery==99 && s.Outstanding()==0,"Bad owner cannot change the query range."); }
		});
		registry.Add("Renderer.Efficiency.LightingDepthTiming.OrderedSubmitAndExactCompletion", [](TestContext& t) {
			TimingState s; const auto o=MakeOwner(); Start(t,s,o); auto wrong=o; wrong.outputId=1; t.Expect(!s.End(wrong),"Different output cannot end an owner.");
			t.Expect(s.End(o),"Record end."); const std::array duplicate{o.commandSerial,o.commandSerial}; t.Expect(!s.ConfirmSubmitted(duplicate,20),"Duplicate queue command receipt rejects atomically.");
			const std::array commands{99ULL,o.commandSerial}; t.Expect(s.ConfirmSubmitted(commands,20),"Other primary commands may share the actual ordered submission.");
			t.Expect(!s.ConfirmSubmitted(commands,22) && !s.CompleteSubmission(20,19),"Wrong submit and older completion do not advance ownership.");
			Result result; t.Expect(!s.Resolve(o,{10,30,true,true,true},true,result),"Queries cannot be read before completion.");
			t.Expect(s.CompleteSubmission(20,21) && s.Resolve(o,{10,30,true,true,true},true,result) && result.status==Status::Valid && result.rawTicks==20 && result.durationNanoseconds==40 && result.submitSerial==20 && result.completionSerial==21,"Actual isolated result is completion qualified.");
			t.Expect(QualifiesProductionSpeedProof(result),"Nonempty production interval may supply stage speed evidence.");
		});
		registry.Add("Renderer.Efficiency.LightingDepthTiming.LateAvailabilityRetainsOwner", [](TestContext& t) {
			TimingState s; const auto o=MakeOwner(); Start(t,s,o); Complete(t,s,o); Result r;
			t.Expect(s.Resolve(o,{10,20,true,false,true},true,r) && r.status==Status::Pending && s.Outstanding()==1,"Missing availability is pending without WAIT or premature range reuse.");
			QueryRange range; t.Expect(!s.Begin(o,range),"Pending query still owns the range.");
			t.Expect(s.Resolve(o,{10,20,true,true,true},true,r) && r.status==Status::Valid && s.CanDestroy(),"Later actual availability completes and releases its pair.");
		});
		registry.Add("Renderer.Efficiency.LightingDepthTiming.TimestampValidBitsAndRollover", [](TestContext& t) {
			for(unsigned bits : {1U,8U,32U,64U}) { TimingState s; const auto o=MakeOwner(); Start(t,s,o,bits); Complete(t,s,o); Result r;
				const auto mask=bits==64?~0ULL:((1ULL<<bits)-1); t.Expect(s.Resolve(o,{mask,0,true,true,true},true,r) && r.status==Status::Valid && r.rawTicks==1 && r.durationNanoseconds==2,"Unsigned masked subtraction handles hardware rollover including64 bits."); }
		});
		registry.Add("Renderer.Efficiency.LightingDepthTiming.IncompleteZeroAndFailedReadUnavailable", [](TestContext& t) {
			for(unsigned fault=0; fault<4; ++fault) { TimingState s; const auto o=MakeOwner(); Start(t,s,o,64,fault==3?std::numeric_limits<double>::max():2.0); if(fault!=0)t.Expect(s.End(o),"Recordend."); const std::array commands{o.commandSerial}; s.ConfirmSubmitted(commands,20); s.CompleteSubmission(20,21); Result r;
				t.Expect(s.Resolve(o,{10,fault==1?10ULL:20ULL,true,true,fault!=2},true,r) && r.status==Status::Unavailable && !QualifiesProductionSpeedProof(r) && s.CanDestroy(),"Missing markers, zero nonempty interval and failed read cannot become valid."); }
		});
		registry.Add("Renderer.Efficiency.LightingDepthTiming.ResetRetainsRecordedAndSubmittedRanges", [](TestContext& t) {
			TimingState s; auto o=MakeOwner(); Start(t,s,o); t.Expect(!s.Reset(4,5,6) && s.Outstanding()==1 && !s.CanDestroy(),"Reset retains recorded old-pool ownership.");
			t.Expect(s.DiscardCommand(o.commandSerial) && s.Reset(4,5,6),"Only actual unsubmitted discard allows newpool configuration."); o.rendererSessionGeneration=4;o.backendResourceGeneration=5;o.queryPoolGeneration=6;o.frameSerial++; QueryRange range; t.Expect(s.Begin(o,range) && s.End(o),"New exactowner records.");
			const std::array commands{o.commandSerial}; s.ConfirmSubmitted(commands,20); t.Expect(!s.DiscardCommand(o.commandSerial) && !s.Reset(7,8,9) && !s.Configure(7,8,9,64,1),"Accepted GPU work cannot be discarded or overwritten by Configure.");
			s.CompleteSubmission(20,21); Result r; t.Expect(s.Resolve(o,{1,2,true,true,true},true,r) && r.status==Status::Unavailable && s.CanDestroy() && s.Reset(7,8,9),"Canceled actual completion releases oldpool without publishing timing.");
		});
		registry.Add("Renderer.Efficiency.LightingDepthTiming.ExplicitCompletedAbandonAndOracleNotSpeedProof", [](TestContext& t) {
			TimingState s; auto o=MakeOwner(); Start(t,s,o); Result r; t.Expect(!s.AbandonCompleted(o,r),"Recorded owners cannot be freed as completed."); Complete(t,s,o);
			t.Expect(s.AbandonCompleted(o,r) && r.status==Status::Unavailable && s.CanDestroy(),"Explicit completed disposal avoids permanently stranded query availability.");
			o.purpose=Purpose::Oracle;o.frameSerial++; QueryRange range; t.Expect(s.Begin(o,range),"Oracle has its separate pair."); Complete(t,s,o);
			t.Expect(s.Resolve(o,{5,15,true,true,true},true,r) && r.status==Status::Valid && !QualifiesProductionSpeedProof(r),"Synthetic oracle duration never substitutes for production speed.");
		});
		registry.Add("Renderer.Efficiency.LightingDepthTiming.ExactOwnerLanesAndEmptyWork", [](TestContext& t) {
			TimingState s; const auto o=MakeOwner(); Start(t,s,o); Complete(t,s,o); Result r;
			for(unsigned fault=0;fault<8;++fault) {auto wrong=o;if(fault==0)wrong.rendererSessionGeneration++;if(fault==1)wrong.queryPoolGeneration++;if(fault==2)wrong.backendResourceGeneration++;if(fault==3)wrong.frameSerial++;if(fault==4)wrong.commandSerial++;if(fault==5)wrong.view=View::Scene;if(fault==6)wrong.outputId++;if(fault==7)wrong.purpose=Purpose::Oracle;
				t.Expect(!s.Resolve(wrong,{1,2,true,true,true},true,r) && s.Outstanding()==1,"Wrong logical owner cannot consume a completed pair.");}
			t.Expect(s.Resolve(o,{1,1,true,true,true},false,r) && r.status==Status::Valid && !QualifiesProductionSpeedProof(r),"Empty zero interval can be reported, but cannot prove nonempty speed.");
		});
	}
}
