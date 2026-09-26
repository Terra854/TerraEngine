// CharacterLocomotion.cs - generation-pinned managed facade for character motion.
using System;
using System.Threading;

namespace TerraEngine.Locomotion
{
	public readonly struct CharacterLocomotionCommand
	{
		public LocomotionCommandKind Kind { get; init; }
		public LocomotionIntentKind IntentKind { get; init; }
		public LocomotionDirectionSpace DirectionSpace { get; init; }
		public LocomotionIntentLifetime Lifetime { get; init; }
		public LocomotionMotionMode Mode { get; init; }
		public double DirectionX { get; init; }
		public double DirectionY { get; init; }
		public double DirectionZ { get; init; }
		public double RequestedSpeed { get; init; }
		public ulong ExpectedAuthorityEpoch { get; init; }
		public ulong ExpectedConfigurationRevision { get; init; }
		public ulong OwnerToken { get; init; }
		public ulong ProducerId { get; init; }
		public ulong Sequence { get; init; }
		public ulong CorrelationId { get; init; }
		public ulong TransferProducerId { get; init; }
	}

	public readonly struct CharacterLocomotionBatchResult
	{
		public LocomotionStatus Status { get; init; }
		public uint RequiredCount { get; init; }
		public CharacterLocomotionReceipt[] Receipts { get; init; }
	}

	// A wrapper pins the first full scene/object/attachment identity it resolves.
	// When that attachment is replaced, native calls return StaleHandle and this
	// instance never silently acquires the replacement controller.
	public sealed class CharacterLocomotion : TerraEngine.Component
	{
		public const uint AbiVersion = 1U;
		private static long s_nextProducerId;
		private NativeLocomotionHandle _handle;
		private bool _bound;
		private bool _identityPinned;
		private uint _firstObjectEntity;
		private ulong _configurationRevision;
		private ulong _authorityEpoch;
		private ulong _ownerToken;
		private ulong _producerId;
		private ulong _sequence;
		private LocomotionStatus _lastStatus = LocomotionStatus.NotReady;

		public LocomotionStatus LastStatus => _lastStatus;
		public NativeLocomotionHandle Identity => TryBind() ? _handle : default;
		public ulong ConfigurationRevision => _configurationRevision;
		public ulong AuthorityEpoch => _authorityEpoch;
		public ulong OwnerToken => _ownerToken;
		public ulong ProducerId => _producerId;

		internal override void OnObjectAssigned()
		{
			if (_identityPinned) return;
			_firstObjectEntity = Object.ID;
		}

		private bool TryBind()
		{
			if (Object.ID != _firstObjectEntity || (_identityPinned && !_bound))
			{
				_lastStatus = LocomotionStatus.StaleHandle;
				return false;
			}
			if (_bound) return true;
			PhysicsUuid objectId = default;
			RuntimeStatus uuidStatus = InternalCalls.Native_Object_ReadUuid(Object.ID, ref objectId);
			if (uuidStatus != RuntimeStatus.Success || !objectId.IsValid)
			{
				_lastStatus = LocomotionStatus.MissingBinding;
				return false;
			}
			var request = new NativeLocomotionResolveRequest {
				Header = NativeLocomotionAbiHeader.For<NativeLocomotionResolveRequest>(NativeLocomotionRecordKind.ResolveRequest),
				ObjectId = objectId
			};
			var result = new NativeLocomotionResolveResult {
				Header = NativeLocomotionAbiHeader.For<NativeLocomotionResolveResult>(NativeLocomotionRecordKind.ResolveResult)
			};
			_lastStatus = InternalCalls.Native_Locomotion_Resolve(ref request, ref result);
			if (_lastStatus != LocomotionStatus.Success || !result.Handle.IsValid) return false;
			_handle = result.Handle;
			_configurationRevision = result.ConfigurationRevision;
			_authorityEpoch = result.AuthorityEpoch;
			_producerId = unchecked((ulong)Interlocked.Increment(ref s_nextProducerId));
			_bound = true;
			_identityPinned = true;
			return true;
		}

		private static CharacterLocomotionReceipt ConvertReceipt(NativeLocomotionReceipt source, LocomotionStatus status) => new()
		{
			Status = status, State = source.State, Reason = source.Reason,
			ProducerId = source.ProducerId, Sequence = source.Sequence,
			CorrelationId = source.CorrelationId, AcceptedUpdateEpoch = source.AcceptedUpdateEpoch,
			AuthorityEpoch = source.AuthorityEpoch, OwnerToken = source.OwnerToken
		};

		private void ObserveAppliedReceipt(CharacterLocomotionReceipt receipt)
		{
			// An older retained receipt must not revive authority after a newer
			// Release, Stop or external exclusion has already been observed.
			if (!receipt.IsApplied || receipt.ProducerId != _producerId
				|| receipt.AuthorityEpoch < _authorityEpoch) return;
			_authorityEpoch = receipt.AuthorityEpoch;
			_ownerToken = receipt.OwnerToken;
		}

		// The low-level batch stays explicit about producer, owner and epoch. This
		// is also the path used by every one-command convenience method below.
		public unsafe CharacterLocomotionBatchResult SubmitBatch(ReadOnlySpan<CharacterLocomotionCommand> commands)
		{
			if (!TryBind()) return new() { Status = _lastStatus, Receipts = Array.Empty<CharacterLocomotionReceipt>() };
			if (commands.IsEmpty || commands.Length > 256)
				return new() { Status = LocomotionStatus.InvalidArgument, Receipts = Array.Empty<CharacterLocomotionReceipt>() };
			var native = new NativeLocomotionCommand[commands.Length];
			var admitted = new NativeLocomotionReceipt[commands.Length];
			for (int index = 0; index < commands.Length; ++index)
			{
				var source = commands[index];
				native[index] = new NativeLocomotionCommand {
					Header = NativeLocomotionAbiHeader.For<NativeLocomotionCommand>(NativeLocomotionRecordKind.Command),
					Handle = _handle, Kind = source.Kind, IntentKind = source.IntentKind,
					DirectionSpace = source.DirectionSpace, Lifetime = source.Lifetime,
					Mode = source.Mode, ExpectedAuthorityEpoch = source.ExpectedAuthorityEpoch,
					ExpectedConfigurationRevision = source.ExpectedConfigurationRevision,
					OwnerToken = source.OwnerToken, ProducerId = source.ProducerId,
					Sequence = source.Sequence, CorrelationId = source.CorrelationId,
					DirectionX = source.DirectionX, DirectionY = source.DirectionY,
					DirectionZ = source.DirectionZ, RequestedSpeed = source.RequestedSpeed,
					TransferProducerId = source.TransferProducerId
				};
				admitted[index].Header = NativeLocomotionAbiHeader.For<NativeLocomotionReceipt>(NativeLocomotionRecordKind.Receipt);
			}
			var batch = new NativeLocomotionCommandBatch {
				Header = NativeLocomotionAbiHeader.For<NativeLocomotionCommandBatch>(NativeLocomotionRecordKind.CommandBatch),
				CommandCount = checked((uint)commands.Length)
			};
			var info = new NativeLocomotionBufferInfo {
				Header = NativeLocomotionAbiHeader.For<NativeLocomotionBufferInfo>(NativeLocomotionRecordKind.BufferInfo)
			};
			fixed (NativeLocomotionCommand* commandPtr = native)
			fixed (NativeLocomotionReceipt* receiptPtr = admitted)
				_lastStatus = InternalCalls.Native_Locomotion_Submit(ref batch, commandPtr, receiptPtr,
					checked((uint)admitted.Length), ref info);
			if (info.WrittenCount > admitted.Length)
				return new() { Status = LocomotionStatus.ProviderFailure, Receipts = Array.Empty<CharacterLocomotionReceipt>() };
			var receipts = new CharacterLocomotionReceipt[checked((int)info.WrittenCount)];
			for (int index = 0; index < receipts.Length; ++index)
			{
				receipts[index] = ConvertReceipt(admitted[index],
					admitted[index].State == LocomotionReceiptState.Queued ? LocomotionStatus.Queued : _lastStatus);
				ObserveAppliedReceipt(receipts[index]);
			}
			return new() { Status = _lastStatus, RequiredCount = info.RequiredCount, Receipts = receipts };
		}

		public CharacterLocomotionReceipt Submit(CharacterLocomotionCommand command)
		{
			var batch = SubmitBatch(new[] { command });
			return batch.Receipts.Length == 1 ? batch.Receipts[0]
				: new CharacterLocomotionReceipt { Status = batch.Status, State = LocomotionReceiptState.Rejected };
		}

		private CharacterLocomotionCommand OwnCommand(LocomotionCommandKind kind, LocomotionMotionMode mode = LocomotionMotionMode.Velocity)
		{
			return new CharacterLocomotionCommand {
				Kind = kind, IntentKind = LocomotionIntentKind.Script,
				DirectionSpace = LocomotionDirectionSpace.World,
				Lifetime = LocomotionIntentLifetime.UntilStopped, Mode = mode,
				ExpectedAuthorityEpoch = _authorityEpoch,
				ExpectedConfigurationRevision = _configurationRevision,
				OwnerToken = kind == LocomotionCommandKind.Acquire ? 0U : _ownerToken,
				ProducerId = _producerId,
				Sequence = ++_sequence, CorrelationId = _sequence
			};
		}

		public CharacterLocomotionReceipt Acquire(LocomotionMotionMode mode = LocomotionMotionMode.Velocity)
		{
			if (!TryBind()) return new() { Status = _lastStatus, State = LocomotionReceiptState.Rejected };
			return Submit(OwnCommand(LocomotionCommandKind.Acquire, mode));
		}

		public CharacterLocomotionReceipt SetIntent(Vector3 direction, double requestedSpeed,
			LocomotionDirectionSpace space = LocomotionDirectionSpace.World,
			LocomotionIntentLifetime lifetime = LocomotionIntentLifetime.UntilStopped)
		{
			if (!TryBind()) return new() { Status = _lastStatus, State = LocomotionReceiptState.Rejected };
			var original = OwnCommand(LocomotionCommandKind.SetIntent);
			return Submit(new CharacterLocomotionCommand {
				Kind = original.Kind, IntentKind = original.IntentKind, DirectionSpace = space,
				Lifetime = lifetime, Mode = original.Mode,
				ExpectedAuthorityEpoch = original.ExpectedAuthorityEpoch,
				ExpectedConfigurationRevision = original.ExpectedConfigurationRevision,
				OwnerToken = original.OwnerToken, ProducerId = original.ProducerId,
				Sequence = original.Sequence, CorrelationId = original.CorrelationId,
				DirectionX = direction.X, DirectionY = direction.Y, DirectionZ = direction.Z,
				RequestedSpeed = requestedSpeed
			});
		}

		public CharacterLocomotionReceipt FollowNavigation() => SubmitOwned(LocomotionCommandKind.FollowNavigation);
		public CharacterLocomotionReceipt Stop() => SubmitOwned(LocomotionCommandKind.Stop);
		public CharacterLocomotionReceipt SetMode(LocomotionMotionMode mode) => SubmitOwned(LocomotionCommandKind.SetMode, mode);
		private CharacterLocomotionReceipt SubmitOwned(LocomotionCommandKind kind, LocomotionMotionMode mode = LocomotionMotionMode.Velocity)
		{
			if (!TryBind()) return new() { Status = _lastStatus, State = LocomotionReceiptState.Rejected };
			return Submit(OwnCommand(kind, mode));
		}
		public CharacterLocomotionReceipt Transfer(ulong newProducerId)
		{
			if (!TryBind()) return new() { Status = _lastStatus, State = LocomotionReceiptState.Rejected };
			var original = OwnCommand(LocomotionCommandKind.Transfer);
			return Submit(new CharacterLocomotionCommand {
				Kind = original.Kind, IntentKind = original.IntentKind, DirectionSpace = original.DirectionSpace,
				Lifetime = original.Lifetime, Mode = original.Mode,
				ExpectedAuthorityEpoch = original.ExpectedAuthorityEpoch,
				ExpectedConfigurationRevision = original.ExpectedConfigurationRevision,
				OwnerToken = original.OwnerToken, ProducerId = original.ProducerId,
				Sequence = original.Sequence, CorrelationId = original.CorrelationId,
				TransferProducerId = newProducerId
			});
		}
		public CharacterLocomotionReceipt Release() => SubmitOwned(LocomotionCommandKind.Release);

		// Poll in managed OnUpdate. A queued Acquire does not grant its owner token;
		// only the applied receipt can update this wrapper's cached authority.
		public CharacterLocomotionReceipt ReadReceipt(ulong producerId, ulong sequence, ulong correlationId)
		{
			if (!TryBind()) return new() { Status = _lastStatus, State = LocomotionReceiptState.Rejected };
			var query = new NativeLocomotionReceiptQuery {
				Header = NativeLocomotionAbiHeader.For<NativeLocomotionReceiptQuery>(NativeLocomotionRecordKind.ReceiptQuery),
				Handle = _handle, ProducerId = producerId, Sequence = sequence, CorrelationId = correlationId
			};
			var native = new NativeLocomotionReceipt {
				Header = NativeLocomotionAbiHeader.For<NativeLocomotionReceipt>(NativeLocomotionRecordKind.Receipt)
			};
			_lastStatus = InternalCalls.Native_Locomotion_ReadReceipt(ref query, ref native);
			var result = ConvertReceipt(native, _lastStatus);
			ObserveAppliedReceipt(result);
			return result;
		}

		public unsafe LocomotionStatus CopyObservation(out CharacterLocomotionObservation observation)
		{
			observation = default;
			if (!TryBind()) return _lastStatus;
			var query = new NativeLocomotionObservationQuery {
				Header = NativeLocomotionAbiHeader.For<NativeLocomotionObservationQuery>(NativeLocomotionRecordKind.ObservationQuery),
				Handle = _handle
			};
			var native = new NativeLocomotionObservation {
				Header = NativeLocomotionAbiHeader.For<NativeLocomotionObservation>(NativeLocomotionRecordKind.Observation)
			};
			var info = new NativeLocomotionBufferInfo {
				Header = NativeLocomotionAbiHeader.For<NativeLocomotionBufferInfo>(NativeLocomotionRecordKind.BufferInfo)
			};
			_lastStatus = InternalCalls.Native_Locomotion_CopyObservations(ref query, &native, 1U, ref info);
			if (_lastStatus != LocomotionStatus.Success || info.WrittenCount != 1U) return _lastStatus;
			if (native.ConfigurationRevision > _configurationRevision)
				_configurationRevision = native.ConfigurationRevision;
			if (native.AuthorityEpoch >= _authorityEpoch)
			{
				_authorityEpoch = native.AuthorityEpoch;
				_ownerToken = native.AuthorityState == LocomotionAuthorityState.Controlled
					&& native.OwnerProducerId == _producerId ? native.OwnerToken : 0U;
			}
			observation = new CharacterLocomotionObservation {
				Handle = native.Handle, AuthorityState = native.AuthorityState, MotionMode = native.MotionMode,
				BindingReason = native.BindingReason, NavigationReason = native.NavigationReason,
				AnimationReason = native.AnimationReason, OwnerToken = native.OwnerToken,
				AuthorityEpoch = native.AuthorityEpoch, ConfigurationRevision = native.ConfigurationRevision,
				PhysicalTick = native.PhysicalTick, PublicationEpoch = native.PublicationEpoch,
				DesiredVelocityX = native.DesiredVelocityX, DesiredVelocityY = native.DesiredVelocityY,
				DesiredVelocityZ = native.DesiredVelocityZ, ObservedPlanarSpeed = native.ObservedPlanarSpeed,
				RemainingRouteDistanceMetres = native.RemainingRouteDistanceMetres,
				NavigationOutcome = native.NavigationOutcome, Grounded = native.Grounded != 0,
				MotionHistoryAvailable = native.MotionHistoryAvailable != 0,
				RequestedYawRadians = native.RequestedYawRadians, RealizedYawRadians = native.RealizedYawRadians
			};
			return _lastStatus;
		}
	}
}
