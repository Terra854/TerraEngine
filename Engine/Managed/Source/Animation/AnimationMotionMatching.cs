// AnimationMotionMatching.cs - Copied motion database controls and diagnostics.

using System;
using System.Runtime.InteropServices;

namespace TerraEngine.Animation
{
	public enum AnimationMotionMatchOperation : uint { SetDatabase = 0, Query = 1, Clear = 2 }

	[Flags]
	public enum AnimationMotionMatchQueryFields : uint
	{
		None = 0,
		Identity = 1u << 0,
		Features = 1u << 1,
		FeatureWeights = 1u << 2,
		Tags = 1u << 3,
		Contacts = 1u << 4,
		Phase = 1u << 5,
		Transitions = 1u << 6,
		Continuity = 1u << 7,
		Exclusions = 1u << 8,
		Costs = 1u << 9,
		Budgets = 1u << 10,
		All = (1u << 11) - 1u
	}

	[Flags]
	public enum AnimationMotionMatchQueryReceiptFlags : uint
	{
		None = 0,
		PayloadRetained = 1u << 0,
		RequiresRuntimeQueryIntegration = 1u << 1
	}

	public enum AnimationMotionMatchFallbackReason : uint
	{
		None = 0,
		EmptyDatabase = 1,
		NoEligibleCandidates = 2,
		CandidateBudgetExhausted = 3,
		GpuUnavailable = 4,
		GpuNotReady = 5,
		GpuCancelled = 6,
		StaleGpuResult = 7,
		GpuBackendFailure = 8,
		InvalidGpuCandidates = 9
	}

	public enum AnimationMotionMatchSelectionSource : uint
	{
		None = 0,
		GpuTopKCpuAuthoritative = 1,
		CpuFullSearch = 2
	}

	[Flags]
	public enum AnimationMotionMatchExclusionReason : uint
	{
		None = 0,
		MissingRequiredTags = 1u << 0,
		ContainsExcludedTags = 1u << 1,
		MissingRequiredContacts = 1u << 2,
		PhaseOutsideWindow = 1u << 3,
		ExplicitSample = 1u << 4,
		DatabaseExclusionRegion = 1u << 5,
		MissingRequiredTransitionFlags = 1u << 6,
		ContainsExcludedTransitionFlags = 1u << 7
	}

	[Flags]
	public enum AnimationMotionMatchResultFields : uint
	{
		None = 0,
		Identity = 1u << 0,
		DatabaseSchema = 1u << 1,
		SelectedSample = 1u << 2,
		SelectedSource = 1u << 3,
		TotalCost = 1u << 4,
		FeatureBlockCosts = 1u << 5,
		MetadataCosts = 1u << 6,
		Exclusions = 1u << 7,
		Fallback = 1u << 8,
		SelectionSource = 1u << 9,
		GpuStatus = 1u << 10,
		CandidateCounts = 1u << 11
	}

	[Flags]
	public enum AnimationMotionMatchResultFlags : uint
	{
		None = 0,
		UsedFallback = 1u << 0,
		UsedGpuCandidates = 1u << 1,
		UsedCpuFallback = 1u << 2,
		CandidateBudgetExhausted = 1u << 3,
		LegacyAggregateScoreConversion = 1u << 4,
		RuntimeUsedLegacyQuery = 1u << 5
	}

	/// <summary>A complete immutable 48-lane motion query copied into native storage.</summary>
	public sealed class AnimationMotionMatchQuery
	{
		public const int FeatureCount = 48;
		public const int FeatureBlockCount = 9;
		public const int MaximumExcludedSamples = 256;
		public const uint DefaultTopK = 32;
		public const uint MaximumTopK = 256;
		public const uint MaximumSamples = 16_777_216;

		private readonly short[] _features;
		private readonly ushort[] _featureWeights;
		private readonly ulong[] _excludedSampleIds;

		public AnimationMotionMatchQuery(ulong schemaFingerprint,
			ReadOnlySpan<short> features)
			: this(schemaFingerprint, features, ReadOnlySpan<ushort>.Empty,
				ReadOnlySpan<ulong>.Empty)
		{
		}

		public AnimationMotionMatchQuery(ulong schemaFingerprint,
			ReadOnlySpan<short> features,
			ReadOnlySpan<ushort> featureWeights,
			ReadOnlySpan<ulong> excludedSampleIds)
		{
			if (schemaFingerprint == 0) throw new ArgumentOutOfRangeException(nameof(schemaFingerprint));
			if (features.Length != FeatureCount) throw new ArgumentException("Motion queries require exactly 48 quantized features.", nameof(features));
			if (!featureWeights.IsEmpty && featureWeights.Length != FeatureCount) throw new ArgumentException("Motion queries require exactly 48 feature weights.", nameof(featureWeights));
			if (excludedSampleIds.Length > MaximumExcludedSamples) throw new ArgumentException("The copied exclusion list cannot exceed 256 sample IDs.", nameof(excludedSampleIds));
			for (int index = 0; index < excludedSampleIds.Length; ++index)
			{
				if (excludedSampleIds[index] == 0 || (index != 0 && excludedSampleIds[index] <= excludedSampleIds[index - 1]))
				{
					throw new ArgumentException("Excluded sample IDs must be non-zero, unique, and strictly increasing.", nameof(excludedSampleIds));
				}
			}
			SchemaFingerprint = schemaFingerprint;
			_features = features.ToArray();
			_featureWeights = new ushort[FeatureCount];
			if (featureWeights.IsEmpty) Array.Fill(_featureWeights, (ushort)1);
			else featureWeights.CopyTo(_featureWeights);
			_excludedSampleIds = excludedSampleIds.ToArray();
		}

		public ulong SchemaFingerprint { get; }
		public ReadOnlyMemory<short> Features => _features;
		public ReadOnlyMemory<ushort> FeatureWeights => _featureWeights;
		public ReadOnlyMemory<ulong> ExcludedSampleIds => _excludedSampleIds;
		public AnimationMotionMatchQueryFields Fields { get; init; } = AnimationMotionMatchQueryFields.All;
		public ulong RequiredTags { get; init; }
		public ulong ExcludedTags { get; init; }
		public ulong PreferredTags { get; init; }
		public ulong LastSampleId { get; init; }
		public ulong LastSourceClipId { get; init; }
		public uint RequiredContacts { get; init; }
		public uint PreferredContacts { get; init; }
		public uint PreferredContinuityGroup { get; init; }
		public uint ExcludedRegionFlags { get; init; }
		public ushort PreferredPhase { get; init; }
		public ushort MaximumPhaseDistance { get; init; } = ushort.MaxValue;
		public ushort RequiredTransitionFlags { get; init; }
		public ushort ExcludedTransitionFlags { get; init; }
		public ushort PreferredTransitionFlags { get; init; }
		public uint PhaseWeight { get; init; } = 1;
		public uint ContinuityPenalty { get; init; }
		public uint ContinuityBreakPenalty { get; init; }
		public uint SameClipPenalty { get; init; }
		public uint MissingPreferredTagPenalty { get; init; }
		public uint ContactMismatchPenalty { get; init; }
		public uint TransitionMismatchPenalty { get; init; }
		public uint MaximumCandidates { get; init; } = DefaultTopK;
		public uint MaximumSamplesToVisit { get; init; } = MaximumSamples;
		public uint MaximumDiagnosticExclusions { get; init; } = 32;

		internal unsafe NativeAnimationMotionMatchQueryRequestV2 ToNative(
			AnimatorHandle animator, AnimationAssetReference database,
			ulong queryId, ulong correlationId, long targetTick)
		{
			NativeAnimationMotionMatchQueryRequestV2 request = new()
			{
				Header = AnimationAbiHeader.For<NativeAnimationMotionMatchQueryRequestV2>(
					AnimationRecordKind.MotionMatchQueryRequestV2),
				Animator = animator,
				DatabaseAssetId = database.Id,
				DatabaseGeneration = database.Generation,
				FeatureCount = FeatureCount,
				SchemaFingerprint = SchemaFingerprint,
				QueryId = queryId,
				CorrelationId = correlationId,
				TargetTick = targetTick,
				RequiredTags = RequiredTags,
				ExcludedTags = ExcludedTags,
				PreferredTags = PreferredTags,
				LastSampleId = LastSampleId,
				LastSourceClipId = LastSourceClipId,
				RequiredContacts = RequiredContacts,
				PreferredContacts = PreferredContacts,
				PreferredContinuityGroup = PreferredContinuityGroup,
				ExcludedRegionFlags = ExcludedRegionFlags,
				PreferredPhase = PreferredPhase,
				MaximumPhaseDistance = MaximumPhaseDistance,
				RequiredTransitionFlags = RequiredTransitionFlags,
				ExcludedTransitionFlags = ExcludedTransitionFlags,
				PreferredTransitionFlags = PreferredTransitionFlags,
				PhaseWeight = PhaseWeight,
				ContinuityPenalty = ContinuityPenalty,
				ContinuityBreakPenalty = ContinuityBreakPenalty,
				SameClipPenalty = SameClipPenalty,
				MissingPreferredTagPenalty = MissingPreferredTagPenalty,
				ContactMismatchPenalty = ContactMismatchPenalty,
				TransitionMismatchPenalty = TransitionMismatchPenalty,
				MaximumCandidates = MaximumCandidates,
				MaximumSamplesToVisit = MaximumSamplesToVisit,
				MaximumDiagnosticExclusions = MaximumDiagnosticExclusions,
				ExcludedSampleCount = (uint)_excludedSampleIds.Length,
				QueryFields = (uint)Fields
			};
			// A local unmanaged record is already fixed for the lifetime of this method.
			// Taking its inline-buffer pointers directly avoids pinning an expression that
			// the C# compiler has already classified as fixed storage.
			short* featureDestination = request.Features;
			_features.AsSpan().CopyTo(new Span<short>(featureDestination, FeatureCount));
			ushort* weightDestination = request.FeatureWeights;
			_featureWeights.AsSpan().CopyTo(new Span<ushort>(weightDestination, FeatureCount));
			ulong* exclusionDestination = request.ExcludedSampleIds;
			_excludedSampleIds.AsSpan().CopyTo(new Span<ulong>(
				exclusionDestination, _excludedSampleIds.Length));
			return request;
		}
	}

	public readonly struct AnimationMotionMatchReceipt
	{
		internal AnimationMotionMatchReceipt(NativeAnimationMotionMatchReceipt value) { Status = value.Status; Animator = value.Animator; Operation = value.Operation; DatabaseGeneration = value.DatabaseGeneration; QueryId = value.QueryId; CorrelationId = value.CorrelationId; ObservableTick = value.ObservableTick; PublicationGeneration = value.PublicationGeneration; }
		public AnimationStatus Status { get; } public AnimatorHandle Animator { get; } public AnimationMotionMatchOperation Operation { get; }
		public uint DatabaseGeneration { get; } public ulong QueryId { get; } public ulong CorrelationId { get; }
		public long ObservableTick { get; } public ulong PublicationGeneration { get; }
	}

	public readonly struct AnimationMotionMatchQueryReceipt
	{
		internal AnimationMotionMatchQueryReceipt(NativeAnimationMotionMatchQueryReceiptV2 value,
			AnimationAssetReference database)
		{
			Status = value.Status;
			Animator = value.Animator;
			Database = database;
			SchemaFingerprint = value.SchemaFingerprint;
			QueryId = value.QueryId;
			CorrelationId = value.CorrelationId;
			ObservableTick = value.ObservableTick;
			PublicationGeneration = value.PublicationGeneration;
			ValidatedFields = (AnimationMotionMatchQueryFields)value.ValidatedFields;
			ScheduledFields = (AnimationMotionMatchQueryFields)value.ScheduledFields;
			Flags = (AnimationMotionMatchQueryReceiptFlags)value.Flags;
		}

		public AnimationStatus Status { get; }
		public AnimatorHandle Animator { get; }
		public AnimationAssetReference Database { get; }
		public ulong SchemaFingerprint { get; }
		public ulong QueryId { get; }
		public ulong CorrelationId { get; }
		public long ObservableTick { get; }
		public ulong PublicationGeneration { get; }
		public AnimationMotionMatchQueryFields ValidatedFields { get; }
		public AnimationMotionMatchQueryFields ScheduledFields { get; }
		public AnimationMotionMatchQueryReceiptFlags Flags { get; }
		public bool FullyScheduled => (Status == AnimationStatus.Ok
			|| Status == AnimationStatus.Queued)
			&& ValidatedFields != AnimationMotionMatchQueryFields.None
			&& ScheduledFields == ValidatedFields;
	}

	public readonly struct AnimationMotionMatchSnapshot
	{
		internal AnimationMotionMatchSnapshot(NativeAnimationMotionMatchSnapshot value) { Status = value.Status; Animator = value.Animator; Database = new AnimationAssetReference(value.DatabaseAssetId, value.DatabaseGeneration, AnimationAssetKind.MotionDatabase); QueryId = value.QueryId; SelectedSampleId = value.SelectedSampleId; SelectedClipGeneration = value.SelectedClipGeneration; TotalScore = value.TotalScore; TrajectoryScore = value.TrajectoryScore; PoseScore = value.PoseScore; UsedFallback = value.UsedFallback != 0; ObservationTick = value.ObservationTick; PublicationGeneration = value.PublicationGeneration; }
		public AnimationStatus Status { get; } public AnimatorHandle Animator { get; } public AnimationAssetReference Database { get; }
		public ulong QueryId { get; } public ulong SelectedSampleId { get; } public uint SelectedClipGeneration { get; }
		public float TotalScore { get; } public float TrajectoryScore { get; } public float PoseScore { get; } public bool UsedFallback { get; }
		public long ObservationTick { get; } public ulong PublicationGeneration { get; }
	}

	/// <summary>A copied diagnostic result. Check PopulatedFields before consuming optional details.</summary>
	public readonly struct AnimationMotionMatchResult
	{
		internal unsafe AnimationMotionMatchResult(NativeAnimationMotionMatchResultV2 value)
		{
			Status = value.Status;
			GpuStatus = value.GpuStatus;
			FallbackReason = value.FallbackReason;
			SelectionSource = value.SelectionSource;
			PopulatedFields = (AnimationMotionMatchResultFields)value.PopulatedFields;
			Flags = (AnimationMotionMatchResultFlags)value.Flags;
			Animator = value.Animator;
			Database = new AnimationAssetReference(value.DatabaseAssetId, value.DatabaseGeneration,
				AnimationAssetKind.MotionDatabase);
			QueryId = value.QueryId;
			SelectedSampleId = value.SelectedSampleId;
			SelectedSourceClipId = value.SelectedSourceClipId;
			SelectedSourceTick = value.SelectedSourceTick;
			SelectedClipGeneration = value.SelectedClipGeneration;
			SchemaFingerprint = value.SchemaFingerprint;
			TotalCost = value.TotalCost;
			FeatureCost = value.FeatureCost;
			ulong[] blockCosts = new ulong[AnimationMotionMatchQuery.FeatureBlockCount];
			// The native result arrives by value, so its fixed buffer is already stable
			// while this constructor copies the nine costs into managed ownership.
			ulong* blockCostSource = value.FeatureBlockCosts;
			new ReadOnlySpan<ulong>(blockCostSource,
				AnimationMotionMatchQuery.FeatureBlockCount).CopyTo(blockCosts);
			FeatureBlockCosts = blockCosts;
			PhaseCost = value.PhaseCost;
			TagCost = value.TagCost;
			ContactCost = value.ContactCost;
			ContinuityCost = value.ContinuityCost;
			TransitionCost = value.TransitionCost;
			VisitedSamples = value.VisitedSamples;
			EvaluatedSamples = value.EvaluatedSamples;
			ExcludedSamples = value.ExcludedSamples;
			ExclusionReasons = (AnimationMotionMatchExclusionReason)value.ExclusionReasonMask;
			GpuCandidateCount = value.GpuCandidateCount;
			ObservationTick = value.ObservationTick;
			PublicationGeneration = value.PublicationGeneration;
		}

		public AnimationStatus Status { get; }
		public AnimationStatus GpuStatus { get; }
		public AnimationMotionMatchFallbackReason FallbackReason { get; }
		public AnimationMotionMatchSelectionSource SelectionSource { get; }
		public AnimationMotionMatchResultFields PopulatedFields { get; }
		public AnimationMotionMatchResultFlags Flags { get; }
		public AnimatorHandle Animator { get; }
		public AnimationAssetReference Database { get; }
		public ulong QueryId { get; }
		public ulong SelectedSampleId { get; }
		public ulong SelectedSourceClipId { get; }
		public long SelectedSourceTick { get; }
		public uint SelectedClipGeneration { get; }
		public ulong SchemaFingerprint { get; }
		public ulong TotalCost { get; }
		public ulong FeatureCost { get; }
		public ReadOnlyMemory<ulong> FeatureBlockCosts { get; }
		public ulong PhaseCost { get; }
		public ulong TagCost { get; }
		public ulong ContactCost { get; }
		public ulong ContinuityCost { get; }
		public ulong TransitionCost { get; }
		public uint VisitedSamples { get; }
		public uint EvaluatedSamples { get; }
		public uint ExcludedSamples { get; }
		public AnimationMotionMatchExclusionReason ExclusionReasons { get; }
		public uint GpuCandidateCount { get; }
		public long ObservationTick { get; }
		public ulong PublicationGeneration { get; }
		public bool Has(AnimationMotionMatchResultFields field) => (PopulatedFields & field) == field;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationMotionMatchRequest
	{
		internal AnimationAbiHeader Header; internal AnimatorHandle Animator; internal AnimationMotionMatchOperation Operation; internal uint Flags;
		internal AnimationUuid DatabaseAssetId; internal uint DatabaseGeneration; internal uint FeatureCount; internal ulong QueryId; internal ulong CorrelationId; internal long TargetTick;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationMotionMatchReceipt
	{
		internal AnimationAbiHeader Header; internal AnimationStatus Status; private ushort _reserved; internal AnimatorHandle Animator; internal AnimationMotionMatchOperation Operation; internal uint DatabaseGeneration;
		internal ulong QueryId; internal ulong CorrelationId; internal long ObservableTick; internal ulong PublicationGeneration;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationMotionMatchSnapshot
	{
		internal AnimationAbiHeader Header; internal AnimationStatus Status; private ushort _reserved; internal AnimatorHandle Animator; internal AnimationUuid DatabaseAssetId;
		internal ulong QueryId; internal ulong SelectedSampleId; internal uint DatabaseGeneration; internal uint SelectedClipGeneration;
		internal float TotalScore; internal float TrajectoryScore; internal float PoseScore; internal uint UsedFallback; internal long ObservationTick; internal ulong PublicationGeneration;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal unsafe struct NativeAnimationMotionMatchQueryRequestV2
	{
		internal AnimationAbiHeader Header;
		internal AnimatorHandle Animator;
		internal AnimationUuid DatabaseAssetId;
		internal uint DatabaseGeneration;
		internal uint FeatureCount;
		internal ulong SchemaFingerprint;
		internal ulong QueryId;
		internal ulong CorrelationId;
		internal long TargetTick;
		internal ulong RequiredTags;
		internal ulong ExcludedTags;
		internal ulong PreferredTags;
		internal ulong LastSampleId;
		internal ulong LastSourceClipId;
		internal uint RequiredContacts;
		internal uint PreferredContacts;
		internal uint PreferredContinuityGroup;
		internal uint ExcludedRegionFlags;
		internal ushort PreferredPhase;
		internal ushort MaximumPhaseDistance;
		internal ushort RequiredTransitionFlags;
		internal ushort ExcludedTransitionFlags;
		internal ushort PreferredTransitionFlags;
		private ushort _reserved16;
		internal uint PhaseWeight;
		internal uint ContinuityPenalty;
		internal uint ContinuityBreakPenalty;
		internal uint SameClipPenalty;
		internal uint MissingPreferredTagPenalty;
		internal uint ContactMismatchPenalty;
		internal uint TransitionMismatchPenalty;
		internal uint MaximumCandidates;
		internal uint MaximumSamplesToVisit;
		internal uint MaximumDiagnosticExclusions;
		internal uint ExcludedSampleCount;
		internal uint QueryFields;
		private uint _reserved32;
		internal fixed short Features[AnimationMotionMatchQuery.FeatureCount];
		internal fixed ushort FeatureWeights[AnimationMotionMatchQuery.FeatureCount];
		internal fixed ulong ExcludedSampleIds[AnimationMotionMatchQuery.MaximumExcludedSamples];
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAnimationMotionMatchQueryReceiptV2
	{
		internal AnimationAbiHeader Header;
		internal AnimationStatus Status;
		private ushort _reserved;
		internal AnimatorHandle Animator;
		internal uint DatabaseGeneration;
		private uint _reserved2;
		internal ulong SchemaFingerprint;
		internal ulong QueryId;
		internal ulong CorrelationId;
		internal long ObservableTick;
		internal ulong PublicationGeneration;
		internal uint ValidatedFields;
		internal uint ScheduledFields;
		internal uint Flags;
		private uint _reserved3;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal unsafe struct NativeAnimationMotionMatchResultV2
	{
		internal AnimationAbiHeader Header;
		internal AnimationStatus Status;
		internal AnimationStatus GpuStatus;
		internal AnimationMotionMatchFallbackReason FallbackReason;
		internal AnimationMotionMatchSelectionSource SelectionSource;
		internal uint PopulatedFields;
		internal uint Flags;
		internal AnimatorHandle Animator;
		internal AnimationUuid DatabaseAssetId;
		internal ulong QueryId;
		internal ulong SelectedSampleId;
		internal ulong SelectedSourceClipId;
		internal long SelectedSourceTick;
		internal uint DatabaseGeneration;
		internal uint SelectedClipGeneration;
		internal ulong SchemaFingerprint;
		internal ulong TotalCost;
		internal ulong FeatureCost;
		internal fixed ulong FeatureBlockCosts[AnimationMotionMatchQuery.FeatureBlockCount];
		internal ulong PhaseCost;
		internal ulong TagCost;
		internal ulong ContactCost;
		internal ulong ContinuityCost;
		internal ulong TransitionCost;
		internal uint VisitedSamples;
		internal uint EvaluatedSamples;
		internal uint ExcludedSamples;
		internal uint ExclusionReasonMask;
		internal uint GpuCandidateCount;
		private uint _reserved;
		internal long ObservationTick;
		internal ulong PublicationGeneration;
	}

	public sealed unsafe partial class Animator
	{
		/// <summary>Queues the legacy control envelope. SetDatabase and Clear remain ABI-v1 compatible.</summary>
		public AnimationMotionMatchReceipt RequestMotionMatch(AnimationMotionMatchOperation operation, MotionDatabaseAsset database, ulong queryId = 0, uint featureCount = 48, uint flags = 0, ulong correlationId = 0, long targetTick = 0)
		{
			AnimationStatus binding = EnsureAnimator();
			if (binding == AnimationStatus.Ok && correlationId == 0 && !TryReserveManagedProducerSequence(out correlationId)) binding = AnimationStatus.RangeOverflow;
			NativeAnimationMotionMatchReceipt receipt = new() { Header = AnimationAbiHeader.For<NativeAnimationMotionMatchReceipt>(AnimationRecordKind.MotionMatchReceipt), Status = binding, Animator = _handle, Operation = operation, DatabaseGeneration = database.Reference.Generation, QueryId = queryId, CorrelationId = correlationId, ObservableTick = targetTick };
			if (binding == AnimationStatus.Ok) { NativeAnimationMotionMatchRequest request = new() { Header = AnimationAbiHeader.For<NativeAnimationMotionMatchRequest>(AnimationRecordKind.MotionMatchRequest), Animator = _handle, Operation = operation, Flags = flags, DatabaseAssetId = database.Reference.Id, DatabaseGeneration = database.Reference.Generation, FeatureCount = featureCount, QueryId = queryId, CorrelationId = correlationId, TargetTick = targetTick }; receipt.Status = InternalCalls.Native_Animation_SubmitMotionMatchRequest(ref request, ref receipt); }
			return new AnimationMotionMatchReceipt(receipt);
		}

		/// <summary>Queues one immutable typed query; the receipt reports exactly which fields reached the current runtime evaluator.</summary>
		public AnimationMotionMatchQueryReceipt QueryMotionMatch(MotionDatabaseAsset database,
			AnimationMotionMatchQuery query, ulong queryId = 0, ulong correlationId = 0,
			long targetTick = 0)
		{
			AnimationStatus binding = EnsureAnimator();
			if (query == null) binding = AnimationStatus.InvalidArgument;
			if (binding == AnimationStatus.Ok && (queryId == 0 || correlationId == 0))
			{
				if (!TryReserveManagedProducerSequence(out ulong generated)) binding = AnimationStatus.RangeOverflow;
				else { if (queryId == 0) queryId = generated; if (correlationId == 0) correlationId = generated; }
			}
			NativeAnimationMotionMatchQueryReceiptV2 receipt = new()
			{
				Header = AnimationAbiHeader.For<NativeAnimationMotionMatchQueryReceiptV2>(
					AnimationRecordKind.MotionMatchQueryReceiptV2),
				Status = binding,
				Animator = _handle,
				DatabaseGeneration = database.Reference.Generation,
				SchemaFingerprint = query?.SchemaFingerprint ?? 0,
				QueryId = queryId,
				CorrelationId = correlationId,
				ObservableTick = targetTick
			};
			if (binding == AnimationStatus.Ok)
			{
				NativeAnimationMotionMatchQueryRequestV2 request = query!.ToNative(
					_handle, database.Reference, queryId, correlationId, targetTick);
				receipt.Status = InternalCalls.Native_Animation_SubmitMotionMatchQueryV2(
					ref request, ref receipt);
			}
			return new AnimationMotionMatchQueryReceipt(receipt, database.Reference);
		}

		/// <summary>Reads complete copied legacy selection snapshots.</summary>
		public AnimationBufferResult<AnimationMotionMatchSnapshot> ReadMotionMatchSnapshots() => CopyBuffer<NativeAnimationMotionMatchSnapshot, AnimationMotionMatchSnapshot>(4096, AnimationRecordKind.MotionMatchSnapshot, InternalCalls.Native_Animation_CopyMotionMatchSnapshots, (value, _) => new AnimationMotionMatchSnapshot(value));

		/// <summary>Reads versioned results with exact field-validity flags and no live evaluator aliases.</summary>
		public AnimationBufferResult<AnimationMotionMatchResult> ReadMotionMatchResults() => CopyBuffer<NativeAnimationMotionMatchResultV2, AnimationMotionMatchResult>(4096, AnimationRecordKind.MotionMatchResultV2, InternalCalls.Native_Animation_CopyMotionMatchResultsV2, (value, _) => new AnimationMotionMatchResult(value));
	}
}
