// AnimationDeterminism.cs - Value-only rollback, replay, and replication bridge.

using System;
using System.Runtime.InteropServices;

namespace TerraEngine.Animation
{
	public enum AnimationDeterminismOperation : uint { Capture = 0, Restore = 1, ReplaySeek = 2, RequestReplication = 3 }

	public readonly struct AnimationDeterminismReceipt
	{
		internal AnimationDeterminismReceipt(NativeAnimationDeterminismReceipt value) { Status = value.Status; Animator = value.Animator; Operation = value.Operation; CorrelationId = value.CorrelationId; ObservableTick = value.ObservableTick; PublicationGeneration = value.PublicationGeneration; }
		public AnimationStatus Status { get; } public AnimatorHandle Animator { get; } public AnimationDeterminismOperation Operation { get; }
		public ulong CorrelationId { get; } public long ObservableTick { get; } public ulong PublicationGeneration { get; }
	}

	public readonly struct AnimationDeterminismSnapshot
	{
		internal AnimationDeterminismSnapshot(NativeAnimationDeterminismSnapshot value) { Status = value.Status; Animator = value.Animator; Tick = value.Tick; StateChecksum = value.StateChecksum; PoseChecksum = value.PoseChecksum; PublicationGeneration = value.PublicationGeneration; ControllerStateId = value.ControllerStateId; ControllerGeneration = value.ControllerGeneration; EventWatermark = value.EventWatermark; DivergenceKind = value.DivergenceKind; }
		public AnimationStatus Status { get; } public AnimatorHandle Animator { get; } public long Tick { get; }
		public ulong StateChecksum { get; } public ulong PoseChecksum { get; } public ulong PublicationGeneration { get; }
		public ulong ControllerStateId { get; } public uint ControllerGeneration { get; } public uint EventWatermark { get; } public uint DivergenceKind { get; }
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationDeterminismRequest
	{
		internal AnimationAbiHeader Header; internal AnimatorHandle Animator; internal AnimationDeterminismOperation Operation; internal uint Flags;
		internal long SourceTick; internal long TargetTick; internal ulong ExpectedChecksum; internal ulong ExpectedPublicationGeneration; internal ulong CorrelationId;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationDeterminismReceipt
	{
		internal AnimationAbiHeader Header; internal AnimationStatus Status; private ushort _reserved; internal AnimatorHandle Animator; internal AnimationDeterminismOperation Operation; private uint _reserved2;
		internal ulong CorrelationId; internal long ObservableTick; internal ulong PublicationGeneration;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationDeterminismSnapshot
	{
		internal AnimationAbiHeader Header; internal AnimationStatus Status; private ushort _reserved; internal AnimatorHandle Animator;
		internal long Tick; internal ulong StateChecksum; internal ulong PoseChecksum; internal ulong PublicationGeneration; internal ulong ControllerStateId;
		internal uint ControllerGeneration; internal uint EventWatermark; internal uint DivergenceKind; private uint _reserved2;
	}

	public sealed unsafe partial class Animator
	{
		/// <summary>Queues a value-only deterministic action. This API does not transfer history storage, callbacks, or network buffers across the managed boundary.</summary>
		public AnimationDeterminismReceipt RequestDeterminism(AnimationDeterminismOperation operation, long sourceTick, long targetTick, ulong expectedChecksum = 0, ulong expectedPublicationGeneration = 0, uint flags = 0, ulong correlationId = 0)
		{
			AnimationStatus binding = EnsureAnimator();
			if (binding == AnimationStatus.Ok && correlationId == 0 && !TryReserveManagedProducerSequence(out correlationId)) binding = AnimationStatus.RangeOverflow;
			NativeAnimationDeterminismReceipt receipt = new() { Header = AnimationAbiHeader.For<NativeAnimationDeterminismReceipt>(AnimationRecordKind.DeterminismReceipt), Status = binding, Animator = _handle, Operation = operation, CorrelationId = correlationId, ObservableTick = targetTick };
			if (binding == AnimationStatus.Ok) { NativeAnimationDeterminismRequest request = new() { Header = AnimationAbiHeader.For<NativeAnimationDeterminismRequest>(AnimationRecordKind.DeterminismRequest), Animator = _handle, Operation = operation, Flags = flags, SourceTick = sourceTick, TargetTick = targetTick, ExpectedChecksum = expectedChecksum, ExpectedPublicationGeneration = expectedPublicationGeneration, CorrelationId = correlationId }; receipt.Status = InternalCalls.Native_Animation_SubmitDeterminismRequest(ref request, ref receipt); }
			return new AnimationDeterminismReceipt(receipt);
		}

		/// <summary>Reads a complete immutable deterministic publication batch. It is safe to retain these copied checksums after a scene, script, or replay lifecycle change.</summary>
		public AnimationBufferResult<AnimationDeterminismSnapshot> ReadDeterminismSnapshots() => CopyBuffer<NativeAnimationDeterminismSnapshot, AnimationDeterminismSnapshot>(4096, AnimationRecordKind.DeterminismSnapshot, InternalCalls.Native_Animation_CopyDeterminismSnapshots, (value, _) => new AnimationDeterminismSnapshot(value));
	}
}
