// AnimationProcedural.cs - Copied procedural goal commands and result snapshots.

using System;
using System.Runtime.InteropServices;

namespace TerraEngine.Animation
{
	public enum AnimationProceduralCommandKind : uint
	{
		SetGoal = 0,
		ClearGoal = 1
	}

	public enum AnimationProceduralGoalKind : uint
	{
		Position = 0,
		Rotation = 1,
		PositionAndRotation = 2,
		Aim = 3,
		Look = 4,
		FootPlant = 5,
		CopyTransform = 6,
		Twist = 7
	}

	public enum AnimationProceduralCoordinateSpace : uint
	{
		Local = 0,
		Parent = 1,
		Model = 2,
		World = 3
	}

	public enum AnimationProceduralSolverKind : uint
	{
		TwoBoneIk = 0,
		ChainIk = 1,
		Aim = 2,
		Look = 3,
		JointConstraint = 4,
		CopyTransform = 5,
		TwistDistribution = 6,
		WeightedTargets = 7,
		FootPlacement = 8,
		PelvisCompensation = 9
	}

	public enum AnimationProceduralSolverState : uint
	{
		Solved = 0,
		Converged = 1,
		MaxIterations = 2,
		Unreachable = 3,
		Singular = 4,
		Clamped = 5,
		StaleInput = 6,
		NoContribution = 7,
		InvalidInput = 8
	}

	[Flags]
	public enum AnimationProceduralGoalFlags : uint
	{
		None = 0,
		UsePole = 1U << 0,
		AllowStretch = 1U << 1,
		Authoritative = 1U << 2,
		Persistent = 1U << 3
	}

	[Flags]
	public enum AnimationProceduralResultFlags : uint
	{
		None = 0,
		Converged = 1U << 0,
		Clamped = 1U << 1,
		Singular = 1U << 2,
		Unreachable = 1U << 3,
		StaleInput = 1U << 4,
		CpuAuthoritative = 1U << 5
	}

	/// <summary>
	/// One generation-safe procedural target. The native bridge copies this value;
	/// it never retains a managed object, array, delegate, or engine pointer.
	/// </summary>
	public struct AnimationProceduralGoal
	{
		public ulong GoalId;
		public ulong TargetJointStableId;
		public uint LayerIndex;
		public AnimationProceduralGoalKind Kind;
		public AnimationProceduralCoordinateSpace CoordinateSpace;
		public AnimationTransform Transform;
		public Vector3 PoleVector;
		public int Priority;
		public float PositionWeight;
		public float RotationWeight;
		public float PoleWeight;
		public float MaximumStretchScale;
		public AnimationProceduralGoalFlags Flags;
		public AnimationUuid TargetObjectId;
		public uint TargetObjectGeneration;
		public uint TargetComponentGeneration;
		public ulong ExpectedPublicationGeneration;
		public uint ExpectedPoseGeneration;
	}

	public readonly struct AnimationProceduralGoalReceipt
	{
		internal AnimationProceduralGoalReceipt(NativeAnimationProceduralGoalReceipt value)
		{
			Status = value.Status;
			Animator = value.Animator;
			Command = value.Command;
			ProducerId = value.ProducerId;
			ProducerSequence = value.ProducerSequence;
			CorrelationId = value.CorrelationId;
			IdempotencyKey = value.IdempotencyKey;
			CancellationToken = value.CancellationToken;
			GoalId = value.GoalId;
			ObservableTick = value.ObservableTick;
			PublicationGeneration = value.PublicationGeneration;
			Duplicate = value.Duplicate != 0;
			IdempotencyConflict = value.IdempotencyConflict != 0;
		}

		public AnimationStatus Status { get; }
		public AnimatorHandle Animator { get; }
		public AnimationProceduralCommandKind Command { get; }
		public uint ProducerId { get; }
		public ulong ProducerSequence { get; }
		public ulong CorrelationId { get; }
		public ulong IdempotencyKey { get; }
		public ulong CancellationToken { get; }
		public ulong GoalId { get; }
		public long ObservableTick { get; }
		public ulong PublicationGeneration { get; }
		public bool Duplicate { get; }
		public bool IdempotencyConflict { get; }
		public bool Accepted => Status == AnimationStatus.Ok || Status == AnimationStatus.Queued;
	}

	public readonly struct AnimationProceduralResultSnapshot
	{
		internal AnimationProceduralResultSnapshot(NativeAnimationProceduralResultSnapshot value)
		{
			Status = value.Status;
			Animator = value.Animator;
			Solver = value.Solver;
			State = value.State;
			GoalId = value.GoalId;
			TargetJointStableId = value.TargetJointStableId;
			ResultTransform = value.ResultTransform;
			TargetErrorMeters = value.TargetErrorMeters;
			TargetErrorRadians = value.TargetErrorRadians;
			InputWeight = value.InputWeight;
			FinalContribution = value.FinalContribution;
			IterationCount = value.IterationCount;
			IterationLimit = value.IterationLimit;
			Flags = value.Flags;
			PoseGeneration = value.PoseGeneration;
			ComponentGeneration = value.ComponentGeneration;
			SkeletonGeneration = value.SkeletonGeneration;
			ProgramGeneration = value.ProgramGeneration;
			ObservationTick = value.ObservationTick;
			PublicationGeneration = value.PublicationGeneration;
			ProducerOrder = value.ProducerOrder;
			CorrelationId = value.CorrelationId;
		}

		public AnimationStatus Status { get; }
		public AnimatorHandle Animator { get; }
		public AnimationProceduralSolverKind Solver { get; }
		public AnimationProceduralSolverState State { get; }
		public ulong GoalId { get; }
		public ulong TargetJointStableId { get; }
		public AnimationTransform ResultTransform { get; }
		public float TargetErrorMeters { get; }
		public float TargetErrorRadians { get; }
		public float InputWeight { get; }
		public float FinalContribution { get; }
		public uint IterationCount { get; }
		public uint IterationLimit { get; }
		public AnimationProceduralResultFlags Flags { get; }
		public uint PoseGeneration { get; }
		public uint ComponentGeneration { get; }
		public uint SkeletonGeneration { get; }
		public uint ProgramGeneration { get; }
		public long ObservationTick { get; }
		public ulong PublicationGeneration { get; }
		public ulong ProducerOrder { get; }
		public ulong CorrelationId { get; }
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationProceduralGoalCommand
	{
		internal AnimationAbiHeader Header;
		internal AnimatorHandle Animator;
		internal AnimationProceduralCommandKind Command;
		internal AnimationProceduralGoalKind GoalKind;
		internal AnimationProceduralCoordinateSpace CoordinateSpace;
		internal uint LayerIndex;
		internal ulong GoalId;
		internal ulong TargetJointStableId;
		internal AnimationUuid TargetObjectId;
		internal uint TargetObjectGeneration;
		internal uint TargetComponentGeneration;
		internal AnimationTransform GoalTransform;
		internal Vector3 PoleVector;
		internal int Priority;
		internal float PositionWeight;
		internal float RotationWeight;
		internal float PoleWeight;
		internal float MaximumStretchScale;
		internal AnimationProceduralGoalFlags Flags;
		internal uint ProducerId;
		internal ulong ProducerSequence;
		internal ulong CorrelationId;
		internal ulong IdempotencyKey;
		internal ulong CancellationToken;
		internal long TargetTick;
		internal ulong ExpectedPublicationGeneration;
		internal uint ExpectedPoseGeneration;
		private uint _reserved;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationProceduralGoalReceipt
	{
		internal AnimationAbiHeader Header;
		internal AnimationStatus Status;
		private ushort _reserved;
		internal AnimatorHandle Animator;
		internal AnimationProceduralCommandKind Command;
		internal uint ProducerId;
		internal uint Duplicate;
		internal uint IdempotencyConflict;
		private uint _reserved2;
		internal ulong ProducerSequence;
		internal ulong CorrelationId;
		internal ulong IdempotencyKey;
		internal ulong CancellationToken;
		internal ulong GoalId;
		internal long ObservableTick;
		internal ulong PublicationGeneration;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationProceduralResultSnapshot
	{
		internal AnimationAbiHeader Header;
		internal AnimationStatus Status;
		private ushort _reserved;
		internal AnimatorHandle Animator;
		internal AnimationProceduralSolverKind Solver;
		internal AnimationProceduralSolverState State;
		internal ulong GoalId;
		internal ulong TargetJointStableId;
		internal AnimationTransform ResultTransform;
		internal float TargetErrorMeters;
		internal float TargetErrorRadians;
		internal float InputWeight;
		internal float FinalContribution;
		internal uint IterationCount;
		internal uint IterationLimit;
		internal AnimationProceduralResultFlags Flags;
		internal uint PoseGeneration;
		internal uint ComponentGeneration;
		internal uint SkeletonGeneration;
		internal uint ProgramGeneration;
		private uint _reserved2;
		internal long ObservationTick;
		internal ulong PublicationGeneration;
		internal ulong ProducerOrder;
		internal ulong CorrelationId;
	}

	public sealed unsafe partial class Animator
	{
		/// <summary>Queues a copied goal that becomes visible at its protected target tick.</summary>
		public AnimationProceduralGoalReceipt SetProceduralGoal(
			AnimationProceduralGoal goal,
			AnimationCommandOptions options = default)
		{
			return SubmitProceduralGoal(AnimationProceduralCommandKind.SetGoal, goal, options);
		}

		/// <summary>Queues removal of one stable goal without exposing live graph storage.</summary>
		public AnimationProceduralGoalReceipt ClearProceduralGoal(
			ulong goalId,
			AnimationCommandOptions options = default,
			uint expectedPoseGeneration = 0,
			ulong expectedPublicationGeneration = 0)
		{
			AnimationProceduralGoal goal = new()
			{
				GoalId = goalId,
				ExpectedPoseGeneration = expectedPoseGeneration,
				ExpectedPublicationGeneration = expectedPublicationGeneration
			};
			return SubmitProceduralGoal(AnimationProceduralCommandKind.ClearGoal, goal, options);
		}

		/// <summary>Returns one complete copied result batch from the latest publication.</summary>
		public AnimationBufferResult<AnimationProceduralResultSnapshot> ReadProceduralResults()
			=> CopyBuffer<NativeAnimationProceduralResultSnapshot, AnimationProceduralResultSnapshot>(
				36000, AnimationRecordKind.ProceduralResultSnapshot,
				InternalCalls.Native_Animation_CopyProceduralResults,
				(value, _) => new AnimationProceduralResultSnapshot(value));

		private AnimationProceduralGoalReceipt SubmitProceduralGoal(
			AnimationProceduralCommandKind command,
			AnimationProceduralGoal goal,
			AnimationCommandOptions options)
		{
			AnimationStatus binding = EnsureAnimator();
			ulong sequence = options.ProducerSequence;
			if (sequence == 0 && !TryReserveManagedProducerSequence(out sequence))
			{
				return new AnimationProceduralGoalReceipt(NewProceduralReceipt(
					AnimationStatus.RangeOverflow, command, goal.GoalId));
			}
			NativeAnimationProceduralGoalCommand request = new()
			{
				Header = AnimationAbiHeader.For<NativeAnimationProceduralGoalCommand>(
					AnimationRecordKind.ProceduralGoalCommand),
				Animator = _handle,
				Command = command,
				GoalKind = goal.Kind,
				CoordinateSpace = goal.CoordinateSpace,
				LayerIndex = goal.LayerIndex,
				GoalId = goal.GoalId,
				TargetJointStableId = goal.TargetJointStableId,
				TargetObjectId = goal.TargetObjectId,
				TargetObjectGeneration = goal.TargetObjectGeneration,
				TargetComponentGeneration = goal.TargetComponentGeneration,
				GoalTransform = goal.Transform,
				PoleVector = goal.PoleVector,
				Priority = goal.Priority,
				PositionWeight = goal.PositionWeight,
				RotationWeight = goal.RotationWeight,
				PoleWeight = goal.PoleWeight,
				MaximumStretchScale = goal.MaximumStretchScale,
				Flags = goal.Flags,
				ProducerId = options.ProducerId == 0 ? ManagedProducerId : options.ProducerId,
				ProducerSequence = sequence,
				CorrelationId = options.CorrelationId == 0 ? sequence : options.CorrelationId,
				IdempotencyKey = options.IdempotencyKey == 0 ? sequence : options.IdempotencyKey,
				CancellationToken = options.CancellationToken == 0 ? sequence : options.CancellationToken,
				TargetTick = options.TargetTick,
				ExpectedPublicationGeneration = goal.ExpectedPublicationGeneration,
				ExpectedPoseGeneration = goal.ExpectedPoseGeneration
			};
			NativeAnimationProceduralGoalReceipt receipt = NewProceduralReceipt(
				binding, command, goal.GoalId);
			receipt.ProducerId = request.ProducerId;
			receipt.ProducerSequence = request.ProducerSequence;
			receipt.CorrelationId = request.CorrelationId;
			receipt.IdempotencyKey = request.IdempotencyKey;
			receipt.CancellationToken = request.CancellationToken;
			receipt.ObservableTick = request.TargetTick;
			if (binding == AnimationStatus.Ok)
			{
				receipt.Status = InternalCalls.Native_Animation_SubmitProceduralGoal(
					ref request, ref receipt);
			}
			return new AnimationProceduralGoalReceipt(receipt);
		}

		private NativeAnimationProceduralGoalReceipt NewProceduralReceipt(
			AnimationStatus status,
			AnimationProceduralCommandKind command,
			ulong goalId) => new()
		{
			Header = AnimationAbiHeader.For<NativeAnimationProceduralGoalReceipt>(
				AnimationRecordKind.ProceduralGoalReceipt),
			Status = status,
			Animator = _handle,
			Command = command,
			GoalId = goalId
		};
	}
}
