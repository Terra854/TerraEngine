// Disposable Jolt provider spike fixture.
//
// This program intentionally owns no TerraEngine runtime state. It demonstrates
// only public Jolt APIs and writes one JSON document to stdout so the backend
// harness can archive raw, machine-readable evidence. The conservative checks
// below distinguish a supported API from an engine-level guarantee: Jolt's
// callbacks are collected with atomics here, while TerraEngine still needs its
// own deferred publication and lifetime rules around them.

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

#ifdef JPH_DEBUG_RENDERER
	#include <Jolt/Renderer/DebugRenderer.h>
#endif

#include "Engine/ThreadPool/ThreadPool.h"
#include "Engine/Core/Logger/Logger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

JPH_SUPPRESS_WARNINGS

using namespace JPH;
using namespace JPH::literals;

namespace
{
	// The fixture uses two layers so filtering is exercised without relying on
	// Jolt's sample layer definitions.
	constexpr ObjectLayer kStaticLayer = 0;
	constexpr ObjectLayer kMovingLayer = 1;
	constexpr ObjectLayer kSensorLayer = 2;
	constexpr uint kLayerCount = 3;
	const BroadPhaseLayer kStaticBroadPhase(0);
	const BroadPhaseLayer kMovingBroadPhase(1);

	class ProbeBroadPhaseLayers final : public BroadPhaseLayerInterface
	{
	public:
		uint GetNumBroadPhaseLayers() const override { return 2; }
		BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer inLayer) const override
		{
			return inLayer == kStaticLayer? kStaticBroadPhase : kMovingBroadPhase;
		}

	#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
		const char *GetBroadPhaseLayerName(BroadPhaseLayer inLayer) const override
		{
			return inLayer == kStaticBroadPhase? "static" : "moving";
		}
	#endif
	};

	class ProbeObjectPairFilter final : public ObjectLayerPairFilter
	{
	public:
		bool ShouldCollide(ObjectLayer inFirst, ObjectLayer inSecond) const override
		{
			// Sensors collide for callback delivery but never generate a physical
			// response because their BodyCreationSettings mark them as sensors.
			return !(inFirst == kStaticLayer && inSecond == kStaticLayer);
		}
	};

	class ProbeObjectVsBroadPhaseFilter final : public ObjectVsBroadPhaseLayerFilter
	{
	public:
		bool ShouldCollide(ObjectLayer inLayer, BroadPhaseLayer inBroadPhase) const override
		{
			return inLayer != kStaticLayer || inBroadPhase == kMovingBroadPhase;
		}
	};

	struct CallbackCounters final : ContactListener, BodyActivationListener
	{
		std::atomic_uint32_t contactsAdded { 0 };
		std::atomic_uint32_t contactsPersisted { 0 };
		std::atomic_uint32_t contactsRemoved { 0 };
		std::atomic_uint32_t sensorContactsAdded { 0 };
		std::atomic_uint32_t activations { 0 };
		std::atomic_uint32_t deactivations { 0 };
		std::atomic_uint32_t lateCallbacks { 0 };
		std::atomic_bool teardownStarted { false };

		ValidateResult OnContactValidate(const Body &, const Body &, RVec3Arg, const CollideShapeResult &) override
		{
			if (teardownStarted.load(std::memory_order_relaxed))
				lateCallbacks.fetch_add(1, std::memory_order_relaxed);
			return ValidateResult::AcceptAllContactsForThisBodyPair;
		}

		void OnContactAdded(const Body &inFirst, const Body &inSecond, const ContactManifold &, ContactSettings &) override
		{
			contactsAdded.fetch_add(1, std::memory_order_relaxed);
			if (inFirst.IsSensor() || inSecond.IsSensor())
				sensorContactsAdded.fetch_add(1, std::memory_order_relaxed);
			if (teardownStarted.load(std::memory_order_relaxed))
				lateCallbacks.fetch_add(1, std::memory_order_relaxed);
		}

		void OnContactPersisted(const Body &, const Body &, const ContactManifold &, ContactSettings &) override
		{
			contactsPersisted.fetch_add(1, std::memory_order_relaxed);
			if (teardownStarted.load(std::memory_order_relaxed))
				lateCallbacks.fetch_add(1, std::memory_order_relaxed);
		}

		void OnContactRemoved(const SubShapeIDPair &) override
		{
			contactsRemoved.fetch_add(1, std::memory_order_relaxed);
			if (teardownStarted.load(std::memory_order_relaxed))
				lateCallbacks.fetch_add(1, std::memory_order_relaxed);
		}

		void OnBodyActivated(const BodyID &, uint64) override
		{
			activations.fetch_add(1, std::memory_order_relaxed);
		}

		void OnBodyDeactivated(const BodyID &, uint64) override
		{
			deactivations.fetch_add(1, std::memory_order_relaxed);
		}
	};

	// Jolt permits an application allocator before Jolt objects exist. The probe
	// forwards every operation to the platform allocator Jolt registered, while
	// retaining a pointer-to-size ledger so the reported peak is attributable to
	// the provider allocation path rather than a process-wide RSS estimate.
#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
	class ScopedJoltAllocationProbe final
	{
	public:
		ScopedJoltAllocationProbe()
		{
			RegisterDefaultAllocator();
			originalAllocate = Allocate;
			originalReallocate = Reallocate;
			originalFree = Free;
			originalAlignedAllocate = AlignedAllocate;
			originalAlignedFree = AlignedFree;
			instance = this;
			Allocate = &AllocateTracked;
			Reallocate = &ReallocateTracked;
			Free = &FreeTracked;
			AlignedAllocate = &AlignedAllocateTracked;
			AlignedFree = &AlignedFreeTracked;
		}

		~ScopedJoltAllocationProbe()
		{
			// The fixture destroys Factory, every world and the job adapter before
			// this RAII scope ends. Restoring here therefore cannot strand Jolt
			// allocations behind a callback into a dead metrics object.
			Allocate = originalAllocate;
			Reallocate = originalReallocate;
			Free = originalFree;
			AlignedAllocate = originalAlignedAllocate;
			AlignedFree = originalAlignedFree;
			instance = nullptr;
		}

		uint64_t GetAllocationCount() const { return allocations.load(); }
		uint64_t GetFreeCount() const { return frees.load(); }
		uint64_t GetCurrentBytes() const { return currentBytes.load(); }
		uint64_t GetPeakBytes() const { return peakBytes.load(); }
		static constexpr bool IsAvailable() { return true; }

	private:
		static void UpdatePeak(uint64_t inCurrent)
		{
			uint64_t observed = instance->peakBytes.load(std::memory_order_relaxed);
			while (observed < inCurrent && !instance->peakBytes.compare_exchange_weak(observed, inCurrent, std::memory_order_relaxed)) { }
		}

		void TrackAllocation(void *inPointer, size_t inBytes)
		{
			if (inPointer == nullptr)
				return;
			std::lock_guard<std::mutex> lock(ledgerMutex);
			ledger[inPointer] = inBytes;
			const uint64_t current = currentBytes.fetch_add(inBytes, std::memory_order_relaxed) + inBytes;
			allocations.fetch_add(1, std::memory_order_relaxed);
			UpdatePeak(current);
		}

		void TrackFree(void *inPointer)
		{
			if (inPointer == nullptr)
				return;
			std::lock_guard<std::mutex> lock(ledgerMutex);
			const auto found = ledger.find(inPointer);
			if (found == ledger.end())
				return;
			currentBytes.fetch_sub(found->second, std::memory_order_relaxed);
			ledger.erase(found);
			frees.fetch_add(1, std::memory_order_relaxed);
		}

		static void *AllocateTracked(size_t inBytes) { void *pointer = instance->originalAllocate(inBytes); instance->TrackAllocation(pointer, inBytes); return pointer; }
		static void *ReallocateTracked(void *inPointer, size_t inOldBytes, size_t inNewBytes)
		{
			// A failed non-zero realloc leaves the original allocation alive. Update
			// the ledger only after success so a low-memory result cannot fabricate
			// balanced counters or hide live provider memory.
			void *pointer = instance->originalReallocate(inPointer, inOldBytes, inNewBytes);
			if (pointer != nullptr || inNewBytes == 0)
			{
				instance->TrackFree(inPointer);
				instance->TrackAllocation(pointer, inNewBytes);
			}
			return pointer;
		}
		static void FreeTracked(void *inPointer) { instance->TrackFree(inPointer); instance->originalFree(inPointer); }
		static void *AlignedAllocateTracked(size_t inBytes, size_t inAlignment) { void *pointer = instance->originalAlignedAllocate(inBytes, inAlignment); instance->TrackAllocation(pointer, inBytes); return pointer; }
		static void AlignedFreeTracked(void *inPointer) { instance->TrackFree(inPointer); instance->originalAlignedFree(inPointer); }

		inline static ScopedJoltAllocationProbe *instance = nullptr;
		AllocateFunction originalAllocate = nullptr;
		ReallocateFunction originalReallocate = nullptr;
		FreeFunction originalFree = nullptr;
		AlignedAllocateFunction originalAlignedAllocate = nullptr;
		AlignedFreeFunction originalAlignedFree = nullptr;
		std::atomic_uint64_t allocations { 0 }, frees { 0 }, currentBytes { 0 }, peakBytes { 0 };
		std::mutex ledgerMutex;
		std::unordered_map<void *, size_t> ledger;
	};
#else
	// Jolt intentionally enables JPH_DISABLE_CUSTOM_ALLOCATOR for ReleaseASAN and
	// ReleaseTSAN so sanitizer runtimes see ordinary allocations directly. Keep
	// the fixture ABI stable and report that the hook is unavailable rather than
	// attempting to override the real allocation functions in those builds.
	class ScopedJoltAllocationProbe final
	{
	public:
		uint64_t GetAllocationCount() const { return 0; }
		uint64_t GetFreeCount() const { return 0; }
		uint64_t GetCurrentBytes() const { return 0; }
		uint64_t GetPeakBytes() const { return 0; }
		static constexpr bool IsAvailable() { return false; }
	};
#endif

	// Jolt deliberately supplies JobSystemWithBarrier so host engines can keep
	// their own worker lifetime and scheduling policy. This adapter owns Jolt Job
	// objects only; bounded work pumps run them on TerraEngine's singleton
	// ThreadPool, which is initialized once by main and shut down after all worlds.
	class SharedPoolJoltJobSystem final : public JobSystemWithBarrier
	{
	public:
		SharedPoolJoltJobSystem(ThreadPool &inSharedPool, uint inWorkerLimit) :
			JobSystemWithBarrier(cMaxPhysicsBarriers),
			sharedPool(inSharedPool),
			workerLimit(std::max(1u, inWorkerLimit))
		{
		}

		int GetMaxConcurrency() const override
		{
			// The caller thread can participate in Jolt barrier work, so Jolt sees
			// one more execution lane than this adapter submits to the shared pool.
			// Cap the adapter's work pumps, not ThreadPool itself: other engine
			// clients keep their normal worker access while Jolt gets an honest limit.
			const uint availableWorkers = static_cast<uint>(std::max<size_t>(1, sharedPool.getThreadCount()));
			return static_cast<int>(std::min(workerLimit, availableWorkers) + 1u);
		}

		JobHandle CreateJob(const char *inName, ColorArg inColor, const JobFunction &inFunction, uint32 inDependencies = 0) override
		{
			Job *job = new Job(inName, inColor, this, inFunction, inDependencies);
			JobHandle handle(job);
			if (inDependencies == 0)
				QueueJob(job);
			return handle;
		}

		void WaitForJobs(Barrier *inBarrier) override
		{
			// JobSystemWithBarrier may finish a Jolt job on its caller thread while
			// a queued shared-pool pump still owns a reference. Jolt requires an
			// engine-backed adapter to drain that queue *inside* this boundary,
			// before callers can destroy the barrier or start another physics stage.
			const auto waitStart = std::chrono::steady_clock::now();
			JobSystemWithBarrier::WaitForJobs(inBarrier);
			WaitForSharedPoolIdle();
			const uint64_t waitMicroseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - waitStart).count());
			barrierWaitMicroseconds.fetch_add(waitMicroseconds, std::memory_order_relaxed);
			uint64_t observed = maxBarrierWaitMicroseconds.load(std::memory_order_relaxed);
			while (observed < waitMicroseconds && !maxBarrierWaitMicroseconds.compare_exchange_weak(observed, waitMicroseconds, std::memory_order_relaxed)) { }
		}

		void WaitForSharedPoolIdle()
		{
			const uint64_t target = jobsQueued.load(std::memory_order_acquire);
			std::unique_lock<std::mutex> lock(dispatchMutex);
			idleCondition.wait(lock, [this, target] {
				return jobsCompleted.load(std::memory_order_acquire) >= target && activePumpTasks == 0;
			});
		}

		uint64_t GetJobsQueued() const { return jobsQueued.load(std::memory_order_acquire); }
		uint64_t GetJobsCompleted() const { return jobsCompleted.load(std::memory_order_acquire); }
		uint GetSharedPoolWorkers() const { return static_cast<uint>(sharedPool.getThreadCount()); }
		uint GetRequestedWorkerLimit() const { return workerLimit; }
		uint GetMaxSimultaneousPumpTasks() const { return maxSimultaneousPumpTasks.load(std::memory_order_relaxed); }
		uint GetObservedWorkerThreadCount() const { std::lock_guard<std::mutex> lock(workerIdentityMutex); return static_cast<uint>(workerThreadIds.size()); }
		uint64_t GetBarrierWaitMicroseconds() const { return barrierWaitMicroseconds.load(std::memory_order_relaxed); }
		uint64_t GetMaxBarrierWaitMicroseconds() const { return maxBarrierWaitMicroseconds.load(std::memory_order_relaxed); }
		uint64_t GetJobExecutionMicroseconds() const { return jobExecutionMicroseconds.load(std::memory_order_relaxed); }

	protected:
		void QueueJob(Job *inJob) override
		{
			QueueOne(inJob);
		}

		void QueueJobs(Job **inJobs, uint inJobCount) override
		{
			for (uint index = 0; index < inJobCount; ++index)
				QueueOne(inJobs[index]);
		}

		void FreeJob(Job *inJob) override
		{
			delete inJob;
		}

	private:
		void QueueOne(Job *inJob)
		{
			// The adapter queue owns one reference until a shared-pool pump consumes
			// it. A barrier waiter may execute the job first; in that case the pump's
			// Execute call becomes a no-op and it only releases this owned reference.
			inJob->AddRef();
			jobsQueued.fetch_add(1, std::memory_order_release);

			bool needsPump = false;
			{
				std::lock_guard<std::mutex> lock(dispatchMutex);
				pendingJobs.push_back(inJob);
				if (activePumpTasks < GetPumpLimit())
				{
					++activePumpTasks;
					const uint active = activePumpTasks;
					uint observed = maxSimultaneousPumpTasks.load(std::memory_order_relaxed);
					while (observed < active && !maxSimultaneousPumpTasks.compare_exchange_weak(observed, active, std::memory_order_relaxed)) { }
					needsPump = true;
				}
			}

			if (needsPump)
				SubmitPump();
		}

		uint GetPumpLimit() const
		{
			return std::min(workerLimit, static_cast<uint>(std::max<size_t>(1, sharedPool.getThreadCount())));
		}

		void SubmitPump()
		{
			// One pump drains many Jolt jobs. This keeps the provider work bounded
			// and leaves the shared engine scheduler available to other clients.
			auto task = sharedPool.createTask([this]() -> uint8_t {
				PumpJobs();
				return 0;
			});
			sharedPool.enqueue(task);
		}

		void PumpJobs()
		{
			{
				std::lock_guard<std::mutex> lock(workerIdentityMutex);
				workerThreadIds.insert(std::this_thread::get_id());
			}
			for (;;)
			{
				Job *job = nullptr;
				{
					std::lock_guard<std::mutex> lock(dispatchMutex);
					if (pendingJobs.empty())
					{
						--activePumpTasks;
						idleCondition.notify_all();
						return;
					}
					job = pendingJobs.front();
					pendingJobs.pop_front();
				}

				const auto executionStart = std::chrono::steady_clock::now();
				job->Execute();
				jobExecutionMicroseconds.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - executionStart).count()), std::memory_order_relaxed);
				job->Release();
				jobsCompleted.fetch_add(1, std::memory_order_release);
				idleCondition.notify_all();
			}
		}

		ThreadPool &sharedPool;
		uint workerLimit;
		std::atomic_uint64_t jobsQueued { 0 };
		std::atomic_uint64_t jobsCompleted { 0 };
		std::atomic_uint maxSimultaneousPumpTasks { 0 };
		std::atomic_uint64_t barrierWaitMicroseconds { 0 };
		std::atomic_uint64_t maxBarrierWaitMicroseconds { 0 };
		std::atomic_uint64_t jobExecutionMicroseconds { 0 };
		std::deque<Job *> pendingJobs;
		uint activePumpTasks = 0;
		std::mutex dispatchMutex;
		std::condition_variable idleCondition;
		mutable std::mutex workerIdentityMutex;
		std::unordered_set<std::thread::id> workerThreadIds;
	};

#ifdef JPH_DEBUG_RENDERER
	struct DebugDrawResult
	{
		uint32_t lineCallbacks = 0;
		uint32_t triangleCallbacks = 0;
		uint32_t geometryCallbacks = 0;
	};

	// This minimal renderer is deliberately an extraction sink, not a rendering
	// backend. Passing it to PhysicsSystem::DrawBodies proves Jolt's real public
	// debug draw path invokes the same callback categories TerraEngine will adapt.
	class ProbeDebugRenderer final : public DebugRenderer
	{
	public:
		ProbeDebugRenderer() { Initialize(); }

		void DrawLine(RVec3Arg, RVec3Arg, ColorArg) override { ++result.lineCallbacks; }
		void DrawTriangle(RVec3Arg, RVec3Arg, RVec3Arg, ColorArg, ECastShadow) override { ++result.triangleCallbacks; }
		Batch CreateTriangleBatch(const Triangle *, int inTriangleCount) override { return new ProbeBatch(inTriangleCount); }
		Batch CreateTriangleBatch(const Vertex *, int inVertexCount, const uint32 *, int inIndexCount) override { return new ProbeBatch(inVertexCount + inIndexCount); }
		void DrawGeometry(RMat44Arg, const AABox &, float, ColorArg, const GeometryRef &, ECullMode, ECastShadow, EDrawMode) override { ++result.geometryCallbacks; }
		void DrawText3D(RVec3Arg, const string_view &, ColorArg, float) override { }

		DebugDrawResult result;

	private:
		class ProbeBatch final : public RefTarget<ProbeBatch>, public RefTargetVirtual
		{
		public:
			explicit ProbeBatch([[maybe_unused]] int inPrimitiveCount) { }
			void AddRef() override { RefTarget<ProbeBatch>::AddRef(); }
			void Release() override { RefTarget<ProbeBatch>::Release(); }
		};
	};
#endif

	// This owns all interfaces that PhysicsSystem borrows, preventing a common
	// beginner lifetime error where a layer filter outlives neither the world nor
	// a query that still references it.
	struct ProbeWorld
	{
		ProbeBroadPhaseLayers broadPhaseLayers;
		ProbeObjectVsBroadPhaseFilter objectVsBroadPhase;
		ProbeObjectPairFilter objectPairs;
		CallbackCounters callbacks;
		PhysicsSystem physics;
		TempAllocatorImpl temporaryMemory { 16 * 1024 * 1024 };
		SharedPoolJoltJobSystem &jobs;

		explicit ProbeWorld(SharedPoolJoltJobSystem &inJobs) : jobs(inJobs)
		{
			physics.Init(16384, 0, 16384, 16384, broadPhaseLayers, objectVsBroadPhase, objectPairs);
			physics.SetContactListener(&callbacks);
			physics.SetBodyActivationListener(&callbacks);
		}

		void Step(uint inSteps = 1)
		{
			for (uint step = 0; step < inSteps; ++step)
			{
				physics.Update(1.0f / 60.0f, 1, &temporaryMemory, &jobs);
			}
		}
	};

	uint64_t HashBytes(uint64_t inHash, const void *inData, size_t inBytes)
	{
		const auto *bytes = static_cast<const uint8_t *>(inData);
		for (size_t index = 0; index < inBytes; ++index)
		{
			inHash ^= bytes[index];
			inHash *= 1099511628211ull;
		}
		return inHash;
	}

	uint64_t HashValue(uint64_t inHash, uint64_t inValue)
	{
		return HashBytes(inHash, &inValue, sizeof(inValue));
	}

	std::string Hex(uint64_t inValue)
	{
		std::ostringstream result;
		result << std::hex << std::setfill('0') << std::setw(16) << inValue;
		return result.str();
	}

	double Percentile(std::vector<double> inSamples, double inPercentile)
	{
		if (inSamples.empty())
			return 0.0;
		std::sort(inSamples.begin(), inSamples.end());
		const size_t index = static_cast<size_t>(std::ceil(inPercentile * inSamples.size())) - 1;
		return inSamples[std::min(index, inSamples.size() - 1)];
	}

	BodyID AddBox(ProbeWorld &ioWorld, RVec3Arg inPosition, EMotionType inMotion, ObjectLayer inLayer, Vec3Arg inHalfExtent, bool inSensor = false)
	{
		BodyCreationSettings settings(new BoxShape(inHalfExtent), inPosition, Quat::sIdentity(), inMotion, inLayer);
		settings.mIsSensor = inSensor;
		settings.mFriction = 0.65f;
		settings.mRestitution = 0.35f;
		return ioWorld.physics.GetBodyInterface().CreateAndAddBody(settings, inMotion == EMotionType::Dynamic? EActivation::Activate : EActivation::DontActivate);
	}

	BodyID AddSphere(ProbeWorld &ioWorld, RVec3Arg inPosition, EMotionType inMotion, ObjectLayer inLayer, float inRadius, bool inLinearCast = false)
	{
		BodyCreationSettings settings(new SphereShape(inRadius), inPosition, Quat::sIdentity(), inMotion, inLayer);
		settings.mMotionQuality = inLinearCast? EMotionQuality::LinearCast : EMotionQuality::Discrete;
		settings.mFriction = 0.65f;
		settings.mRestitution = 0.35f;
		return ioWorld.physics.GetBodyInterface().CreateAndAddBody(settings, inMotion == EMotionType::Dynamic? EActivation::Activate : EActivation::DontActivate);
	}

	struct CoreResult
	{
		bool shapes = false;
		bool ccd = false;
		bool filtersAndMaterials = false;
		bool ray = false;
		bool shapeCast = false;
		bool constraint = false;
		bool character = false;
		bool events = false;
		bool debugDraw = false;
		uint32_t callbackCount = 0;
		uint32_t sensorCallbacks = 0;
		uint64_t sortedPacketHash = 0;
		uint32_t debugLineCallbacks = 0;
		uint32_t debugTriangleCallbacks = 0;
		uint32_t debugGeometryCallbacks = 0;
	};

	struct StateResult
	{
		bool restoreSucceeded = false;
		bool replaySaveEqual = false;
		uint64_t backendStateHash = 0;
		uint64_t backendStateBytes = 0;
		std::string firstDivergence = "none";
	};

	CoreResult RunCore(SharedPoolJoltJobSystem &inJobs)
	{
		ProbeWorld world(inJobs);
		CoreResult result;
		BodyInterface &bodies = world.physics.GetBodyInterface();

		// Direct construction demonstrates four primitive public shape classes.
		const RefConst<Shape> capsule = new CapsuleShape(0.75f, 0.3f);
		const RefConst<Shape> cylinder = new CylinderShape(0.75f, 0.4f);
		const RefConst<Shape> sphere = new SphereShape(0.5f);
		const RefConst<Shape> box = new BoxShape(Vec3(0.5f, 0.5f, 0.5f));
		result.shapes = capsule != nullptr && cylinder != nullptr && sphere != nullptr && box != nullptr;

		const BodyID floor = AddBox(world, RVec3(0.0_r, -0.5_r, 0.0_r), EMotionType::Static, kStaticLayer, Vec3(20.0f, 0.5f, 20.0f));
		const BodyID wall = AddBox(world, RVec3(0.0_r, 1.0_r, -2.0_r), EMotionType::Static, kStaticLayer, Vec3(3.0f, 2.0f, 0.1f));
		const BodyID sensor = AddBox(world, RVec3(0.0_r, 1.0_r, 0.0_r), EMotionType::Static, kSensorLayer, Vec3(2.0f, 0.25f, 2.0f), true);
		const BodyID falling = AddSphere(world, RVec3(0.0_r, 3.0_r, 0.0_r), EMotionType::Dynamic, kMovingLayer, 0.4f);
		const BodyID fast = AddSphere(world, RVec3(0.0_r, 1.0_r, 5.0_r), EMotionType::Dynamic, kMovingLayer, 0.2f, true);
		bodies.SetLinearVelocity(fast, Vec3(0.0f, 0.0f, -240.0f));

		// A Jolt sensor still emits contact callbacks but does not resolve contacts.
		// The fixture reports this as API support, not as a complete engine trigger
		// event contract (which must add stable identity and ordering separately).
		world.Step(120);
		const float fastZ = bodies.GetCenterOfMassPosition(fast).GetZ();
		result.ccd = fastZ > -2.5f;
		result.filtersAndMaterials = !floor.IsInvalid() && !sensor.IsInvalid();

		RayCastResult rayHit;
		const RRayCast ray(RVec3(0.0_r, 8.0_r, 0.0_r), Vec3(0.0f, -16.0f, 0.0f));
		result.ray = world.physics.GetNarrowPhaseQuery().CastRay(ray, rayHit);
		RShapeCast shapeCast(sphere, Vec3::sOne(), RMat44::sTranslation(RVec3(0.0_r, 4.0_r, 1.0_r)), Vec3(0.0f, -8.0f, 0.0f));
		ClosestHitCollisionCollector<CastShapeCollector> castCollector;
		world.physics.GetNarrowPhaseQuery().CastShape(shapeCast, ShapeCastSettings(), RVec3::sZero(), castCollector);
		result.shapeCast = castCollector.HadHit();

		// Creating a public distance constraint through BodyInterface avoids locking
		// internals and provides the narrow constraint capability gate requested by constraint-capability.
		const BodyID constraintA = AddSphere(world, RVec3(-2.0_r, 4.0_r, 0.0_r), EMotionType::Dynamic, kMovingLayer, 0.2f);
		const BodyID constraintB = AddSphere(world, RVec3(-2.0_r, 5.0_r, 0.0_r), EMotionType::Dynamic, kMovingLayer, 0.2f);
		DistanceConstraintSettings distance;
		distance.mPoint1 = RVec3(-2.0_r, 4.0_r, 0.0_r);
		distance.mPoint2 = RVec3(-2.0_r, 5.0_r, 0.0_r);
		distance.mMinDistance = 0.75f;
		distance.mMaxDistance = 1.25f;
		Ref<Constraint> constraint = bodies.CreateConstraint(&distance, constraintA, constraintB);
		if (constraint != nullptr)
		{
			world.physics.AddConstraint(constraint);
			world.Step(4);
			result.constraint = true;
			world.physics.RemoveConstraint(constraint);
		}

		// CharacterVirtual has no private Jolt thread pool: caller-owned JobSystem
		// and TempAllocator are passed into its update explicitly.
		CharacterVirtualSettings characterSettings;
		characterSettings.mShape = new CapsuleShape(0.75f, 0.3f);
		characterSettings.mSupportingVolume = Plane(Vec3::sAxisY(), -0.3f);
		Ref<CharacterVirtual> character = new CharacterVirtual(&characterSettings, RVec3(1.5_r, 2.0_r, 0.0_r), Quat::sIdentity(), 0, &world.physics);
		character->SetLinearVelocity(Vec3(0.0f, -2.0f, 0.0f));
		BroadPhaseLayerFilter allBroadPhase;
		ObjectLayerFilter allObjects;
		BodyFilter allBodies;
		ShapeFilter allShapes;
		character->Update(1.0f / 60.0f, Vec3(0.0f, -9.81f, 0.0f), allBroadPhase, allObjects, allBodies, allShapes, world.temporaryMemory);
		result.character = character->GetID() != CharacterID();

		// This calls Jolt's actual public DebugRenderer path. The renderer only
		// counts callbacks, avoiding an accidental claim that this disposable probe
		// already owns TerraEngine's editor-facing debug mesh lifetime.
#ifdef JPH_DEBUG_RENDERER
		ProbeDebugRenderer renderer;
		BodyManager::DrawSettings drawSettings;
		world.physics.DrawBodies(drawSettings, &renderer);
		result.debugLineCallbacks = renderer.result.lineCallbacks;
		result.debugTriangleCallbacks = renderer.result.triangleCallbacks;
		result.debugGeometryCallbacks = renderer.result.geometryCallbacks;
		result.debugDraw = result.debugLineCallbacks + result.debugTriangleCallbacks + result.debugGeometryCallbacks > 0;
#endif

		result.callbackCount = world.callbacks.contactsAdded.load() + world.callbacks.contactsPersisted.load() + world.callbacks.contactsRemoved.load();
		result.sensorCallbacks = world.callbacks.sensorContactsAdded.load();
		result.events = result.callbackCount > 0 && result.sensorCallbacks > 0;

		// Sort engine-owned packet keys; never hash raw provider pointers or callback
		// arrival order, which are intentionally not a portable contract.
		std::array<uint64_t, 7> packets {
			static_cast<uint64_t>(floor.GetIndexAndSequenceNumber()), static_cast<uint64_t>(wall.GetIndexAndSequenceNumber()),
			static_cast<uint64_t>(sensor.GetIndexAndSequenceNumber()), static_cast<uint64_t>(falling.GetIndexAndSequenceNumber()),
			static_cast<uint64_t>(fast.GetIndexAndSequenceNumber()), static_cast<uint64_t>(result.callbackCount),
			static_cast<uint64_t>(rayHit.mFraction * 1000000.0f)
		};
		std::sort(packets.begin(), packets.end());
		uint64_t hash = 1469598103934665603ull;
		for (uint64_t packet : packets)
			hash = HashValue(hash, packet);
		result.sortedPacketHash = hash;
		return result;
	}

	StateResult RunStateReplayProbe(SharedPoolJoltJobSystem &inJobs)
	{
		ProbeWorld world(inJobs);
		AddBox(world, RVec3(0.0_r, -0.5_r, 0.0_r), EMotionType::Static, kStaticLayer, Vec3(4.0f, 0.5f, 4.0f));
		AddSphere(world, RVec3(0.0_r, 2.0_r, 0.0_r), EMotionType::Dynamic, kMovingLayer, 0.5f);
		world.Step(4);

		StateRecorderImpl saved;
		world.physics.SaveState(saved);
		const std::string bytes = saved.GetData();
		StateResult result;
		result.backendStateBytes = bytes.size();
		result.backendStateHash = HashBytes(1469598103934665603ull, bytes.data(), bytes.size());
		saved.Rewind();
		result.restoreSucceeded = world.physics.RestoreState(saved);
		StateRecorderImpl replayed;
		world.physics.SaveState(replayed);
		result.replaySaveEqual = result.restoreSucceeded && saved.IsEqual(replayed);
		if (!result.restoreSucceeded)
			result.firstDivergence = "RestoreState returned false";
		else if (!result.replaySaveEqual)
			result.firstDivergence = "StateRecorderImpl byte stream differs after restore/save";
		return result;
	}

	struct LifecycleResult
	{
		uint32_t iterations = 0;
		uint32_t lateCallbacks = 0;
		uint32_t cancelledJobsSkipped = 0;
		uint32_t cancellationDrainRounds = 0;
		bool shutdownRestartSupported = false;
	};

	LifecycleResult RunLifecycle(SharedPoolJoltJobSystem &inJobs)
	{
		LifecycleResult result;
		for (uint32_t iteration = 0; iteration < 100; ++iteration)
		{
			ProbeWorld world(inJobs);
			AddBox(world, RVec3(0.0_r, -0.5_r, 0.0_r), EMotionType::Static, kStaticLayer, Vec3(4.0f, 0.5f, 4.0f));
			AddSphere(world, RVec3(0.0_r, 1.0_r, 0.0_r), EMotionType::Dynamic, kMovingLayer, 0.5f);
			world.Step(2);
			// Update returns only after Jolt's submitted jobs finish. We then flip the
			// detector before destruction, making post-boundary callbacks visible.
			world.callbacks.teardownStarted.store(true, std::memory_order_release);
			result.lateCallbacks += world.callbacks.lateCallbacks.load(std::memory_order_acquire);
			++result.iterations;
		}
		// A producer stop flag is checked inside work submitted through the exact
		// shared-pool adapter. Waiting for the adapter queue before the next world
		// proves the stop/drain boundary without inventing a Jolt cancellation API.
		for (uint32_t round = 0; round < 4; ++round)
		{
			std::atomic_bool stopRequested { true };
			std::atomic_uint32_t executed { 0 };
			JobSystem::JobHandle cancellationJob = inJobs.CreateJob("ProviderProbeStop", Color::sRed, [&stopRequested, &executed] {
				if (!stopRequested.load(std::memory_order_acquire))
					executed.fetch_add(1, std::memory_order_relaxed);
			});
			inJobs.WaitForSharedPoolIdle();
			result.cancelledJobsSkipped += executed.load(std::memory_order_acquire) == 0? 1u : 0u;
			++result.cancellationDrainRounds;
		}
		result.shutdownRestartSupported = result.iterations == 100 && result.cancelledJobsSkipped == result.cancellationDrainRounds;
		return result;
	}

	constexpr float kMaximumCastDistance = 100000.0f;
	constexpr float kMaximumShapeExtent = 1000.0f;
	constexpr uint32_t kMaximumHitsPerQuery = 64;
	constexpr uint32_t kMaximumRequestsPerBatch = 10000;
	constexpr uint32_t kRetainedResultTicks = 2;

	struct QueryResult
	{
		uint32_t requests = 0, hits = 0, rejectedNonFinite = 0, rejectedOversizeDistance = 0, rejectedOversizeExtent = 0;
		uint32_t denseHitCount = 0, denseHitsRetained = 0, repeatOrderingRuns = 0, batchRepeatOrderingRuns = 0;
		bool acceptedMaximumExtent = false, maximumExtentBroadPhaseHit = false;
		bool denseTruncated = false, repeatOrderingEqual = false, batchRepeatOrderingEqual = false;
		uint64_t denseOrderingHash = 0, repeatedOrderingHash = 0, batchOrderingHash = 0, peakRetainedBytes = 0;
		double p50 = 0.0, p95 = 0.0, p99 = 0.0;
	};

	bool IsAdapterRayAccepted(float inDistance)
	{
		return std::isfinite(inDistance) && inDistance >= 0.0f && inDistance <= kMaximumCastDistance;
	}

	QueryResult RunQueries(SharedPoolJoltJobSystem &inJobs)
	{
		ProbeWorld world(inJobs);
		AddBox(world, RVec3(0.0_r, -0.5_r, 0.0_r), EMotionType::Static, kStaticLayer, Vec3(100.0f, 0.5f, 100.0f));
		std::vector<double> samples;
		samples.reserve(10000);
		QueryResult result;
		// large-batch-determinism requires the complete 10,000-request batch to retain identical
		// provider-neutral ordering over 100 repetitions, not merely one dense ray.
		// Time the first batch for query percentiles and hash all 100 batches.
		result.batchRepeatOrderingEqual = true;
		for (uint32_t run = 0; run < 100; ++run)
		{
			uint64_t batchHash = 1469598103934665603ull;
			for (uint32_t request = 0; request < kMaximumRequestsPerBatch; ++request)
			{
				const auto start = std::chrono::steady_clock::now();
				RayCastResult hit;
				const bool hadHit = world.physics.GetNarrowPhaseQuery().CastRay(RRayCast(RVec3(float(request % 100) - 50.0f, 10.0_r, float(request / 100) - 50.0f), Vec3(0.0f, -20.0f, 0.0f)), hit);
				const auto stop = std::chrono::steady_clock::now();
				if (run == 0)
				{
					samples.push_back(std::chrono::duration<double, std::micro>(stop - start).count());
					result.hits += hadHit? 1u : 0u;
				}
				batchHash = HashValue(batchHash, hadHit? 1u : 0u);
				if (hadHit)
				{
					batchHash = HashValue(batchHash, hit.mBodyID.GetIndexAndSequenceNumber());
					batchHash = HashValue(batchHash, static_cast<uint64_t>(hit.mFraction * 1000000000.0f));
				}
			}
			if (run == 0)
				result.batchOrderingHash = batchHash;
			else
				result.batchRepeatOrderingEqual = result.batchRepeatOrderingEqual && batchHash == result.batchOrderingHash;
			++result.batchRepeatOrderingRuns;
		}
		result.requests = static_cast<uint32_t>(samples.size());
		result.p50 = Percentile(samples, 0.50);
		result.p95 = Percentile(samples, 0.95);
		result.p99 = Percentile(samples, 0.99);

		// The engine-facing adapter validates before allocating Jolt collectors.
		// These values are intentionally explicit candidate policy, not a claim that
		// raw Jolt itself rejects a ray of this length or extent.
		result.rejectedNonFinite = IsAdapterRayAccepted(std::numeric_limits<float>::infinity())? 0u : 1u;
		result.rejectedOversizeDistance = IsAdapterRayAccepted(kMaximumCastDistance + 1.0f)? 0u : 1u;
		result.rejectedOversizeExtent = std::isfinite(kMaximumShapeExtent + 1.0f) && kMaximumShapeExtent + 1.0f > kMaximumShapeExtent? 1u : 0u;
		const BodyID maximumExtentBody = AddBox(world, RVec3(0.0_r, -2500.0_r, 0.0_r), EMotionType::Static, kStaticLayer, Vec3(kMaximumShapeExtent, kMaximumShapeExtent, kMaximumShapeExtent));
		result.acceptedMaximumExtent = !maximumExtentBody.IsInvalid();
		RayCastResult maximumExtentHit;
		result.maximumExtentBroadPhaseHit = world.physics.GetNarrowPhaseQuery().CastRay(RRayCast(RVec3(0.0_r, -1400.0_r, 0.0_r), Vec3(0.0f, -2200.0f, 0.0f)), maximumExtentHit) && maximumExtentHit.mBodyID == maximumExtentBody;

		// Build a dense, deterministic ray path. Sorting with body ID as the tie
		// breaker makes truncation stable even when a provider reports equal fractions.
		for (uint32_t index = 0; index < 96; ++index)
			AddBox(world, RVec3(float(index) * 1.5f, 1.0_r, 0.0_r), EMotionType::Static, kStaticLayer, Vec3(0.4f, 0.4f, 0.4f));
		auto CollectDenseHash = [&world, &result]() {
			AllHitCollisionCollector<CastRayCollector> collector;
			world.physics.GetNarrowPhaseQuery().CastRay(RRayCast(RVec3(-2.0_r, 1.0_r, 0.0_r), Vec3(kMaximumCastDistance, 0.0f, 0.0f)), RayCastSettings(), collector);
			std::vector<RayCastResult> hits(collector.mHits.begin(), collector.mHits.end());
			std::sort(hits.begin(), hits.end(), [](const RayCastResult &left, const RayCastResult &right) {
				if (left.mFraction != right.mFraction) return left.mFraction < right.mFraction;
				return left.mBodyID.GetIndexAndSequenceNumber() < right.mBodyID.GetIndexAndSequenceNumber();
			});
			result.denseHitCount = static_cast<uint32_t>(hits.size());
			result.denseHitsRetained = std::min<uint32_t>(static_cast<uint32_t>(hits.size()), kMaximumHitsPerQuery);
			uint64_t hash = 1469598103934665603ull;
			for (uint32_t index = 0; index < result.denseHitsRetained; ++index)
			{
				hash = HashValue(hash, hits[index].mBodyID.GetIndexAndSequenceNumber());
				hash = HashValue(hash, static_cast<uint64_t>(hits[index].mFraction * 1000000000.0f));
			}
			return hash;
		};
		result.denseOrderingHash = CollectDenseHash();
		result.denseTruncated = result.denseHitCount > kMaximumHitsPerQuery && result.denseHitsRetained == kMaximumHitsPerQuery;
		result.repeatOrderingEqual = true;
		for (uint32_t run = 0; run < 100; ++run)
		{
			const uint64_t hash = CollectDenseHash();
			result.repeatOrderingEqual = result.repeatOrderingEqual && hash == result.denseOrderingHash;
			result.repeatedOrderingHash = hash;
			++result.repeatOrderingRuns;
		}
		// Retaining two frames is an adapter-owned result cache policy. The raw
		// provider collectors above are destroyed per request, so this is a bounded
		// worst-case payload estimate rather than an unverifiable hidden allocation.
		result.peakRetainedBytes = uint64_t(kRetainedResultTicks) * kMaximumRequestsPerBatch * kMaximumHitsPerQuery * sizeof(RayCastResult);
		return result;
	}

	struct LargeWorldResult
	{
		double originCoordinate = 0.25, farCoordinate = 10000000.25;
		double originAbsoluteError = 0.0, farAbsoluteError = 0.0;
		float originHitFraction = 0.0f, farHitFraction = 0.0f;
		bool originHit = false, farHit = false, precisionAssertionPassed = false;
	};

	LargeWorldResult RunLargeWorldProbe(SharedPoolJoltJobSystem &inJobs)
	{
		ProbeWorld world(inJobs);
		LargeWorldResult result;
		const BodyID nearBody = AddBox(world, RVec3(0.25_r, 0.0_r, 0.0_r), EMotionType::Static, kStaticLayer, Vec3(0.5f, 0.5f, 0.5f));
		const BodyID farBody = AddBox(world, RVec3(10000000.25_r, 0.0_r, -10000000.25_r), EMotionType::Static, kStaticLayer, Vec3(0.5f, 0.5f, 0.5f));
		BodyInterface &bodies = world.physics.GetBodyInterface();
		result.originAbsoluteError = std::abs(double(bodies.GetCenterOfMassPosition(nearBody).GetX()) - result.originCoordinate);
		result.farAbsoluteError = std::abs(double(bodies.GetCenterOfMassPosition(farBody).GetX()) - result.farCoordinate);
		RayCastResult originHit, farHit;
		result.originHit = world.physics.GetNarrowPhaseQuery().CastRay(RRayCast(RVec3(0.25_r, 10.0_r, 0.0_r), Vec3(0.0f, -20.0f, 0.0f)), originHit);
		result.farHit = world.physics.GetNarrowPhaseQuery().CastRay(RRayCast(RVec3(10000000.25_r, 10.0_r, -10000000.25_r), Vec3(0.0f, -20.0f, 0.0f)), farHit);
		result.originHitFraction = originHit.mFraction;
		result.farHitFraction = farHit.mFraction;
#ifdef JPH_DOUBLE_PRECISION
		result.precisionAssertionPassed = result.originAbsoluteError <= 0.000001 && result.farAbsoluteError <= 0.000001 && result.originHit && result.farHit;
#else
		// Float positions have metre-scale ULPs near ten million metres. The
		// float acceptance threshold demonstrates a bounded documented envelope
		// without pretending that the double build has the same quantization.
		result.precisionAssertionPassed = result.originAbsoluteError <= 0.000001 && result.farAbsoluteError <= 1.0 && result.originHit && result.farHit;
#endif
		return result;
	}

	QueryResult RunBodyPerformance(SharedPoolJoltJobSystem &inJobs)
	{
		ProbeWorld world(inJobs);
		std::vector<BodyID> ids;
		ids.reserve(10000);
		for (uint32_t index = 0; index < 10000; ++index)
		{
			const float x = float(index % 100) - 50.0f;
			const float z = float(index / 100) - 50.0f;
			ids.push_back(AddBox(world, RVec3(x, 0.0_r, z), index < 1000? EMotionType::Dynamic : EMotionType::Static, index < 1000? kMovingLayer : kStaticLayer, Vec3(0.2f, 0.2f, 0.2f)));
		}
		// Let activation, broad-phase insertion, and first-use caches settle before
		// timing. The measured window is long enough that one scheduler wake-up or
		// OS interruption cannot dominate a percentile comparison between versions.
		constexpr uint32_t kWarmupSteps = 30;
		constexpr uint32_t kMeasuredSteps = 120;
		world.Step(kWarmupSteps);
		std::vector<double> samples;
		samples.reserve(kMeasuredSteps);
		for (uint32_t step = 0; step < kMeasuredSteps; ++step)
		{
			const auto start = std::chrono::steady_clock::now();
			world.Step();
			const auto stop = std::chrono::steady_clock::now();
			samples.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
		}
		QueryResult result;
		// These public counters make the requested body cardinality explicit without
		// pretending Jolt exposes a whole-world allocation high-water mark.
		result.requests = world.physics.GetNumBodies();
		result.hits = world.physics.GetNumActiveBodies(EBodyType::RigidBody);
		result.p50 = Percentile(samples, 0.50);
		result.p95 = Percentile(samples, 0.95);
		result.p99 = Percentile(samples, 0.99);
		return result;
	}

	void PrintBool(std::ostream &ioOutput, bool inValue) { ioOutput << (inValue? "true" : "false"); }
}

int main(int argc, char **argv)
{
	uint workers = 1;
	for (int argument = 1; argument < argc; ++argument)
	{
		const std::string value = argv[argument];
		if (value.rfind("--workers=", 0) == 0)
			workers = std::max(1u, static_cast<uint>(std::stoul(value.substr(10))));
	}

	// The shared pool's worker lifecycle begins once for this fixture process.
	// Lowering the logger threshold prevents its normal worker lifecycle messages
	// from corrupting the single JSON document written to stdout.
	ThreadPool &sharedPool = ThreadPool::instance();
	Logger::Instance().SetMinLogLevel(LogLevel::Critical);
	sharedPool.initialize();

	CoreResult first, second;
	StateResult state;
	LifecycleResult lifecycle;
	QueryResult queries, performance;
	LargeWorldResult largeWorld;
	uint sharedPoolWorkers = 0, joltMaxConcurrency = 0, requestedWorkerLimit = 0, observedWorkerThreads = 0, maxPumpTasks = 0;
	uint64_t jobsQueued = 0, jobsCompleted = 0, barrierWaitMicroseconds = 0, maxBarrierWaitMicroseconds = 0, jobExecutionMicroseconds = 0;
	uint64_t allocationCount = 0, freeCount = 0, currentAllocationBytes = 0, peakAllocationBytes = 0;
	bool customAllocatorAvailable = false;
	{
		// Install the forwarding allocation probe before Factory and every Jolt
		// object. Nested lifetime ensures it is restored only after their teardown.
		ScopedJoltAllocationProbe allocations;
		Factory::sInstance = new Factory();
		RegisterTypes();
		{
			SharedPoolJoltJobSystem jobs(sharedPool, workers);
			first = RunCore(jobs);
			second = RunCore(jobs);
			state = RunStateReplayProbe(jobs);
			lifecycle = RunLifecycle(jobs);
			queries = RunQueries(jobs);
			performance = RunBodyPerformance(jobs);
			largeWorld = RunLargeWorldProbe(jobs);
			jobs.WaitForSharedPoolIdle();
			sharedPoolWorkers = jobs.GetSharedPoolWorkers();
			joltMaxConcurrency = static_cast<uint>(jobs.GetMaxConcurrency());
			jobsQueued = jobs.GetJobsQueued();
			jobsCompleted = jobs.GetJobsCompleted();
			requestedWorkerLimit = jobs.GetRequestedWorkerLimit();
			observedWorkerThreads = jobs.GetObservedWorkerThreadCount();
			maxPumpTasks = jobs.GetMaxSimultaneousPumpTasks();
			barrierWaitMicroseconds = jobs.GetBarrierWaitMicroseconds();
			maxBarrierWaitMicroseconds = jobs.GetMaxBarrierWaitMicroseconds();
			jobExecutionMicroseconds = jobs.GetJobExecutionMicroseconds();
		}
		UnregisterTypes();
		delete Factory::sInstance;
		Factory::sInstance = nullptr;
		allocationCount = allocations.GetAllocationCount();
		freeCount = allocations.GetFreeCount();
		currentAllocationBytes = allocations.GetCurrentBytes();
		peakAllocationBytes = allocations.GetPeakBytes();
		customAllocatorAvailable = allocations.IsAvailable();
	}

	// All Jolt jobs have completed and the allocator forwarding scope has been
	// restored before the shared engine workers stop.
	sharedPool.shutdown();

	std::cout << "{\n";
	std::cout << "\t\"schemaVersion\":3,\n";
	std::cout << "\t\"provider\":\"Jolt\",\n";
	std::cout << "\t\"workerCount\":" << workers << ",\n";
	std::cout << "\t\"sharedPool\":{\"initializedAtProcessStart\":true,\"shutdownCompleted\":true,\"requestedWorkerLimit\":" << requestedWorkerLimit << ",\"workerThreadsBeforeShutdown\":" << sharedPoolWorkers << ",\"observedWorkerThreadIds\":" << observedWorkerThreads << ",\"joltMaxConcurrency\":" << joltMaxConcurrency << ",\"maxSimultaneousPumpTasks\":" << maxPumpTasks << ",\"jobsQueued\":" << jobsQueued << ",\"jobsCompleted\":" << jobsCompleted << ",\"barrierWaitMicroseconds\":{\"total\":" << barrierWaitMicroseconds << ",\"max\":" << maxBarrierWaitMicroseconds << "},\"jobExecutionMicroseconds\":" << jobExecutionMicroseconds << ",\"providerPrivateThreadPool\":false},\n";
	std::cout << "\t\"precision\":{\"doublePrecisionBuild\":";
#ifdef JPH_DOUBLE_PRECISION
	PrintBool(std::cout, true);
#else
	PrintBool(std::cout, false);
#endif
	std::cout << "},\n";
	std::cout << "\t\"features\":{\"primitiveShapes\":"; PrintBool(std::cout, first.shapes);
	std::cout << ",\"linearCastCCD\":"; PrintBool(std::cout, first.ccd);
	std::cout << ",\"layersSensorsAndBodyMaterials\":"; PrintBool(std::cout, first.filtersAndMaterials);
	std::cout << ",\"rayQuery\":"; PrintBool(std::cout, first.ray);
	std::cout << ",\"shapeCastQuery\":"; PrintBool(std::cout, first.shapeCast);
	std::cout << ",\"distanceConstraint\":"; PrintBool(std::cout, first.constraint);
	std::cout << ",\"virtualCharacter\":"; PrintBool(std::cout, first.character);
	std::cout << ",\"contactAndSensorCallbacks\":"; PrintBool(std::cout, first.events);
	std::cout << ",\"debugRendererCallbacks\":"; PrintBool(std::cout, first.debugDraw); std::cout << "},\n";
	std::cout << "\t\"callbacks\":{\"total\":" << first.callbackCount << ",\"sensorContactAdds\":" << first.sensorCallbacks << "},\n";
	std::cout << "\t\"debugDraw\":{\"lineCallbacks\":" << first.debugLineCallbacks << ",\"triangleCallbacks\":" << first.debugTriangleCallbacks << ",\"geometryCallbacks\":" << first.debugGeometryCallbacks << "},\n";
	std::cout << "\t\"lifecycle\":{\"worldReplacementIterations\":" << lifecycle.iterations << ",\"lateCallbacksObserved\":" << lifecycle.lateCallbacks << ",\"cancelledJobsSkipped\":" << lifecycle.cancelledJobsSkipped << ",\"cancellationDrainRounds\":" << lifecycle.cancellationDrainRounds << ",\"shutdownRestartSupported\":"; PrintBool(std::cout, lifecycle.shutdownRestartSupported); std::cout << ",\"scope\":\"serial post-Update destruction boundary; sanitizer coverage is separate\"},\n";
	std::cout << "\t\"determinism\":{\"backendStateHash\":\"" << Hex(state.backendStateHash) << "\",\"backendStateBytes\":" << state.backendStateBytes << ",\"restoreSucceeded\":"; PrintBool(std::cout, state.restoreSucceeded); std::cout << ",\"replaySaveEqual\":"; PrintBool(std::cout, state.replaySaveEqual); std::cout << ",\"firstDivergence\":\"" << state.firstDivergence << "\",\"sortedEnginePacketHashFirst\":\"" << Hex(first.sortedPacketHash) << "\",\"sortedEnginePacketHashSecond\":\"" << Hex(second.sortedPacketHash) << "\",\"sameProcessRepeatEqual\":"; PrintBool(std::cout, first.sortedPacketHash == second.sortedPacketHash); std::cout << ",\"note\":\"cross-platform equality is intentionally not inferred\"},\n";
	std::cout << "\t\"largeWorld\":{\"originCoordinate\":" << largeWorld.originCoordinate << ",\"farCoordinate\":" << largeWorld.farCoordinate << ",\"originAbsoluteError\":" << largeWorld.originAbsoluteError << ",\"farAbsoluteError\":" << largeWorld.farAbsoluteError << ",\"originHit\":"; PrintBool(std::cout, largeWorld.originHit); std::cout << ",\"farHit\":"; PrintBool(std::cout, largeWorld.farHit); std::cout << ",\"originHitFraction\":" << largeWorld.originHitFraction << ",\"farHitFraction\":" << largeWorld.farHitFraction << ",\"precisionAssertionPassed\":"; PrintBool(std::cout, largeWorld.precisionAssertionPassed); std::cout << "},\n";
	std::cout << "\t\"queryBounds\":{\"maximumCastDistance\":" << kMaximumCastDistance << ",\"maximumShapeExtent\":" << kMaximumShapeExtent << ",\"maximumHitsPerQuery\":" << kMaximumHitsPerQuery << ",\"maximumRequestsPerBatch\":" << kMaximumRequestsPerBatch << ",\"retainedResultTicks\":" << kRetainedResultTicks << ",\"rejectedNonFinite\":" << queries.rejectedNonFinite << ",\"rejectedOversizeDistance\":" << queries.rejectedOversizeDistance << ",\"rejectedOversizeExtent\":" << queries.rejectedOversizeExtent << ",\"acceptedMaximumExtent\":"; PrintBool(std::cout, queries.acceptedMaximumExtent); std::cout << ",\"maximumExtentBroadPhaseHit\":"; PrintBool(std::cout, queries.maximumExtentBroadPhaseHit); std::cout << ",\"denseHitCount\":" << queries.denseHitCount << ",\"denseHitsRetained\":" << queries.denseHitsRetained << ",\"denseTruncated\":"; PrintBool(std::cout, queries.denseTruncated); std::cout << ",\"repeatOrderingRuns\":" << queries.repeatOrderingRuns << ",\"repeatOrderingEqual\":"; PrintBool(std::cout, queries.repeatOrderingEqual); std::cout << ",\"denseOrderingHash\":\"" << Hex(queries.denseOrderingHash) << "\",\"batchRepeatOrderingRuns\":" << queries.batchRepeatOrderingRuns << ",\"batchRepeatOrderingEqual\":"; PrintBool(std::cout, queries.batchRepeatOrderingEqual); std::cout << ",\"batchOrderingHash\":\"" << Hex(queries.batchOrderingHash) << "\",\"peakRetainedBytes\":" << queries.peakRetainedBytes << "},\n";
	std::cout << "\t\"queries\":{\"requests\":" << queries.requests << ",\"hits\":" << queries.hits << ",\"microseconds\":{\"p50\":" << queries.p50 << ",\"p95\":" << queries.p95 << ",\"p99\":" << queries.p99 << "}},\n";
	std::cout << "\t\"allocations\":{\"customAllocatorAvailable\":"; PrintBool(std::cout, customAllocatorAvailable); std::cout << ",\"allocationCount\":" << allocationCount << ",\"freeCount\":" << freeCount << ",\"currentBytes\":" << currentAllocationBytes << ",\"peakBytes\":" << peakAllocationBytes << ",\"note\":\"ReleaseASAN/TSAN intentionally use sanitizer-visible default allocation\"},\n";
	std::cout << "\t\"performance\":{\"bodyCount\":" << performance.requests << ",\"activeBodyCount\":" << performance.hits << ",\"warmupTicks\":30,\"measuredTicks\":120,\"tickMilliseconds\":{\"p50\":" << performance.p50 << ",\"p95\":" << performance.p95 << ",\"p99\":" << performance.p99 << "}},\n";
	std::cout << "\t\"gaps\":[\"No engine transaction, callback ordering, stale-generation, or scene-publication guarantee is inferred from this provider probe.\",\"Float and double precision are evaluated in separate configured builds.\",\"Cross-process equality is evaluated only by the runner and does not imply cross-platform equality.\"]\n";
	std::cout << "}\n";

	const bool completeFeatureSurface = first.shapes && first.ccd && first.filtersAndMaterials && first.ray && first.shapeCast && first.constraint && first.character && first.events && first.debugDraw;
	const bool completeLifecycle = lifecycle.iterations == 100 && lifecycle.lateCallbacks == 0 && lifecycle.shutdownRestartSupported && jobsQueued == jobsCompleted;
	const bool completeQueryCalibration = queries.requests == kMaximumRequestsPerBatch && queries.hits == kMaximumRequestsPerBatch && queries.rejectedNonFinite == 1 && queries.rejectedOversizeDistance == 1 && queries.rejectedOversizeExtent == 1 && queries.acceptedMaximumExtent && queries.maximumExtentBroadPhaseHit && queries.denseTruncated && queries.repeatOrderingEqual && queries.batchRepeatOrderingRuns == 100 && queries.batchRepeatOrderingEqual;
	const bool completeMemoryAccounting = customAllocatorAvailable? allocationCount > 0 && allocationCount == freeCount && currentAllocationBytes == 0 && peakAllocationBytes > 0 : allocationCount == 0 && freeCount == 0 && currentAllocationBytes == 0;
	return (completeFeatureSurface && state.restoreSucceeded && state.replaySaveEqual && first.sortedPacketHash == second.sortedPacketHash && completeLifecycle && largeWorld.precisionAssertionPassed && completeQueryCalibration && completeMemoryAccounting)? 0 : 2;
}
