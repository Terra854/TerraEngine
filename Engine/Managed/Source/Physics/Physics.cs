// Physics.cs - bounded immutable reads and scheduled query helpers.

using System;

namespace TerraEngine
{
	public static class Physics
	{
		public static uint AbiVersion => InternalCalls.Native_Physics_GetAbiVersion();

		public static unsafe PhysicsQueryResult Query(PhysicsQueryRequest request)
		{
			NativePhysicsQueryRequest nativeRequest = ToNative(request);
			NativePhysicsQueryResultInfo info = new() { Header = PhysicsAbiHeader.For<NativePhysicsQueryResultInfo>() };
			PhysicsStatus status = InternalCalls.Native_Physics_QueryImmediate(ref nativeRequest, ref info, null, 0, out uint count);
			if (status != PhysicsStatus.Success && status != PhysicsStatus.LimitExceeded)
			{
				return new PhysicsQueryResult { Status = status, Tick = info.Tick, CorrelationId = request.CorrelationId, Diagnostic = info.Diagnostic };
			}
			NativePhysicsQueryHit[] hits = new NativePhysicsQueryHit[count];
			fixed (NativePhysicsQueryHit* pointer = hits)
			{
				status = InternalCalls.Native_Physics_QueryImmediate(ref nativeRequest, ref info, pointer, count, out _);
			}
			PhysicsQueryHit[] publicHits = new PhysicsQueryHit[hits.Length];
			for (int i = 0; i < hits.Length; ++i) publicHits[i] = hits[i].ToPublic();
			return new PhysicsQueryResult { Status = status, Tick = info.Tick, CorrelationId = info.CorrelationId, Truncated = info.Truncated != 0, Diagnostic = info.Diagnostic, Hits = publicHits };
		}

		public static unsafe PhysicsQueryHandle ScheduleQuery(PhysicsQueryRequest request, out PhysicsStatus status)
		{
			NativePhysicsQueryRequest nativeRequest = ToNative(request);
			NativePhysicsQueryReceipt receipt = new() { Header = PhysicsAbiHeader.For<NativePhysicsQueryReceipt>() };
			status = InternalCalls.Native_Physics_SubmitQuery(ref nativeRequest, ref receipt);
			return receipt.Handle;
		}

		public static unsafe PhysicsQueryHandle[] ScheduleBatch(PhysicsQueryRequest[] requests, out PhysicsStatus status)
		{
			if (requests == null || requests.Length == 0) { status = PhysicsStatus.Success; return Array.Empty<PhysicsQueryHandle>(); }
			NativePhysicsQueryRequest[] nativeRequests = new NativePhysicsQueryRequest[requests.Length];
			NativePhysicsQueryReceipt[] receipts = new NativePhysicsQueryReceipt[requests.Length];
			for (int i = 0; i < requests.Length; ++i) { nativeRequests[i] = ToNative(requests[i]); receipts[i].Header = PhysicsAbiHeader.For<NativePhysicsQueryReceipt>(); }
			fixed (NativePhysicsQueryRequest* requestPointer = nativeRequests)
			fixed (NativePhysicsQueryReceipt* receiptPointer = receipts)
			{
				status = InternalCalls.Native_Physics_SubmitQueryBatch(requestPointer, (uint)nativeRequests.Length, receiptPointer, (uint)receipts.Length, out _);
			}
			PhysicsQueryHandle[] handles = new PhysicsQueryHandle[receipts.Length];
			for (int i = 0; i < receipts.Length; ++i) handles[i] = receipts[i].Handle;
			return handles;
		}

		public static unsafe PhysicsQueryResult ReadQuery(PhysicsQueryHandle handle)
		{
			NativePhysicsQueryResultInfo info = new() { Header = PhysicsAbiHeader.For<NativePhysicsQueryResultInfo>() };
			InternalCalls.Native_Physics_ReadQuery(ref handle, ref info, null, 0, out uint count);
			NativePhysicsQueryHit[] hits = new NativePhysicsQueryHit[count];
			fixed (NativePhysicsQueryHit* pointer = hits)
			{
				PhysicsStatus status = InternalCalls.Native_Physics_ReadQuery(ref handle, ref info, pointer, count, out _);
				PhysicsQueryHit[] publicHits = new PhysicsQueryHit[hits.Length];
				for (int i = 0; i < hits.Length; ++i) publicHits[i] = hits[i].ToPublic();
				return new PhysicsQueryResult { Status = status, Tick = info.Tick, CorrelationId = info.CorrelationId, Truncated = info.Truncated != 0, Diagnostic = info.Diagnostic, Hits = publicHits };
			}
		}

		public static PhysicsStatus CancelQuery(PhysicsQueryHandle handle) => InternalCalls.Native_Physics_CancelQuery(ref handle);

		public static unsafe PhysicsBodyState[] BodyStates()
		{
			InternalCalls.Native_Physics_CopyBodyStates(null, 0, out uint count);
			NativePhysicsBodyState[] states = new NativePhysicsBodyState[count];
			fixed (NativePhysicsBodyState* pointer = states)
			{
				if (InternalCalls.Native_Physics_CopyBodyStates(pointer, count, out _) != PhysicsStatus.Success) return Array.Empty<PhysicsBodyState>();
			}
			PhysicsBodyState[] output = new PhysicsBodyState[states.Length]; for (int i = 0; i < states.Length; ++i) output[i] = states[i].ToPublic(); return output;
		}

		public static unsafe PhysicsCharacterState[] CharacterStates()
		{
			InternalCalls.Native_Physics_CopyCharacterStates(null, 0, out uint count);
			NativePhysicsCharacterState[] states = new NativePhysicsCharacterState[count];
			fixed (NativePhysicsCharacterState* pointer = states)
			{
				if (InternalCalls.Native_Physics_CopyCharacterStates(pointer, count, out _) != PhysicsStatus.Success) return Array.Empty<PhysicsCharacterState>();
			}
			PhysicsCharacterState[] output = new PhysicsCharacterState[states.Length]; for (int i = 0; i < states.Length; ++i) output[i] = states[i].ToPublic(); return output;
		}

		public static PhysicsSettingsSnapshot Settings()
		{
			NativePhysicsSettingsSnapshot settings = new() { Header = PhysicsAbiHeader.For<NativePhysicsSettingsSnapshot>() };
			InternalCalls.Native_Physics_ReadSettings(ref settings);
			return settings.ToPublic();
		}

		// Component construction resolves stable native identities and runtime
		// handles by entity only long enough to copy them. Retained wrappers are
		// revalidated by UUID plus object/component/binding/world generations.
		internal static void BindComponent(Component component, Object owner)
		{
			if (component is Rigidbody rigidbody)
			{
				NativePhysicsBodyState state = new() { Header = PhysicsAbiHeader.For<NativePhysicsBodyState>() };
				if (InternalCalls.Native_Physics_ResolveBody(owner.ID, ref state) == PhysicsStatus.Success)
				{
					rigidbody.Handle = state.Body;
					rigidbody.Identity = state.Identity;
				}
				return;
			}
			if (component is CharacterController character)
			{
				NativePhysicsCharacterState state = new() { Header = PhysicsAbiHeader.For<NativePhysicsCharacterState>() };
				if (InternalCalls.Native_Physics_ResolveCharacter(owner.ID, ref state) == PhysicsStatus.Success)
				{
					character.Handle = state.Character;
					character.Identity = state.Identity;
				}
				return;
			}

			NativePhysicsIdentitySnapshot identity = new() { Header = PhysicsAbiHeader.For<NativePhysicsIdentitySnapshot>() };
			if (InternalCalls.Native_Physics_ResolveIdentity(owner.ID, ref identity) != PhysicsStatus.Success) return;
			if (component is ColliderSet colliders) colliders.Identity = identity.Identity;
			if (component is ConstraintSet constraints) constraints.Identity = identity.Identity;
		}

		private static NativePhysicsQueryRequest ToNative(PhysicsQueryRequest request) => new()
		{
			Header = PhysicsAbiHeader.For<NativePhysicsQueryRequest>(), World = request.World, Kind = request.Kind, HitMode = request.HitMode,
			Pose = request.Pose, Direction = request.Direction, ShapeExtent = request.ShapeExtent, Distance = request.Distance, MaximumHits = request.MaximumHits,
			CollisionMask = request.Filter.CollisionMask, BodyTypeMask = request.Filter.BodyTypeMask, IncludeSensors = request.Filter.IncludeSensors ? (byte)1 : (byte)0,
			SubmittedTick = request.SubmittedTick, ExpiryTick = request.ExpiryTick, CorrelationId = request.CorrelationId
		};
	}
}
