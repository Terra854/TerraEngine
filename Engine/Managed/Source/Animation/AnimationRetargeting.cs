// AnimationRetargeting.cs - Copied retarget control and diagnostics records.

using System;
using System.Runtime.InteropServices;

namespace TerraEngine.Animation
{
	public enum AnimationRetargetOperation : uint
	{
		ActivateProfile = 0,
		DeactivateProfile = 1,
		RequestRebuild = 2
	}

	public readonly struct AnimationRetargetReceipt
	{
		internal AnimationRetargetReceipt(NativeAnimationRetargetReceipt value)
		{
			Status = value.Status; Animator = value.Animator; Operation = value.Operation;
			ProfileGeneration = value.ProfileGeneration; CorrelationId = value.CorrelationId;
			ObservableTick = value.ObservableTick; PublicationGeneration = value.PublicationGeneration;
		}
		public AnimationStatus Status { get; }
		public AnimatorHandle Animator { get; }
		public AnimationRetargetOperation Operation { get; }
		public uint ProfileGeneration { get; }
		public ulong CorrelationId { get; }
		public long ObservableTick { get; }
		public ulong PublicationGeneration { get; }
	}

	public readonly struct AnimationRetargetSnapshot
	{
		internal AnimationRetargetSnapshot(NativeAnimationRetargetSnapshot value)
		{
			Status = value.Status; Animator = value.Animator; Profile = new AnimationAssetReference(value.ProfileAssetId, value.ProfileGeneration, AnimationAssetKind.RetargetProfile);
			SourceSkeletonGeneration = value.SourceSkeletonGeneration; TargetSkeletonGeneration = value.TargetSkeletonGeneration;
			PoseGeneration = value.PoseGeneration; ContactErrorMeters = value.ContactErrorMeters;
			ReachableIkErrorMeters = value.ReachableIkErrorMeters; ObservationTick = value.ObservationTick;
			PublicationGeneration = value.PublicationGeneration;
		}
		public AnimationStatus Status { get; }
		public AnimatorHandle Animator { get; }
		public AnimationAssetReference Profile { get; }
		public uint SourceSkeletonGeneration { get; }
		public uint TargetSkeletonGeneration { get; }
		public uint PoseGeneration { get; }
		public float ContactErrorMeters { get; }
		public float ReachableIkErrorMeters { get; }
		public long ObservationTick { get; }
		public ulong PublicationGeneration { get; }
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationRetargetRequest
	{
		internal AnimationAbiHeader Header; internal AnimatorHandle Animator; internal AnimationRetargetOperation Operation; internal uint LayerIndex;
		internal AnimationUuid ProfileAssetId; internal uint ProfileGeneration; internal uint ExpectedPoseGeneration;
		internal ulong ExpectedPublicationGeneration; internal ulong CorrelationId; internal long TargetTick;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationRetargetReceipt
	{
		internal AnimationAbiHeader Header; internal AnimationStatus Status; private ushort _reserved; internal AnimatorHandle Animator;
		internal AnimationRetargetOperation Operation; internal uint ProfileGeneration; internal ulong CorrelationId;
		internal long ObservableTick; internal ulong PublicationGeneration;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationRetargetSnapshot
	{
		internal AnimationAbiHeader Header; internal AnimationStatus Status; private ushort _reserved; internal AnimatorHandle Animator;
		internal AnimationUuid ProfileAssetId; internal uint ProfileGeneration; internal uint SourceSkeletonGeneration;
		internal uint TargetSkeletonGeneration; internal uint PoseGeneration; internal float ContactErrorMeters;
		internal float ReachableIkErrorMeters; internal long ObservationTick; internal ulong PublicationGeneration;
	}

	public sealed unsafe partial class Animator
	{
		/// <summary>Queues a copied retarget-profile change. Visibility occurs at the protected animation publication boundary.</summary>
		public AnimationRetargetReceipt RequestRetarget(AnimationRetargetOperation operation, RetargetProfileAsset profile, uint layerIndex = 0, uint expectedPoseGeneration = 0, ulong expectedPublicationGeneration = 0, ulong correlationId = 0, long targetTick = 0)
		{
			AnimationStatus binding = EnsureAnimator();
			if (binding == AnimationStatus.Ok && correlationId == 0 && !TryReserveManagedProducerSequence(out correlationId)) binding = AnimationStatus.RangeOverflow;
			NativeAnimationRetargetReceipt receipt = new() { Header = AnimationAbiHeader.For<NativeAnimationRetargetReceipt>(AnimationRecordKind.RetargetReceipt), Status = binding, Animator = _handle, Operation = operation, ProfileGeneration = profile.Reference.Generation, CorrelationId = correlationId, ObservableTick = targetTick };
			if (binding == AnimationStatus.Ok)
			{
				NativeAnimationRetargetRequest request = new() { Header = AnimationAbiHeader.For<NativeAnimationRetargetRequest>(AnimationRecordKind.RetargetRequest), Animator = _handle, Operation = operation, LayerIndex = layerIndex, ProfileAssetId = profile.Reference.Id, ProfileGeneration = profile.Reference.Generation, ExpectedPoseGeneration = expectedPoseGeneration, ExpectedPublicationGeneration = expectedPublicationGeneration, CorrelationId = correlationId, TargetTick = targetTick };
				receipt.Status = InternalCalls.Native_Animation_SubmitRetargetRequest(ref request, ref receipt);
			}
			return new AnimationRetargetReceipt(receipt);
		}

		/// <summary>Reads an immutable retarget publication batch. The returned values never alias a profile compiler or pose evaluator.</summary>
		public AnimationBufferResult<AnimationRetargetSnapshot> ReadRetargetSnapshots() => CopyBuffer<NativeAnimationRetargetSnapshot, AnimationRetargetSnapshot>(4096, AnimationRecordKind.RetargetSnapshot, InternalCalls.Native_Animation_CopyRetargetSnapshots, (value, _) => new AnimationRetargetSnapshot(value));
	}
}
