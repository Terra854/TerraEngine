// AnimationRagdoll.cs - Copied ragdoll requests and protected-boundary diagnostics.

using System;
using System.Runtime.InteropServices;

namespace TerraEngine.Animation
{
	public enum AnimationRagdollOperation : uint { Activate = 0, Deactivate = 1, RequestRecovery = 2 }

	public readonly struct AnimationRagdollReceipt
	{
		internal AnimationRagdollReceipt(NativeAnimationRagdollReceipt value) { Status = value.Status; Animator = value.Animator; Operation = value.Operation; ProfileGeneration = value.ProfileGeneration; CorrelationId = value.CorrelationId; ObservableTick = value.ObservableTick; PublicationGeneration = value.PublicationGeneration; }
		public AnimationStatus Status { get; } public AnimatorHandle Animator { get; } public AnimationRagdollOperation Operation { get; }
		public uint ProfileGeneration { get; } public ulong CorrelationId { get; } public long ObservableTick { get; } public ulong PublicationGeneration { get; }
	}

	public readonly struct AnimationRagdollSnapshot
	{
		internal AnimationRagdollSnapshot(NativeAnimationRagdollSnapshot value) { Status = value.Status; Animator = value.Animator; Profile = new AnimationAssetReference(value.ProfileAssetId, value.ProfileGeneration, AnimationAssetKind.RagdollProfile); PhysicsWorldGeneration = value.PhysicsWorldGeneration; Lifecycle = value.Lifecycle; BodyCount = value.BodyCount; BlendWeight = value.BlendWeight; RecoveryBlocked = value.RecoveryBlocked != 0; ObservationTick = value.ObservationTick; PublicationGeneration = value.PublicationGeneration; }
		public AnimationStatus Status { get; } public AnimatorHandle Animator { get; } public AnimationAssetReference Profile { get; }
		public uint PhysicsWorldGeneration { get; } public uint Lifecycle { get; } public uint BodyCount { get; }
		public float BlendWeight { get; } public bool RecoveryBlocked { get; } public long ObservationTick { get; } public ulong PublicationGeneration { get; }
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationRagdollRequest
	{
		internal AnimationAbiHeader Header; internal AnimatorHandle Animator; internal AnimationRagdollOperation Operation; internal uint Flags;
		internal AnimationUuid ProfileAssetId; internal uint ProfileGeneration; internal uint PhysicsWorldGeneration; internal float BlendWeight; private uint _reserved;
		internal ulong CorrelationId; internal long TargetTick;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationRagdollReceipt
	{
		internal AnimationAbiHeader Header; internal AnimationStatus Status; private ushort _reserved; internal AnimatorHandle Animator; internal AnimationRagdollOperation Operation; internal uint ProfileGeneration;
		internal ulong CorrelationId; internal long ObservableTick; internal ulong PublicationGeneration;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationRagdollSnapshot
	{
		internal AnimationAbiHeader Header; internal AnimationStatus Status; private ushort _reserved; internal AnimatorHandle Animator; internal AnimationUuid ProfileAssetId;
		internal uint ProfileGeneration; internal uint PhysicsWorldGeneration; internal uint Lifecycle; internal uint BodyCount; internal float BlendWeight; internal uint RecoveryBlocked;
		internal long ObservationTick; internal ulong PublicationGeneration;
	}

	public sealed unsafe partial class Animator
	{
		/// <summary>Queues a copied ragdoll request. It cannot retain a PhysicsSystem body or world pointer, and stale worlds are reported by a future scheduler owner.</summary>
		public AnimationRagdollReceipt RequestRagdoll(AnimationRagdollOperation operation, RagdollProfileAsset profile, float blendWeight = 1.0f, uint physicsWorldGeneration = 0, uint flags = 0, ulong correlationId = 0, long targetTick = 0)
		{
			AnimationStatus binding = EnsureAnimator();
			if (binding == AnimationStatus.Ok && correlationId == 0 && !TryReserveManagedProducerSequence(out correlationId)) binding = AnimationStatus.RangeOverflow;
			NativeAnimationRagdollReceipt receipt = new() { Header = AnimationAbiHeader.For<NativeAnimationRagdollReceipt>(AnimationRecordKind.RagdollReceipt), Status = binding, Animator = _handle, Operation = operation, ProfileGeneration = profile.Reference.Generation, CorrelationId = correlationId, ObservableTick = targetTick };
			if (binding == AnimationStatus.Ok) { NativeAnimationRagdollRequest request = new() { Header = AnimationAbiHeader.For<NativeAnimationRagdollRequest>(AnimationRecordKind.RagdollRequest), Animator = _handle, Operation = operation, Flags = flags, ProfileAssetId = profile.Reference.Id, ProfileGeneration = profile.Reference.Generation, PhysicsWorldGeneration = physicsWorldGeneration, BlendWeight = blendWeight, CorrelationId = correlationId, TargetTick = targetTick }; receipt.Status = InternalCalls.Native_Animation_SubmitRagdollRequest(ref request, ref receipt); }
			return new AnimationRagdollReceipt(receipt);
		}

		/// <summary>Reads copied physics-to-animation state only after its declared publication boundary; the result is never a live Jolt pose.</summary>
		public AnimationBufferResult<AnimationRagdollSnapshot> ReadRagdollSnapshots() => CopyBuffer<NativeAnimationRagdollSnapshot, AnimationRagdollSnapshot>(4096, AnimationRecordKind.RagdollSnapshot, InternalCalls.Native_Animation_CopyRagdollSnapshots, (value, _) => new AnimationRagdollSnapshot(value));
	}
}
