#include "TestHarness.h"

#include "Animation/AnimationPresentationSystem.h"
#include "Animation/AnimationSystem.h"
#include "Animation/Diagnostics/AnimationDiagnostics.h"
#include "Core/Scheduling/SystemScheduler.h"
#include "Core/SystemBase.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using TerraEngine::Scheduling::FrameWorkloadDeclaration;
		using TerraEngine::Scheduling::SchedulerPhaseDefinition;
		using TerraEngine::Scheduling::SchedulerResourceDefinition;

		// This declaration-only system lets the test build the real animation
		// ordering graph without initializing windows, physics, or the renderer.
		class DeclarationOnlySystem final : public SystemsBase {
		public:
			explicit DeclarationOnlySystem(TerraEngine::Scheduling::SystemDeclaration declaration)
				: declaration_(std::move(declaration))
			{
			}

			void Initialize() override {}
			void Update(float) override {}
			void Shutdown() override {}

			std::string SystemName() override
			{
				return declaration_.name;
			}

			TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override
			{
				return declaration_;
			}

		private:
			TerraEngine::Scheduling::SystemDeclaration declaration_;
		};

		// The real AnimationSystem remains the sole owner of live instances. This
		// RAII fixture requests protected lifecycle changes and guarantees cleanup
		// even if a contract expectation throws midway through the test.
		class AnimatorMembershipGuard final {
		public:
			AnimatorMembershipGuard(TerraEngine::Animation::AnimationSystem& system, std::size_t count)
				: system_(system)
			{
				handles_.reserve(count);
				for(std::size_t index = 0U; index < count; ++index) {
					TerraEngine::Animation::AnimatorInstanceDescriptor descriptor;
					descriptor.ownerObjectId[0] = static_cast<std::uint8_t>((index % 255U) + 1U);
					descriptor.ownerObjectId[1] = static_cast<std::uint8_t>((index / 255U) + 1U);
					descriptor.componentGeneration = 1U;
					const TerraEngine::Animation::AnimatorInstanceHandle handle = system_.CreateAnimatorInstance(descriptor);
					if(handle.IsValid()) handles_.push_back(handle);
				}
			}

			~AnimatorMembershipGuard()
			{
				for(const TerraEngine::Animation::AnimatorInstanceHandle handle : handles_) {
					(void)system_.InvalidateAnimatorInstance(handle);
				}
			}

			[[nodiscard]] std::size_t Size() const noexcept { return handles_.size(); }
			[[nodiscard]] TerraEngine::Animation::AnimatorInstanceHandle First() const noexcept
			{
				return handles_.empty() ? TerraEngine::Animation::AnimatorInstanceHandle{} : handles_.front();
			}

		private:
			TerraEngine::Animation::AnimationSystem& system_;
			std::vector<TerraEngine::Animation::AnimatorInstanceHandle> handles_;
		};

		bool Contains(const std::vector<std::string>& values, const std::string& expected)
		{
			return std::find(values.begin(), values.end(), expected) != values.end();
		}

		const SchedulerPhaseDefinition* FindPhase(
			const std::vector<SchedulerPhaseDefinition>& phases,
			const std::string& name)
		{
			const auto found = std::find_if(phases.begin(), phases.end(), [&name](const SchedulerPhaseDefinition& phase) {
				return phase.name == name;
			});
			return found == phases.end() ? nullptr : &(*found);
		}

		const SchedulerResourceDefinition* FindResource(
			const std::vector<SchedulerResourceDefinition>& resources,
			const std::string& name)
		{
			const auto found = std::find_if(resources.begin(), resources.end(), [&name](const SchedulerResourceDefinition& resource) {
				return resource.name == name;
			});
			return found == resources.end() ? nullptr : &(*found);
		}

		const FrameWorkloadDeclaration* FindWorkload(
			const std::vector<FrameWorkloadDeclaration>& workloads,
			const std::string& name)
		{
			const auto found = std::find_if(workloads.begin(), workloads.end(), [&name](const FrameWorkloadDeclaration& workload) {
				return workload.workloadName == name;
			});
			return found == workloads.end() ? nullptr : &(*found);
		}

		TerraEngine::Scheduling::SystemDeclaration MakeDependencySystemDeclaration(
			std::string name,
			std::string phase)
		{
			TerraEngine::Scheduling::SystemDeclaration declaration;
			declaration.name = std::move(name);
			declaration.phase = std::move(phase);
			declaration.mainThread = true;
			declaration.profilingLabel = declaration.name;
			declaration.usesDefaultMetadata = false;
			return declaration;
		}

		void ExpectWorkerWorkload(
			TestContext& context,
			const FrameWorkloadDeclaration& workload,
			TerraEngine::Scheduling::FrameWorkloadPartitionPolicy expectedPartitionPolicy =
				TerraEngine::Scheduling::FrameWorkloadPartitionPolicy::DeterministicRanges)
		{
			context.Expect(workload.workerEligible, "Animation preparation must be worker eligible.");
			context.Expect(!workload.mainThreadRequired, "Worker-safe animation preparation must not require the main thread.");
			context.Expect(workload.phaseKind == TerraEngine::Scheduling::WorkerSystemPhaseKind::WorkerPrepare,
				"Worker animation work must identify itself as preparation.");
			context.Expect(workload.boundaryClassification == TerraEngine::Scheduling::HardBoundaryClassification::WorkerSafe,
				"Worker animation work must expose a worker-safe diagnostic classification.");
			context.Expect(workload.partitionPolicy == expectedPartitionPolicy,
				expectedPartitionPolicy == TerraEngine::Scheduling::FrameWorkloadPartitionPolicy::DeterministicRanges
					? "Animation worker work must use stable range partitions rather than per-animator jobs."
					: "The transactional presentation merge must remain one explicitly declared worker task.");
			context.Expect(workload.mutationPolicy == TerraEngine::Scheduling::FrameWorkloadMutationPolicy::None,
				"Worker animation work must only write owned prepared output, never live scene mutation state.");
			context.Expect(!workload.safeSubwork.empty(), "Worker animation work must explain its safe immutable-input work.");
			context.Expect(!workload.profilingLabel.empty(), "Every animation workload must have a visible profiling label.");
		}

		void ExpectProtectedWorkload(TestContext& context, const FrameWorkloadDeclaration& workload)
		{
			context.Expect(!workload.workerEligible, "Animation capture/publication must stay protected.");
			context.Expect(workload.mainThreadRequired, "Animation capture/publication must require the protected coordinator boundary.");
			context.Expect(workload.boundaryClassification == TerraEngine::Scheduling::HardBoundaryClassification::Protected,
				"Animation capture/publication must expose a protected diagnostic classification.");
			context.Expect(workload.hardBoundary, "Animation capture/publication must be visible as a hard boundary.");
			context.Expect(workload.mutationPolicy == TerraEngine::Scheduling::FrameWorkloadMutationPolicy::DeferredOnly,
				"Protected animation work must retain typed/deferred mutation discipline.");
			context.Expect(!workload.protectedReason.empty(), "Protected animation work must explain why it cannot run on workers.");
			context.Expect(!workload.profilingLabel.empty(), "Every protected animation boundary must have a profiling label.");
		}
	}

	void RegisterAnimationSchedulerContractTests(TestRegistry& registry)
	{
		registry.Add("Animation.Scheduler.PhaseOrderAndResourcesAreStable", [](TestContext& context) {
			const std::vector<SchedulerPhaseDefinition> phases = TerraEngine::Scheduling::DefaultSchedulerPhases();
			const SchedulerPhaseDefinition* scripting = FindPhase(phases, "Scripting");
			const SchedulerPhaseDefinition* animation = FindPhase(phases, "Animation");
			const SchedulerPhaseDefinition* physics = FindPhase(phases, "Physics");
			const SchedulerPhaseDefinition* presentation = FindPhase(phases, "AnimationPresentation");
			const SchedulerPhaseDefinition* ui = FindPhase(phases, "UI");
			const SchedulerPhaseDefinition* render = FindPhase(phases, "Render");
			context.Expect(scripting != nullptr && animation != nullptr && physics != nullptr
				&& presentation != nullptr && ui != nullptr && render != nullptr,
				"The complete animation phase chain must be declared.");
			if(scripting == nullptr || animation == nullptr || physics == nullptr
				|| presentation == nullptr || ui == nullptr || render == nullptr) {
				return;
			}
			context.Expect(scripting->order < animation->order && animation->order < physics->order
				&& physics->order < presentation->order && presentation->order < ui->order
				&& ui->order < render->order,
				"Animation authority must run after scripts and before physics; presentation must run after physics and before UI/render.");

			const std::vector<SchedulerResourceDefinition> resources = TerraEngine::Scheduling::DefaultSchedulerResources();
			const std::array<const char*, 19> requiredResources = {
				TerraEngine::Scheduling::kResourceAnimatorComponents,
				TerraEngine::Scheduling::kResourceAnimationCommandQueues,
				TerraEngine::Scheduling::kResourceAnimationFrameInputs,
				TerraEngine::Scheduling::kResourceAnimationEvaluationPrograms,
				TerraEngine::Scheduling::kResourceAnimationResidencyCapabilities,
				TerraEngine::Scheduling::kResourceAnimationExecutionModes,
				TerraEngine::Scheduling::kResourceAnimationConsumerMasks,
				TerraEngine::Scheduling::kResourceAnimationWorkPackets,
				TerraEngine::Scheduling::kResourceAnimationVisualPackets,
				TerraEngine::Scheduling::kResourceAnimationClipSegments,
				TerraEngine::Scheduling::kResourceAnimationRollbackState,
				TerraEngine::Scheduling::kResourceAnimationAuthoritativeOutputs,
				TerraEngine::Scheduling::kResourceAnimationRuntimeState,
				TerraEngine::Scheduling::kResourceAnimationPhysicsHandoffQueue,
				TerraEngine::Scheduling::kResourceAnimationManagedCallbackBatches,
				TerraEngine::Scheduling::kResourceAnimationRagdollMappings,
				TerraEngine::Scheduling::kResourceAnimationRecordingSamples,
				TerraEngine::Scheduling::kResourceAnimationPresentationPrepared,
				TerraEngine::Scheduling::kResourceAnimationPresentationPlan
			};
			for(const char* required : requiredResources) {
				const SchedulerResourceDefinition* resource = FindResource(resources, required);
				context.Expect(resource != nullptr, "Every animation handoff resource must be visible in scheduler diagnostics.");
				if(resource != nullptr) {
					context.Expect(!resource->description.empty(), "Animation scheduler resources must explain their ownership and visibility.");
				}
			}
		});

		registry.Add("Animation.Scheduler.SystemDeclarationsExposeProtectedHandoffs", [](TestContext& context) {
			static_assert(std::derived_from<TerraEngine::Animation::AnimationSystem, SystemsBase>);
			static_assert(std::derived_from<TerraEngine::Animation::AnimationPresentationSystem, SystemsBase>);

			TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
			TerraEngine::Animation::AnimationPresentationSystem& presentationSystem = TerraEngine::Animation::AnimationPresentationSystem::Instance();
			const TerraEngine::Scheduling::SystemDeclaration animation = animationSystem.SchedulerDeclaration();
			const TerraEngine::Scheduling::SystemDeclaration presentation = presentationSystem.SchedulerDeclaration();

			context.ExpectEqual(std::string("Animation"), animation.phase,
				"AnimationSystem must own the pre-physics Animation phase.");
			context.Expect(Contains(animation.after, "ScriptingSystem") && Contains(animation.before, "PhysicsSystem"),
				"AnimationSystem must be explicitly ordered between scripts and physics.");
			context.Expect(animation.requiresFlushedSceneMutations,
				"Animation capture must consume the last completed structural flush; script-submitted membership follows the documented next-frame rule.");
			context.Expect(animation.boundaryClassification == TerraEngine::Scheduling::HardBoundaryClassification::PartiallyWorkerSafe
				&& animation.hardBoundary && !animation.safeSubwork.empty() && !animation.protectedCommit.empty(),
				"AnimationSystem must diagnose both worker planning and protected authority publication.");
			context.Expect(animation.omitWhenNoFrameWorkloads
				&& !animation.usesDefaultMetadata && animation.profilingLabel == "Animation",
				"AnimationSystem must expose stable non-default scheduling diagnostics.");

			context.ExpectEqual(std::string("AnimationPresentation"), presentation.phase,
				"AnimationPresentationSystem must own the post-physics presentation phase.");
			context.Expect(Contains(presentation.after, "PhysicsSystem")
				&& Contains(presentation.before, "UISystem") && Contains(presentation.before, "RenderSystem"),
				"Animation presentation must be explicitly ordered after physics and before UI/render.");
			context.Expect(!presentation.requiresFlushedSceneMutations,
				"Presentation must consume immutable physics snapshots without forcing an extra live-scene flush.");
			context.Expect(presentation.boundaryClassification == TerraEngine::Scheduling::HardBoundaryClassification::PartiallyWorkerSafe
				&& presentation.hardBoundary && !presentation.safeSubwork.empty() && !presentation.protectedCommit.empty(),
				"AnimationPresentationSystem must diagnose worker preparation and protected plan publication.");
			context.Expect(presentation.omitWhenNoFrameWorkloads
				&& !presentation.usesDefaultMetadata && presentation.profilingLabel == "AnimationPresentation",
				"Animation presentation must expose stable non-default scheduling diagnostics.");

			DeclarationOnlySystem scripting(MakeDependencySystemDeclaration("ScriptingSystem", "Scripting"));
			DeclarationOnlySystem physics(MakeDependencySystemDeclaration("PhysicsSystem", "Physics"));
			DeclarationOnlySystem ui(MakeDependencySystemDeclaration("UISystem", "UI"));
			DeclarationOnlySystem render(MakeDependencySystemDeclaration("RenderSystem", "Render"));
			TerraEngine::Scheduling::SystemScheduler scheduler;
			scheduler.RegisterSystem(scripting);
			scheduler.RegisterSystem(animationSystem);
			scheduler.RegisterSystem(physics);
			scheduler.RegisterSystem(presentationSystem);
			scheduler.RegisterSystem(ui);
			scheduler.RegisterSystem(render);
			const TerraEngine::Scheduling::ScheduleSummary& summary = scheduler.BuildSchedule();
			context.Expect(summary.valid, "The animation authority/presentation declarations must form a valid scheduler graph.");
			context.Expect(summary.systems.size() >= 4U, "A valid animation fixture must contain the four authority/handoff systems.");
			if(summary.systems.size() >= 4U) {
				context.ExpectEqual(std::string("ScriptingSystem"), summary.systems[0], "Scripting must begin the animation contract fixture.");
				context.ExpectEqual(std::string("AnimationSystem"), summary.systems[1], "Animation authority must follow scripting.");
				context.ExpectEqual(std::string("PhysicsSystem"), summary.systems[2], "Physics must consume animation authority next.");
				context.ExpectEqual(std::string("AnimationPresentationSystem"), summary.systems[3], "Presentation must consume the current physics result.");
			}
		});

		registry.Add("Animation.Presentation.OwnerMappingUsesStableSlotGenerationIdentity", [](TestContext& context) {
			TerraEngine::Animation::GpuAnimationOwnerObjectMapping mapping{};
			mapping.ownerObjectId[0] = 71U;
			mapping.instanceSlot = 4U;
			mapping.instanceGeneration = 5U;
			mapping.componentGeneration = 6U;
			mapping.stableInstanceId = (static_cast<std::uint64_t>(mapping.instanceSlot) << 32U)
				| static_cast<std::uint64_t>(mapping.instanceGeneration);
			context.Expect(mapping.IsComplete(),
				"Presentation must preserve the established slot-high/generation-low stable identity for GPU-only object matching.");
			context.Expect(!mapping.HasPublishedPacketIndex(),
				"A producer mapping must remain unusable for direct render lookup until lowering publishes its exact packet index.");
			mapping.workPacketIndex = 19U;
			context.Expect(mapping.HasPublishedPacketIndex(),
				"A lowered owner mapping must expose its exact packet index without a renderer-side identity table.");

			mapping.stableInstanceId = (static_cast<std::uint64_t>(mapping.instanceGeneration) << 32U)
				| static_cast<std::uint64_t>(mapping.instanceSlot + 1U);
			context.Expect(!mapping.IsComplete(),
				"The former generation-high/slot-plus-one encoding must fail closed before RenderSystem can bind a GPU-only packet.");
		});

		registry.Add("Animation.Presentation.OwnerLookupFindsFirstExactMapping", [](TestContext& context) {
			TerraEngine::Animation::PublishedGpuAnimationPresentationPlan plan;
			for(std::uint32_t index = 0U; index < 32U; ++index) {
				TerraEngine::Animation::GpuAnimationOwnerObjectMapping mapping;
				mapping.ownerObjectId[0] = static_cast<std::uint8_t>(index + 1U);
				mapping.stableInstanceId = static_cast<std::uint64_t>(index + 1U);
				mapping.workPacketIndex = index;
				plan.ownerObjectMappings.push_back(mapping);
			}
			// A duplicate owner remains adjacent after canonical publication sorting.
			// The lookup must name the first record so RenderSystem can detect ambiguity.
			TerraEngine::Animation::GpuAnimationOwnerObjectMapping duplicate =
				plan.ownerObjectMappings[11U];
			duplicate.workPacketIndex = 99U;
			plan.ownerObjectMappings.insert(
				plan.ownerObjectMappings.begin() + 12U, duplicate);
			plan.BuildOwnerObjectLookup();

			const TerraEngine::Animation::GpuAnimationOwnerObjectMapping* found =
				plan.FindFirstOwnerObjectMapping(plan.ownerObjectMappings[11U].ownerObjectId);
			context.Expect(found == &plan.ownerObjectMappings[11U]
				&& found->workPacketIndex == 11U,
				"The immutable owner index must resolve the first exact sorted mapping.");
			TerraEngine::UUID missing{};
			missing[0] = 201U;
			context.Expect(plan.FindFirstOwnerObjectMapping(missing) == nullptr,
				"An absent owner must fail closed without falling through to another bucket.");
		});

		registry.Add("Animation.Binding.SharedArraysDetachOnlyOnMutation", [](TestContext& context) {
			TerraEngine::Animation::SharedCopyVector<std::uint64_t> original{
				11U, 22U, 33U};
			TerraEngine::Animation::SharedCopyVector<std::uint64_t> copied = original;
			context.Expect(original.data() == copied.data(),
				"A frame binding copy must retain the same immutable array storage.");

			copied.Mutable()[1U] = 99U;
			context.Expect(original.data() != copied.data(),
				"The first mutation must detach without changing an older frame snapshot.");
			context.ExpectEqual(static_cast<std::uint64_t>(22U), original[1U],
				"The original immutable binding array must preserve its captured value.");
			context.ExpectEqual(static_cast<std::uint64_t>(99U), copied[1U],
				"The detached writer must publish its replacement value independently.");
		});

		registry.Add("Animation.Scheduler.WorkloadsSeparateWorkerPreparationFromPublication", [](TestContext& context) {
			TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
			TerraEngine::Animation::AnimationPresentationSystem& presentationSystem = TerraEngine::Animation::AnimationPresentationSystem::Instance();
			AnimatorMembershipGuard membership(animationSystem, 256U);
			context.ExpectEqual(static_cast<std::size_t>(256U), membership.Size(),
				"The scheduler fixture must create every protected runtime membership record.");
			const std::vector<FrameWorkloadDeclaration> animationWorkloads = animationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
			const std::vector<FrameWorkloadDeclaration> presentationWorkloads = presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F);

			context.ExpectEqual(static_cast<std::size_t>(5U), animationWorkloads.size(),
				"AnimationSystem must expose capture, planning, single-plan finalization, authoritative evaluation, and publication.");
			context.ExpectEqual(static_cast<std::size_t>(2U), presentationWorkloads.size(),
				"AnimationPresentationSystem must expose worker preparation and protected publication.");
			const FrameWorkloadDeclaration* capture = FindWorkload(animationWorkloads, "CaptureAnimationInputs");
			const FrameWorkloadDeclaration* plan = FindWorkload(animationWorkloads, "PlanAnimationEvaluation");
			const FrameWorkloadDeclaration* finalize = FindWorkload(animationWorkloads, "FinalizeAnimationWorkPlan");
			const FrameWorkloadDeclaration* evaluate = FindWorkload(animationWorkloads, "EvaluateAuthoritativeAnimation");
			const FrameWorkloadDeclaration* publish = FindWorkload(animationWorkloads, "PublishAnimationAuthority");
			const FrameWorkloadDeclaration* preparePresentation = FindWorkload(presentationWorkloads, "PrepareAnimationPresentation");
			const FrameWorkloadDeclaration* publishPresentation = FindWorkload(presentationWorkloads, "PublishAnimationPresentation");
			context.Expect(capture != nullptr && plan != nullptr && finalize != nullptr
				&& evaluate != nullptr && publish != nullptr
				&& preparePresentation != nullptr && publishPresentation != nullptr,
				"Every frozen animation workload name must be declared.");
			if(capture == nullptr || plan == nullptr || finalize == nullptr
				|| evaluate == nullptr || publish == nullptr
				|| preparePresentation == nullptr || publishPresentation == nullptr) {
				return;
			}
			for(const FrameWorkloadDeclaration& workload : animationWorkloads) {
				const std::size_t expectedItems = workload.workloadName == "FinalizeAnimationWorkPlan"
					? 1U : 256U;
				context.ExpectEqual(expectedItems, workload.estimatedWorkItemCount,
					"Per-animator stages must freeze membership while finalization remains one atomic plan build.");
			}

			ExpectProtectedWorkload(context, *capture);
			context.Expect(capture->phaseKind == TerraEngine::Scheduling::WorkerSystemPhaseKind::Snapshot,
				"Live component/command capture must be identified as a protected snapshot.");
			ExpectWorkerWorkload(context, *plan);
			context.Expect(plan->partitionWorkItemCount == 128U
				&& evaluate->partitionWorkItemCount == 128U,
				"Full-rate crowd planning and evaluation should use one practical worker-pool wave instead of dispatching 64-item microtasks.");
			ExpectWorkerWorkload(context, *finalize,
				TerraEngine::Scheduling::FrameWorkloadPartitionPolicy::Single);
			ExpectWorkerWorkload(context, *evaluate);
			ExpectProtectedWorkload(context, *publish);
			context.Expect(publish->phaseKind == TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit
				&& publish->sameFrameVisibilityRequired,
				"Authority publication must be a same-frame protected handoff to physics.");

			ExpectWorkerWorkload(context, *preparePresentation,
				TerraEngine::Scheduling::FrameWorkloadPartitionPolicy::Single);
			ExpectProtectedWorkload(context, *publishPresentation);
			context.Expect(publishPresentation->phaseKind == TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit
				&& publishPresentation->sameFrameVisibilityRequired,
				"Presentation publication must be protected before UI/render consume the immutable plan.");

			context.Expect(Contains(capture->reads, TerraEngine::Scheduling::kResourceAnimatorComponents)
				&& Contains(capture->reads, TerraEngine::Scheduling::kResourceAnimationCommandQueues)
				&& Contains(capture->writes, TerraEngine::Scheduling::kResourceAnimationFrameInputs),
				"Capture must snapshot live animator inputs into the immutable frame input resource.");
			context.Expect(Contains(plan->reads, TerraEngine::Scheduling::kResourceAnimationFrameInputs)
				&& Contains(plan->writes, TerraEngine::Scheduling::kResourceAnimationWorkPackets)
				&& Contains(plan->writes, TerraEngine::Scheduling::kResourceAnimationVisualPackets),
				"Planning must consume immutable inputs and emit typed work/visual packets.");
			context.Expect(Contains(finalize->reads, TerraEngine::Scheduling::kResourceAnimationWorkPackets)
				&& Contains(finalize->writes, TerraEngine::Scheduling::kResourceAnimationWorkPackets),
				"Finalization must convert the complete planned packet set into one immutable scheduler plan.");
			context.Expect(Contains(evaluate->reads, TerraEngine::Scheduling::kResourceAnimationWorkPackets)
				&& Contains(evaluate->writes, TerraEngine::Scheduling::kResourceAnimationAuthoritativeOutputs),
				"Authoritative evaluation must write only prepared output owned by its frame workload.");
			context.Expect(Contains(publish->reads, TerraEngine::Scheduling::kResourceAnimationAuthoritativeOutputs)
				&& Contains(publish->writes, TerraEngine::Scheduling::kResourceAnimationPhysicsHandoffQueue)
				&& Contains(publish->writes, TerraEngine::Scheduling::kResourceAnimationManagedCallbackBatches),
				"Protected authority publication must feed typed physics and copied managed handoffs.");
			context.Expect(Contains(preparePresentation->reads, "PhysicsPublishedSnapshots")
				&& Contains(preparePresentation->writes, TerraEngine::Scheduling::kResourceAnimationPresentationPrepared),
				"Presentation preparation must consume immutable current-step physics snapshots.");
			context.Expect(Contains(publishPresentation->reads, TerraEngine::Scheduling::kResourceAnimationPresentationPrepared)
				&& Contains(publishPresentation->writes, TerraEngine::Scheduling::kResourceAnimationPresentationPlan),
				"Only the protected presentation boundary may publish the immutable render-facing plan.");

			context.Expect(!plan->explicitDependencies.empty() && capture->stableDescriptorId == plan->explicitDependencies.front(),
				"Planning must wait for the protected immutable input capture.");
			context.Expect(!finalize->explicitDependencies.empty()
				&& plan->stableDescriptorId == finalize->explicitDependencies.front(),
				"Single-plan finalization must wait for every planning range.");
			context.Expect(!evaluate->explicitDependencies.empty()
				&& finalize->stableDescriptorId == evaluate->explicitDependencies.front(),
				"Authoritative evaluation must wait for the immutable plan.");
			context.Expect(!publish->explicitDependencies.empty() && evaluate->stableDescriptorId == publish->explicitDependencies.front(),
				"Authority publication must wait for all worker evaluation.");
			context.Expect(!publishPresentation->explicitDependencies.empty()
				&& preparePresentation->stableDescriptorId == publishPresentation->explicitDependencies.front(),
				"Presentation publication must wait for worker preparation.");

		});

		registry.Add("Animation.Scheduler.CancelledPresentationPreparationDoesNotPoisonNextFrame", [](TestContext& context) {
			TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
			TerraEngine::Animation::AnimationPresentationSystem& presentationSystem = TerraEngine::Animation::AnimationPresentationSystem::Instance();
			AnimatorMembershipGuard membership(animationSystem, 32U);
			(void)animationSystem.DeclareFrameWorkloads(1.0F / 60.0F);

			std::vector<FrameWorkloadDeclaration> cancelledFrame = presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
			context.ExpectEqual(static_cast<std::size_t>(2U), cancelledFrame.size(),
				"The cancellation fixture requires prepare and publication descriptors.");
			if(cancelledFrame.size() != 2U) return;
			// Execute preparation alone to model cancellation before the protected
			// publication callback. Its private item count must not leak forward.
			std::vector<FrameWorkloadDeclaration> prepareOnly{cancelledFrame.front()};
			const TerraEngine::Scheduling::FrameExecutionPlan partial =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(prepareOnly), "animation-presentation-cancelled");
			context.Expect(partial.valid && partial.completed,
				"The fixture's worker preparation must complete before simulated cancellation.");

			const std::uint64_t generationBefore = presentationSystem.PresentationPublicationGeneration();
			std::vector<FrameWorkloadDeclaration> nextFrame = presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
			const TerraEngine::Scheduling::FrameExecutionPlan recovered =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(nextFrame), "animation-presentation-recovered");
			context.Expect(recovered.valid && recovered.completed,
				"The frame after cancelled preparation must execute normally.");
			context.ExpectEqual(generationBefore + 1U, presentationSystem.PresentationPublicationGeneration(),
				"A stale partial preparation count must not block the next protected publication.");
			context.ExpectEqual(static_cast<std::size_t>(32U), presentationSystem.PublishedAnimatorCount(),
				"Recovered publication must describe the complete current membership only.");
			const std::uint64_t recoveredGeneration = presentationSystem.PresentationPublicationGeneration();
			std::vector<FrameWorkloadDeclaration> latePublish{cancelledFrame.back()};
			// The dependency ran before cancellation, so invoke the retained protected
			// closure alone to model a late scheduler completion from that old frame.
			latePublish.front().explicitDependencies.clear();
			const TerraEngine::Scheduling::FrameExecutionPlan rejectedLatePublish =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(latePublish), "animation-presentation-cancelled-late-publish");
			context.Expect(rejectedLatePublish.valid && rejectedLatePublish.completed,
				"The scheduler may complete a retained callback, but its frame token must fail closed.");
			context.ExpectEqual(recoveredGeneration, presentationSystem.PresentationPublicationGeneration(),
				"A cancelled frame must not publish after a newer frame has replaced its declaration token.");
			context.ExpectEqual(static_cast<std::size_t>(32U), presentationSystem.PublishedAnimatorCount(),
				"Rejected late publication must preserve the newer complete presentation plan.");
		});

		registry.Add("Animation.Scheduler.PresentationCapturesSameFrameSubmissionAndRejectsStaleGeneration", [](TestContext& context) {
			TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
			TerraEngine::Animation::AnimationPresentationSystem& presentationSystem = TerraEngine::Animation::AnimationPresentationSystem::Instance();
			// Isolate the singleton's monotonic submission stream so this regression
			// does not depend on an earlier test's submitted source generation.
			presentationSystem.Shutdown();
			presentationSystem.Initialize();
			AnimatorMembershipGuard membership(animationSystem, 1U);
			(void)animationSystem.DeclareFrameWorkloads(1.0F / 60.0F);

			std::vector<FrameWorkloadDeclaration> first = presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
			auto firstFrame = std::make_shared<TerraEngine::Animation::GpuAnimationPresentationFrame>();
			firstFrame->sourceGeneration = 1U;
			context.Expect(presentationSystem.SubmitGpuAnimationPresentationFrame(firstFrame) == TerraEngine::Animation::AnimationStatus::Ok,
				"A frame submitted after declarations but before worker preparation must be accepted.");
			const TerraEngine::Scheduling::FrameExecutionPlan firstPlan =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(std::move(first), "animation-presentation-same-frame");
			const auto firstPublication = presentationSystem.LoadPublishedGpuAnimationPresentationPlan();
			context.Expect(firstPlan.valid && firstPlan.completed && firstPublication
				&& firstPublication->sourceGeneration == 1U,
				"Presentation preparation must capture the same-frame producer handoff, not a declaration-time snapshot.");

			std::vector<FrameWorkloadDeclaration> second = presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
			auto replacementFrame = std::make_shared<TerraEngine::Animation::GpuAnimationPresentationFrame>();
			replacementFrame->sourceGeneration = 2U;
			auto staleFrame = std::make_shared<TerraEngine::Animation::GpuAnimationPresentationFrame>();
			staleFrame->sourceGeneration = 1U;
			context.Expect(presentationSystem.SubmitGpuAnimationPresentationFrame(replacementFrame) == TerraEngine::Animation::AnimationStatus::Ok
				&& presentationSystem.SubmitGpuAnimationPresentationFrame(staleFrame) == TerraEngine::Animation::AnimationStatus::StaleHandle,
				"A stale producer must not replace a newer immutable presentation source.");
			const TerraEngine::Scheduling::FrameExecutionPlan secondPlan =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(std::move(second), "animation-presentation-stale-source");
			const auto secondPublication = presentationSystem.LoadPublishedGpuAnimationPresentationPlan();
			context.Expect(secondPlan.valid && secondPlan.completed && secondPublication
				&& secondPublication->sourceGeneration == 2U,
				"Protected publication must validate and expose one captured source generation without mixing stale work.");
			presentationSystem.Shutdown();
			presentationSystem.Initialize();
		});

		registry.Add("Animation.Scheduler.ProtectedCaptureDrainsTypedCommandsBeforeWorkerPlanning", [](TestContext& context) {
			TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
			AnimatorMembershipGuard membership(animationSystem, 1U);
			TerraEngine::Animation::AnimationCommand command;
			command.target = membership.First();
			command.kind = TerraEngine::Animation::AnimationCommandKind::Stop;
			command.targetTick = 0;
			command.producerId = 7U;
			command.producerSequence = 1U;
			command.correlationId = 99U;
			const TerraEngine::Animation::AnimationCommandReceipt receipt = animationSystem.SubmitCommand(command);
			context.Expect(receipt.status == TerraEngine::Animation::AnimationStatus::Queued,
				"A valid typed command must enter the cross-producer queue before capture.");
			context.ExpectEqual(static_cast<std::size_t>(1U), animationSystem.PendingCommandCount(),
				"The command must remain queued until the protected capture boundary.");

			std::vector<FrameWorkloadDeclaration> workloads = animationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
			context.Expect(!workloads.empty(), "Active animation membership must expose its capture workload.");
			if(workloads.empty()) return;
			std::vector<FrameWorkloadDeclaration> captureOnly{workloads.front()};
			const TerraEngine::Scheduling::FrameExecutionPlan capturePlan =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(captureOnly), "animation-command-capture");
			context.Expect(capturePlan.valid && capturePlan.completed,
				"Protected input capture must complete without invoking worker evaluation.");
			context.ExpectEqual(static_cast<std::size_t>(0U), animationSystem.PendingCommandCount(),
				"Captured due commands must no longer remain in the producer queue.");
			context.ExpectEqual(static_cast<std::size_t>(1U), animationSystem.CapturedCommandCount(),
				"Worker planning must receive one copied command snapshot, not live queue access.");
		});

		registry.Add("Animation.Scheduler.CommandSubmissionCannotResurrectShutdownOwnership", [](TestContext& context) {
			TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
			animationSystem.Initialize();
			{
				AnimatorMembershipGuard membership(animationSystem, 1U);
				TerraEngine::Animation::AnimationCommand command;
				command.target = membership.First();
				command.kind = TerraEngine::Animation::AnimationCommandKind::Stop;
				command.targetTick = 0;
				command.producerId = 8U;
				command.producerSequence = 1U;
				command.correlationId = 100U;

				std::atomic_bool submit = false;
				TerraEngine::Animation::AnimationCommandReceipt racedReceipt;
				std::thread producer([&]() {
					while(!submit.load(std::memory_order_acquire)) std::this_thread::yield();
					racedReceipt = animationSystem.SubmitCommand(command);
				});
				submit.store(true, std::memory_order_release);
				animationSystem.Shutdown();
				producer.join();
				context.Expect(racedReceipt.status == TerraEngine::Animation::AnimationStatus::Queued
					|| racedReceipt.status == TerraEngine::Animation::AnimationStatus::Cancelled,
					"A command racing protected shutdown must finish atomically as queued-before-shutdown or cancelled-after-shutdown.");

				command.producerSequence = 2U;
				command.correlationId = 101U;
				const TerraEngine::Animation::AnimationCommandReceipt afterShutdown = animationSystem.SubmitCommand(command);
				context.Expect(afterShutdown.status == TerraEngine::Animation::AnimationStatus::Cancelled,
					"Cross-producer submission after shutdown must not recreate runtime owners.");
				context.ExpectEqual(static_cast<std::size_t>(0U), animationSystem.PendingCommandCount(),
					"A cancelled post-shutdown command must create no hidden queue storage.");
			}
			// Restore the process-wide singleton for later tests while keeping
			// allocation/lifetime mutation on the protected coordinator thread.
			animationSystem.Initialize();
		});

		registry.Add("Animation.Scheduler.RestartRetiresHandlesCommandsAndAuthorityGeneration", [](TestContext& context) {
			TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
			animationSystem.Initialize();

			TerraEngine::Animation::AnimatorInstanceDescriptor originalDescriptor;
			originalDescriptor.ownerObjectId[0] = 41U;
			originalDescriptor.componentGeneration = 1U;
			const TerraEngine::Animation::AnimatorInstanceHandle original =
				animationSystem.CreateAnimatorInstance(originalDescriptor);
			context.Expect(original.IsValid(), "The restart fixture requires one live original animator instance.");

			TerraEngine::Animation::AnimationCommand queuedBeforeShutdown;
			queuedBeforeShutdown.target = original;
			queuedBeforeShutdown.kind = TerraEngine::Animation::AnimationCommandKind::Stop;
			queuedBeforeShutdown.targetTick = 100;
			queuedBeforeShutdown.producerId = 19U;
			queuedBeforeShutdown.producerSequence = 1U;
			queuedBeforeShutdown.correlationId = 201U;
			context.Expect(animationSystem.SubmitCommand(queuedBeforeShutdown).status
				== TerraEngine::Animation::AnimationStatus::Queued,
				"A future command must be present so shutdown command cancellation is exercised.");
			TerraEngine::Animation::AnimationCommand capturedBeforeShutdown = queuedBeforeShutdown;
			capturedBeforeShutdown.targetTick = 0;
			capturedBeforeShutdown.producerSequence = 2U;
			capturedBeforeShutdown.correlationId = 202U;
			context.Expect(animationSystem.SubmitCommand(capturedBeforeShutdown).status
				== TerraEngine::Animation::AnimationStatus::Queued,
				"A due command must be present so shutdown copied-work clearing is exercised.");

			std::vector<FrameWorkloadDeclaration> originalFrame =
				animationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
			const TerraEngine::Scheduling::FrameExecutionPlan originalPlan =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(originalFrame), "animation-restart-original");
			context.Expect(originalPlan.valid && originalPlan.completed,
				"The original animator must publish a complete authority generation before shutdown.");
			context.ExpectEqual(static_cast<std::size_t>(1U), animationSystem.PendingCommandCount(),
				"The future command must still be pending immediately before shutdown.");
			context.ExpectEqual(static_cast<std::size_t>(1U), animationSystem.CapturedCommandCount(),
				"The due command must be copied into captured work immediately before shutdown.");
			const std::uint64_t generationBeforeShutdown = animationSystem.AuthorityPublicationGeneration();

			animationSystem.Shutdown();
			animationSystem.Shutdown();
			const std::uint64_t retiredGeneration = animationSystem.AuthorityPublicationGeneration();
			context.Expect(retiredGeneration > generationBeforeShutdown,
				"Shutdown must retire the last authority generation so a later session cannot alias it.");
			context.Expect(!animationSystem.IsAnimatorInstanceAlive(original),
				"Shutdown must invalidate the original handle even though the generation history is retained.");
			context.ExpectEqual(static_cast<std::size_t>(0U), animationSystem.PendingCommandCount(),
				"Shutdown must discard commands captured from the retired runtime session.");
			context.ExpectEqual(static_cast<std::size_t>(0U), animationSystem.CapturedCommandCount(),
				"Shutdown must discard copied command work before a later session starts.");

			animationSystem.Initialize();
			context.ExpectEqual(retiredGeneration, animationSystem.AuthorityPublicationGeneration(),
				"Initialize must preserve the retired authority generation instead of restarting it at zero.");
			TerraEngine::Animation::AnimatorInstanceDescriptor replacementDescriptor;
			replacementDescriptor.ownerObjectId[0] = 42U;
			replacementDescriptor.componentGeneration = 1U;
			const TerraEngine::Animation::AnimatorInstanceHandle replacement =
				animationSystem.CreateAnimatorInstance(replacementDescriptor);
			context.Expect(replacement.IsValid()
				&& replacement.slotIndex == original.slotIndex
				&& replacement.generation > original.generation,
				"Restart must preserve the slot's history and advance its generation for the replacement.");
			context.Expect(!animationSystem.IsAnimatorInstanceAlive(original)
				&& animationSystem.IsAnimatorInstanceAlive(replacement),
				"The stale original identity must remain dead while only its replacement is live.");

			TerraEngine::Animation::AnimationCommand staleAfterRestart = queuedBeforeShutdown;
			staleAfterRestart.targetTick = 0;
			staleAfterRestart.producerSequence = 3U;
			staleAfterRestart.correlationId = 203U;
			context.Expect(animationSystem.SubmitCommand(staleAfterRestart).status
				== TerraEngine::Animation::AnimationStatus::StaleHandle,
				"A command carrying the retired handle must not enter the replacement session's queue.");
			context.ExpectEqual(static_cast<std::size_t>(0U), animationSystem.PendingCommandCount(),
				"Neither pre-shutdown nor post-restart stale work may remain queued for the replacement.");
			context.Expect(animationSystem.IsAnimatorInstanceAlive(replacement),
				"Rejecting stale work must leave the replacement animator alive and unchanged.");

			std::vector<FrameWorkloadDeclaration> replacementFrame =
				animationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
			const TerraEngine::Scheduling::FrameExecutionPlan replacementPlan =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(replacementFrame), "animation-restart-replacement");
			context.Expect(replacementPlan.valid && replacementPlan.completed,
				"The replacement animator must publish normally after stale work is rejected.");
			context.Expect(animationSystem.AuthorityPublicationGeneration() > retiredGeneration,
				"The replacement publication must advance beyond the lifecycle retirement generation.");
			context.ExpectEqual(static_cast<std::size_t>(0U), animationSystem.CapturedCommandCount(),
				"The replacement frame must capture no command from the retired session or stale submission.");
			const std::uint64_t replacementGeneration = animationSystem.AuthorityPublicationGeneration();
			animationSystem.Initialize();
			context.ExpectEqual(replacementGeneration, animationSystem.AuthorityPublicationGeneration(),
				"Repeated Initialize while active must be idempotent and preserve the published identity.");
			context.ExpectEqual(static_cast<std::size_t>(1U), animationSystem.PublishedAnimatorCount(),
				"Idempotent Initialize must not erase a complete live publication under the same generation.");
			context.Expect(animationSystem.IsAnimatorInstanceAlive(replacement),
				"Idempotent Initialize must leave the active replacement instance untouched.");

			(void)animationSystem.InvalidateAnimatorInstance(replacement);
			animationSystem.Shutdown();
			// Restore the process-wide singleton for subsequent scheduler tests.
			animationSystem.Initialize();
		});

		registry.Add("Animation.Scheduler.PresentationLifecycleRetiresAndRejectsPostShutdownWork", [](TestContext& context) {
			TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
			TerraEngine::Animation::AnimationPresentationSystem& presentationSystem = TerraEngine::Animation::AnimationPresentationSystem::Instance();
			animationSystem.Initialize();
			presentationSystem.Initialize();
			{
				AnimatorMembershipGuard membership(animationSystem, 2U);
				std::vector<FrameWorkloadDeclaration> authority = animationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
				const TerraEngine::Scheduling::FrameExecutionPlan authorityPlan =
					TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
						std::move(authority), "animation-presentation-lifecycle-authority");
				context.Expect(authorityPlan.valid && authorityPlan.completed,
					"The lifecycle fixture must publish active authority membership before presentation work is declared.");

				std::vector<FrameWorkloadDeclaration> initialPresentation =
					presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
				const TerraEngine::Scheduling::FrameExecutionPlan initialPlan =
					TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
						std::move(initialPresentation), "animation-presentation-lifecycle-initial");
				context.Expect(initialPlan.valid && initialPlan.completed,
					"The lifecycle fixture must publish one complete render-facing plan before shutdown.");
				context.ExpectEqual(static_cast<std::size_t>(2U), presentationSystem.PublishedAnimatorCount(),
					"The complete presentation plan must expose the active membership count.");
				const std::uint64_t generationBeforeShutdown =
					presentationSystem.PresentationPublicationGeneration();
				// Bind a second frame without executing it. Its closures model scheduler
				// work retained across cancellation at the lifecycle boundary.
				std::vector<FrameWorkloadDeclaration> retiredPresentation =
					presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F);

				presentationSystem.Shutdown();
				presentationSystem.Shutdown();
				const std::uint64_t retiredGeneration =
					presentationSystem.PresentationPublicationGeneration();
				context.ExpectEqual(generationBeforeShutdown + 1U, retiredGeneration,
					"Repeated Shutdown must retire the presentation runtime exactly once.");
				context.ExpectEqual(static_cast<std::size_t>(0U), presentationSystem.PublishedAnimatorCount(),
					"Shutdown must clear the render-facing plan before ownership is released.");
				context.Expect(presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F).empty(),
					"A post-shutdown scheduler query must not recreate presentation ownership from active authority membership.");
				context.ExpectEqual(retiredGeneration, presentationSystem.PresentationPublicationGeneration(),
					"Rejecting post-shutdown work must not synthesize another publication identity.");

				presentationSystem.Initialize();
				context.ExpectEqual(retiredGeneration, presentationSystem.PresentationPublicationGeneration(),
					"Initialize must preserve the retired generation before the replacement plan publishes.");
				const TerraEngine::Scheduling::FrameExecutionPlan retiredPlan =
					TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
						std::move(retiredPresentation), "animation-presentation-lifecycle-retired");
				context.Expect(retiredPlan.valid && retiredPlan.completed,
					"The scheduler may finish cancelled descriptors, but their callbacks must fail closed.");
				context.ExpectEqual(static_cast<std::size_t>(0U), presentationSystem.PublishedAnimatorCount(),
					"A descriptor captured before shutdown must not publish into the replacement runtime session.");
				context.ExpectEqual(retiredGeneration, presentationSystem.PresentationPublicationGeneration(),
					"Rejected stale descriptors must not synthesize a presentation publication generation.");
				std::vector<FrameWorkloadDeclaration> replacementPresentation =
					presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
				const TerraEngine::Scheduling::FrameExecutionPlan replacementPlan =
					TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
						std::move(replacementPresentation), "animation-presentation-lifecycle-replacement");
				context.Expect(replacementPlan.valid && replacementPlan.completed,
					"Explicit Initialize must allow a new complete presentation plan to publish.");
				context.Expect(presentationSystem.PresentationPublicationGeneration() > retiredGeneration,
					"The replacement plan must advance beyond the retired runtime identity.");
				const std::uint64_t replacementGeneration =
					presentationSystem.PresentationPublicationGeneration();
				presentationSystem.Initialize();
				context.ExpectEqual(replacementGeneration, presentationSystem.PresentationPublicationGeneration(),
					"Repeated Initialize while active must preserve the complete published identity.");
				context.ExpectEqual(static_cast<std::size_t>(2U), presentationSystem.PublishedAnimatorCount(),
					"Idempotent Initialize must not erase a complete live presentation plan.");
			}
			presentationSystem.Shutdown();
			// Restore the process-wide singleton for subsequent scheduler tests.
			presentationSystem.Initialize();
		});

		registry.Add("Animation.Scheduler.ActiveToDormantRetiresPublishedGenerations", [](TestContext& context) {
			TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
			TerraEngine::Animation::AnimationPresentationSystem& presentationSystem = TerraEngine::Animation::AnimationPresentationSystem::Instance();
			{
				AnimatorMembershipGuard membership(animationSystem, 8U);
				std::vector<FrameWorkloadDeclaration> authority = animationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
				const TerraEngine::Scheduling::FrameExecutionPlan authorityPlan =
					TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
						std::move(authority), "animation-active-authority");
				context.Expect(authorityPlan.valid && authorityPlan.completed,
					"The active authority shell must publish one complete generation before retirement is tested.");

				std::vector<FrameWorkloadDeclaration> presentation = presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
				const TerraEngine::Scheduling::FrameExecutionPlan presentationPlan =
					TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
						std::move(presentation), "animation-active-presentation");
				context.Expect(presentationPlan.valid && presentationPlan.completed,
					"The active presentation shell must publish one complete render-facing generation.");
				context.ExpectEqual(static_cast<std::size_t>(8U), animationSystem.PublishedAnimatorCount(),
					"The active authority publication must expose the complete membership count.");
				context.ExpectEqual(static_cast<std::size_t>(8U), presentationSystem.PublishedAnimatorCount(),
					"The active presentation publication must expose the complete membership count.");
			}

			const std::uint64_t authorityGeneration = animationSystem.AuthorityPublicationGeneration();
			const std::uint64_t presentationGeneration = presentationSystem.PresentationPublicationGeneration();
			TerraEngine::Animation::ClearPublishedAnimationDiagnostics();
			context.Expect(animationSystem.DeclareFrameWorkloads(1.0F / 60.0F).empty(),
				"Transitioning to zero active animators must allocate no authority descriptors.");
			context.Expect(presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F).empty(),
				"Transitioning to zero active animators must allocate no presentation descriptors.");
			context.ExpectEqual(static_cast<std::size_t>(0U), animationSystem.PublishedAnimatorCount(),
				"Dormancy must retire the prior authoritative animator count.");
			context.ExpectEqual(static_cast<std::size_t>(0U), presentationSystem.PublishedAnimatorCount(),
				"Dormancy must retire the prior presentation animator count.");
			context.ExpectEqual(authorityGeneration + 1U, animationSystem.AuthorityPublicationGeneration(),
				"Authority retirement must advance its generation so stale readers fail closed.");
			context.ExpectEqual(presentationGeneration + 1U, presentationSystem.PresentationPublicationGeneration(),
				"Presentation retirement must advance its generation so stale render plans fail closed.");
			context.Expect(!TerraEngine::Animation::AcquireAnimationDiagnosticsSnapshot().has_value(),
				"Static-only projects must not enter global animation diagnostic collection.");
		});

		registry.Add("Animation.Scheduler.DormantSystemsDeclareNoUsefulJobs", [](TestContext& context) {
			TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
			TerraEngine::Animation::AnimationPresentationSystem& presentationSystem = TerraEngine::Animation::AnimationPresentationSystem::Instance();
			std::vector<FrameWorkloadDeclaration> workloads = animationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
			std::vector<FrameWorkloadDeclaration> presentation = presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F);
			context.Expect(workloads.empty() && presentation.empty(),
				"A zero-animator frame must return before allocating string/vector-bearing workload descriptors.");
			const std::uint64_t authorityGeneration = animationSystem.AuthorityPublicationGeneration();
			const std::uint64_t presentationGeneration = presentationSystem.PresentationPublicationGeneration();
			for(std::size_t frame = 0U; frame < 128U; ++frame) {
				context.Expect(animationSystem.DeclareFrameWorkloads(1.0F / 60.0F).empty()
					&& presentationSystem.DeclareFrameWorkloads(1.0F / 60.0F).empty(),
					"Steady dormant declarations must remain empty without transition cleanup work.");
			}
			context.ExpectEqual(authorityGeneration, animationSystem.AuthorityPublicationGeneration(),
				"Steady dormancy must not repeatedly retire an already-empty authority publication.");
			context.ExpectEqual(presentationGeneration, presentationSystem.PresentationPublicationGeneration(),
				"Steady dormancy must not repeatedly retire an already-empty presentation publication.");

			const TerraEngine::Scheduling::FrameExecutionPlan plan = TerraEngine::Scheduling::ValidateFrameWorkloads(
				{}, "animation-dormant-contract");
			context.Expect(plan.valid, "Dormant animation descriptors must remain a valid visible scheduler contract.");
			context.Expect(plan.results.empty() && plan.workloads.empty(),
				"Dormant animation must create zero worker, inline, protected, or omitted frame jobs.");

			// Verify the scheduler honors the explicit zero-work contract instead of
			// wrapping SystemsBase::Update as a legacy protected workload.
			TerraEngine::Scheduling::SystemDeclaration animation = animationSystem.SchedulerDeclaration();
			animation.after.clear();
			animation.before.clear();
			animation.requiresFlushedSceneMutations = false;
			TerraEngine::Scheduling::SystemDeclaration presentationDeclaration = presentationSystem.SchedulerDeclaration();
			presentationDeclaration.after.clear();
			presentationDeclaration.before.clear();
			TerraEngine::Scheduling::SystemScheduler scheduler;
			scheduler.SetMutationFlushPoints({});
			scheduler.RegisterSystem(animationSystem, std::move(animation));
			scheduler.RegisterSystem(presentationSystem, std::move(presentationDeclaration));
			context.Expect(scheduler.BuildSchedule().valid, "The reduced dormant animation schedule must validate.");
			context.Expect(scheduler.BuildFrameWorkloadDeclarations(1.0F / 60.0F).empty(),
				"SystemScheduler must not synthesize compatibility Update jobs for an explicitly dormant converted system.");
		});
	}
}
