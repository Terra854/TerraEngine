// Animator.cs - generation-safe managed AnimatorComponent facade.

using System;
using System.Runtime.CompilerServices;
using System.Threading;

namespace TerraEngine.Animation
{
	/// <summary>
	/// Managed gameplay facade for the native CRTP AnimatorComponent. Every write
	/// is copied into the deferred animation command queue, and every read returns
	/// a complete immutable packet from a protected publication boundary.
	/// </summary>
	public sealed unsafe partial class Animator : Component
	{
		private const uint ManagedProducerId = 1;
		private static long s_nextProducerSequence;
		private static int s_producerSequenceExhausted;
		private AnimatorHandle _handle = new() { SlotIndex = AnimatorHandle.InvalidSlot };
		private AnimationUuid _ownerObjectId;
		private uint _componentGeneration;
		private AnimationStatus _bindingStatus = AnimationStatus.NotReady;
		private bool _hasEverBound;

		public AnimatorHandle Handle => _handle;
		public AnimationUuid OwnerObjectId => _ownerObjectId;
		public uint ComponentGeneration => _componentGeneration;
		public AnimationStatus BindingStatus => _bindingStatus;

		// These properties are authored component state. Setters follow the same
		// deferred reflected-component visibility rules as every managed component.
		public AnimationExecutionPolicy RequestedExecutionPolicy
		{
			get => (AnimationExecutionPolicy)GetProperty<uint>("requestedMode");
			set => SetProperty("requestedMode", (uint)value);
		}

		public AnimatorRootMotionPolicy RootMotionPolicy
		{
			get => (AnimatorRootMotionPolicy)GetProperty<uint>("rootMotionPolicy");
			set => SetProperty("rootMotionPolicy", (uint)value);
		}

		/// <summary>
		/// Persistent fixed-timeline frame used when a direct clip or controller
		/// entry clip is freshly bound. The timeline is 60 frames per second, and
		/// native binding safely clamps the value after clip reimport.
		/// </summary>
		public uint DefaultClipStartFrame
		{
			get => GetProperty<uint>("defaultClipStartFrame");
			set => SetProperty("defaultClipStartFrame", value);
		}

		/// <summary>
		/// Starts authored playback automatically when the runtime scene begins.
		/// Disable this to hold the configured initial pose until Play is called.
		/// </summary>
		public bool PlayAutomatically
		{
			get => GetProperty<bool>("playAutomatically");
			set => SetProperty("playAutomatically", value);
		}

		public AnimatorUpdatePolicy UpdatePolicy
		{
			get => (AnimatorUpdatePolicy)GetProperty<uint>("updatePolicy");
			set => SetProperty("updatePolicy", (uint)value);
		}

		public AnimatorTimePolicy TimePolicy
		{
			get => (AnimatorTimePolicy)GetProperty<uint>("timePolicy");
			set => SetProperty("timePolicy", (uint)value);
		}

		public string InitialParametersJson
		{
			get => GetStringProperty("initialParameters");
			set => SetStringProperty("initialParameters", value ?? "{}");
		}

		public AnimatorEventPolicy EventPolicy
		{
			get => (AnimatorEventPolicy)GetProperty<uint>("eventPolicy");
			set => SetProperty("eventPolicy", (uint)value);
		}

		public AnimatorCullingPolicy CullingPolicy
		{
			get => (AnimatorCullingPolicy)GetProperty<uint>("cullingPolicy");
			set => SetProperty("cullingPolicy", (uint)value);
		}

		public uint DiagnosticOverrides
		{
			get => GetProperty<uint>("diagnosticOverrides");
			set => SetProperty("diagnosticOverrides", value);
		}

		internal override void OnObjectAssigned()
		{
			// Copy the runtime identity immediately. Once a wrapper has successfully
			// bound, it never silently rebinds to a reused object or component slot.
			TryResolveAnimator();
		}

		public AnimatorStateSnapshot ReadState()
		{
			AnimationStatus binding = EnsureAnimator();
			NativeAnimationStateSnapshot native = new()
			{
				Header = AnimationAbiHeader.For<NativeAnimationStateSnapshot>(AnimationRecordKind.StateSnapshot),
				Status = binding,
				Animator = _handle
			};
			if (binding == AnimationStatus.Ok)
			{
				native.Status = InternalCalls.Native_Animation_ReadState(ref _handle, ref native);
			}
			return new AnimatorStateSnapshot(native);
		}

		public AnimationCapabilityResult QueryCapability(
			AnimationCapability capability,
			ulong stableId = 0,
			uint layerIndex = 0)
		{
			AnimationStatus binding = EnsureAnimator();
			NativeAnimationCapabilityResult result = new()
			{
				Header = AnimationAbiHeader.For<NativeAnimationCapabilityResult>(AnimationRecordKind.CapabilityResult),
				Status = binding,
				Capability = capability,
				ReasonMask = (uint)AnimationCapabilityReason.AnimatorNotReady
			};
			if (binding != AnimationStatus.Ok) return new AnimationCapabilityResult(result);

			NativeAnimationCapabilityQuery query = new()
			{
				Header = AnimationAbiHeader.For<NativeAnimationCapabilityQuery>(AnimationRecordKind.CapabilityQuery),
				Animator = _handle,
				Capability = capability,
				LayerIndex = layerIndex,
				StableId = stableId
			};
			result.Status = InternalCalls.Native_Animation_QueryCapability(ref query, ref result);
			return new AnimationCapabilityResult(result);
		}

		public AnimationCommandReceipt SetParameter(AnimationParameterId parameter, bool value, AnimationCommandOptions options = default)
		{
			NativeAnimationCommandRequest request = CreateCommand(AnimationCommandKind.SetParameter, options);
			request.ParameterId = parameter.Value;
			request.ParameterType = AnimationParameterType.Boolean;
			request.BooleanValue = value ? 1U : 0U;
			return Submit(request);
		}

		public AnimationCommandReceipt SetParameter(AnimationParameterId parameter, int value, AnimationCommandOptions options = default)
		{
			NativeAnimationCommandRequest request = CreateCommand(AnimationCommandKind.SetParameter, options);
			request.ParameterId = parameter.Value;
			request.ParameterType = AnimationParameterType.Integer;
			request.IntegerValue = value;
			return Submit(request);
		}

		public AnimationCommandReceipt SetParameter(AnimationParameterId parameter, float value, AnimationCommandOptions options = default)
		{
			NativeAnimationCommandRequest request = CreateCommand(AnimationCommandKind.SetParameter, options);
			request.ParameterId = parameter.Value;
			request.ParameterType = AnimationParameterType.Float;
			request.FloatValue = value;
			return Submit(request);
		}

		public AnimationCommandReceipt SetParameter(AnimationParameterId parameter, Vector3 value, AnimationCommandOptions options = default)
		{
			NativeAnimationCommandRequest request = CreateCommand(AnimationCommandKind.SetParameter, options);
			request.ParameterId = parameter.Value;
			request.ParameterType = AnimationParameterType.Vector3;
			request.ParameterVector = value;
			return Submit(request);
		}

		public AnimationCommandReceipt SetParameter(AnimationParameterId parameter, Quaternion value, AnimationCommandOptions options = default)
		{
			NativeAnimationCommandRequest request = CreateCommand(AnimationCommandKind.SetParameter, options);
			request.ParameterId = parameter.Value;
			request.ParameterType = AnimationParameterType.Quaternion;
			request.ParameterQuaternion = value;
			return Submit(request);
		}

		public AnimationCommandReceipt SetParameter(AnimationParameterId parameter, AnimationTransform value, AnimationCommandOptions options = default)
		{
			NativeAnimationCommandRequest request = CreateCommand(AnimationCommandKind.SetParameter, options);
			request.ParameterId = parameter.Value;
			request.ParameterType = AnimationParameterType.Transform;
			request.ParameterTransform = value;
			return Submit(request);
		}

		public AnimationCommandReceipt FireTrigger(AnimationParameterId parameter, AnimationCommandOptions options = default)
		{
			NativeAnimationCommandRequest request = CreateCommand(AnimationCommandKind.FireTrigger, options);
			request.ParameterId = parameter.Value;
			request.ParameterType = AnimationParameterType.Trigger;
			request.BooleanValue = 1;
			return Submit(request);
		}

		public AnimationCommandReceipt Play(ulong stateId, long stateTick = 0, AnimationCommandOptions options = default)
		{
			NativeAnimationCommandRequest request = CreateCommand(AnimationCommandKind.Play, options);
			request.StateId = stateId;
			request.PlaybackTick = stateTick;
			return Submit(request);
		}

		public AnimationCommandReceipt CrossFade(ulong stateId, float transitionSeconds, AnimationCommandOptions options = default)
		{
			NativeAnimationCommandRequest request = CreateCommand(AnimationCommandKind.CrossFade, options);
			request.StateId = stateId;
			request.TransitionSeconds = transitionSeconds;
			return Submit(request);
		}

		public AnimationCommandReceipt Stop(AnimationCommandOptions options = default) => Submit(CreateCommand(AnimationCommandKind.Stop, options));
		public AnimationCommandReceipt Pause(AnimationCommandOptions options = default) => Submit(CreateCommand(AnimationCommandKind.Pause, options));
		public AnimationCommandReceipt Resume(AnimationCommandOptions options = default) => Submit(CreateCommand(AnimationCommandKind.Resume, options));

		public AnimationCommandReceipt Seek(long stateTick, AnimationCommandOptions options = default)
		{
			NativeAnimationCommandRequest request = CreateCommand(AnimationCommandKind.Seek, options);
			request.PlaybackTick = stateTick;
			return Submit(request);
		}

		public AnimationCommandReceipt SetLooping(bool looping, AnimationCommandOptions options = default)
		{
			NativeAnimationCommandRequest request = CreateCommand(AnimationCommandKind.SetLooping, options);
			request.PlaybackFlag = looping ? 1U : 0U;
			return Submit(request);
		}

		public AnimationCommandReceipt SetReverse(bool reverse, AnimationCommandOptions options = default)
		{
			NativeAnimationCommandRequest request = CreateCommand(AnimationCommandKind.SetReverse, options);
			request.PlaybackFlag = reverse ? 1U : 0U;
			return Submit(request);
		}

		public AnimationCommandReceipt SetSpeed(float speed, AnimationCommandOptions options = default)
		{
			NativeAnimationCommandRequest request = CreateCommand(AnimationCommandKind.SetSpeed, options);
			request.PlaybackSpeed = speed;
			return Submit(request);
		}

		public AnimationCommandReceipt SetLayerWeight(uint layerIndex, float weight, AnimationCommandOptions options = default)
		{
			NativeAnimationCommandRequest request = CreateCommand(AnimationCommandKind.SetLayerWeight, options);
			request.LayerIndex = layerIndex;
			request.FloatValue = weight;
			return Submit(request);
		}

		public AnimationCommandReceipt SetIkGoal(AnimationIkGoal goal, AnimationCommandOptions options = default)
		{
			NativeAnimationCommandRequest request = CreateCommand(AnimationCommandKind.SetIkGoal, options);
			request.ParameterId = goal.GoalId;
			request.LayerIndex = goal.LayerIndex;
			request.GoalTransform = goal.Transform;
			request.PoleVector = goal.PoleVector;
			request.PositionWeight = goal.PositionWeight;
			request.RotationWeight = goal.RotationWeight;
			request.PoleWeight = goal.PoleWeight;
			request.IkFlags = goal.Flags;
			request.TargetObjectId = goal.TargetObjectId;
			request.TargetObjectGeneration = goal.TargetObjectGeneration;
			request.TargetComponentGeneration = goal.TargetComponentGeneration;
			return Submit(request);
		}

		public AnimationCommandReceipt ClearIkGoal(ulong goalId, AnimationCommandOptions options = default)
		{
			NativeAnimationCommandRequest request = CreateCommand(AnimationCommandKind.ClearIkGoal, options);
			request.ParameterId = goalId;
			return Submit(request);
		}

		public AnimationCommandReceipt RequestRagdoll(bool active, uint layerIndex = 0, float weight = 1.0f, AnimationCommandOptions options = default)
		{
			NativeAnimationCommandRequest request = CreateCommand(AnimationCommandKind.RequestRagdoll, options);
			request.LayerIndex = layerIndex;
			request.BooleanValue = active ? 1U : 0U;
			request.FloatValue = weight;
			return Submit(request);
		}

		public AnimationCommandReceipt ReadReceipt(uint producerId, ulong producerSequence, ulong correlationId = 0)
		{
			// Commands created by this facade default correlation to the producer
			// sequence. Mirror that rule for the lookup convenience overload.
			if (correlationId == 0) correlationId = producerSequence;
			NativeAnimationReceiptQuery query = new()
			{
				Header = AnimationAbiHeader.For<NativeAnimationReceiptQuery>(AnimationRecordKind.ReceiptQuery),
				ProducerId = producerId,
				ProducerSequence = producerSequence,
				CorrelationId = correlationId
			};
			NativeAnimationCommandReceipt receipt = NewReceipt(AnimationStatus.NotReady);
			receipt.Status = InternalCalls.Native_Animation_ReadReceipt(ref query, ref receipt);
			return new AnimationCommandReceipt(receipt);
		}

		public AnimationCommandReceipt Cancel(ulong cancellationToken, ulong correlationId = 0)
		{
			// A non-zero correlation is mandatory in the native transaction log. A
			// caller that only retained the cancellation token still gets a usable,
			// deterministic lookup identity.
			if (correlationId == 0) correlationId = cancellationToken;
			NativeAnimationCancellationRequest request = new()
			{
				Header = AnimationAbiHeader.For<NativeAnimationCancellationRequest>(AnimationRecordKind.CancellationRequest),
				CancellationToken = cancellationToken,
				CorrelationId = correlationId
			};
			NativeAnimationCommandReceipt receipt = NewReceipt(AnimationStatus.NotReady);
			receipt.Status = InternalCalls.Native_Animation_CancelCommand(ref request, ref receipt);
			return new AnimationCommandReceipt(receipt);
		}

		public AnimationBufferResult<AnimationNamedValue> ReadParameters() => CopyBuffer<NativeAnimationNamedValue, AnimationNamedValue>(
			4096, AnimationRecordKind.NamedValue, InternalCalls.Native_Animation_CopyParameters,
			(value, _) => new AnimationNamedValue(value));

		public AnimationBufferResult<AnimationNamedValue> ReadCurves() => CopyBuffer<NativeAnimationNamedValue, AnimationNamedValue>(
			65536, AnimationRecordKind.NamedValue, InternalCalls.Native_Animation_CopyCurves,
			(value, _) => new AnimationNamedValue(value));

		public AnimationBufferResult<AnimationNamedValue> ReadMorphs() => CopyBuffer<NativeAnimationNamedValue, AnimationNamedValue>(
			4096, AnimationRecordKind.NamedValue, InternalCalls.Native_Animation_CopyMorphs,
			(value, _) => new AnimationNamedValue(value));

		public AnimationBufferResult<AnimationBoneSnapshot> ReadBones() => CopyBuffer<NativeAnimationBoneSnapshot, AnimationBoneSnapshot>(
			4096, AnimationRecordKind.BoneSnapshot, InternalCalls.Native_Animation_CopyBones,
			(value, _) => new AnimationBoneSnapshot(value));

		public AnimationBufferResult<AnimationSocketSnapshot> ReadSockets() => CopyBuffer<NativeAnimationSocketSnapshot, AnimationSocketSnapshot>(
			4096, AnimationRecordKind.SocketSnapshot, InternalCalls.Native_Animation_CopySockets,
			(value, _) => new AnimationSocketSnapshot(value));

		public AnimationBufferResult<AnimationEvent> ReadEvents() => CopyBuffer<NativeAnimationEvent, AnimationEvent>(
			8192, AnimationRecordKind.Event, InternalCalls.Native_Animation_CopyEvents,
			(value, publication) => new AnimationEvent(value, publication));

		public AnimationBufferResult<AnimationRootMotionResult> ReadRootMotionResults() => CopyBuffer<NativeAnimationRootMotionResultDetails, AnimationRootMotionResult>(
			8192, AnimationRecordKind.RootMotionResultDetails, InternalCalls.Native_Animation_CopyRootMotionResultDetails,
			(value, _) => new AnimationRootMotionResult(value));

		/// <summary>Queues one exact copied result for transform, character, body, or network consumption.</summary>
		public AnimationRootMotionApplicationReceipt ApplyRootMotion(
			AnimationRootMotionApplicationMode mode,
			AnimationRootMotionResult source,
			AnimationRootMotionApplicationOptions options = default)
		{
			AnimationStatus binding = EnsureAnimator();
			if (binding != AnimationStatus.Ok || source.Animator != _handle
				|| source.Generation == 0 || source.PublicationGeneration == 0)
			{
				NativeAnimationRootMotionApplicationReceipt rejected = NewRootMotionReceipt(
					binding == AnimationStatus.Ok ? AnimationStatus.StaleHandle : binding, mode);
				return new AnimationRootMotionApplicationReceipt(rejected);
			}
			return ApplyRootMotion(mode, source.Generation, source.PublicationGeneration, options);
		}

		/// <summary>Queues a request only if the named pose and authority publication are still latest.</summary>
		public AnimationRootMotionApplicationReceipt ApplyRootMotion(
			AnimationRootMotionApplicationMode mode,
			uint expectedPoseGeneration,
			ulong expectedPublicationGeneration,
			AnimationRootMotionApplicationOptions options = default)
		{
			AnimationStatus binding = EnsureAnimator();
			ulong sequence = options.ProducerSequence;
			if (sequence == 0)
			{
				if (!TryReserveManagedProducerSequence(out sequence))
				{
					NativeAnimationRootMotionApplicationReceipt exhausted =
						NewRootMotionReceipt(AnimationStatus.RangeOverflow, mode);
					return new AnimationRootMotionApplicationReceipt(exhausted);
				}
			}
			NativeAnimationRootMotionApplicationRequest request = new()
			{
				Header = AnimationAbiHeader.For<NativeAnimationRootMotionApplicationRequest>(
					AnimationRecordKind.RootMotionApplicationRequest),
				Animator = _handle,
				Mode = mode,
				ExpectedPoseGeneration = expectedPoseGeneration,
				ExpectedPublicationGeneration = expectedPublicationGeneration,
				ProducerId = options.ProducerId == 0 ? ManagedProducerId : options.ProducerId,
				ProducerSequence = sequence,
				CorrelationId = options.CorrelationId == 0 ? sequence : options.CorrelationId,
				IdempotencyKey = options.IdempotencyKey == 0 ? sequence : options.IdempotencyKey,
				FixedDurationSeconds = options.FixedDurationSeconds > 0.0f
					? options.FixedDurationSeconds : 1.0f / 60.0f
			};
			NativeAnimationRootMotionApplicationReceipt receipt = NewRootMotionReceipt(binding, mode);
			receipt.Generation = expectedPoseGeneration;
			receipt.ProducerId = request.ProducerId;
			receipt.ProducerSequence = request.ProducerSequence;
			receipt.CorrelationId = request.CorrelationId;
			receipt.IdempotencyKey = request.IdempotencyKey;
			receipt.PublicationGeneration = expectedPublicationGeneration;
			if (binding == AnimationStatus.Ok)
			{
				receipt.Status = InternalCalls.Native_Animation_ApplyRootMotion(ref request, ref receipt);
			}
			return new AnimationRootMotionApplicationReceipt(receipt);
		}

		public AnimationEventSubscription SubscribeEvents(Action<AnimationEvent> callback, AnimationEventFilter? filter = null)
		{
			AnimationStatus binding = EnsureAnimator();
			if (binding != AnimationStatus.Ok)
			{
				return new AnimationEventSubscription { CreationStatus = binding };
			}

			AnimationEventFilter selected = filter ?? AnimationEventFilter.All;
			if (selected.Animator.IsValid && selected.Animator != _handle)
			{
				return new AnimationEventSubscription { CreationStatus = AnimationStatus.InvalidArgument };
			}
			AnimationEventFilter scoped = new(_handle, selected.Controller, selected.StateId,
				selected.LayerIndex, selected.EventId, selected.Tag, selected.PayloadCategories);
			return AnimationEvents.Subscribe(callback, scoped);
		}

		private void TryResolveAnimator()
		{
			if (_hasEverBound) return;
			NativeAnimationAnimatorHandleRecord record = new()
			{
				Header = AnimationAbiHeader.For<NativeAnimationAnimatorHandleRecord>(AnimationRecordKind.AnimatorHandle),
				Status = AnimationStatus.NotReady,
				Animator = new AnimatorHandle { SlotIndex = AnimatorHandle.InvalidSlot }
			};
			_bindingStatus = InternalCalls.Native_Animation_ResolveAnimator(Object.ID, ref record);
			if (_bindingStatus != AnimationStatus.Ok || !record.Animator.IsValid) return;
			_handle = record.Animator;
			_ownerObjectId = record.OwnerObjectId;
			_componentGeneration = record.ComponentGeneration;
			_hasEverBound = true;
		}

		private AnimationStatus EnsureAnimator()
		{
			if (_handle.IsValid) return AnimationStatus.Ok;
			if (_hasEverBound) return AnimationStatus.StaleHandle;
			TryResolveAnimator();
			return _handle.IsValid ? AnimationStatus.Ok : _bindingStatus;
		}

		private static NativeAnimationCommandRequest CreateCommand(AnimationCommandKind kind, AnimationCommandOptions options)
		{
			ulong sequence = options.ProducerSequence;
			if (sequence == 0)
			{
				// Submit turns this zero sentinel into a copied RangeOverflow receipt.
				_ = TryReserveManagedProducerSequence(out sequence);
			}
			return new NativeAnimationCommandRequest
			{
				Header = AnimationAbiHeader.For<NativeAnimationCommandRequest>(AnimationRecordKind.CommandRequest),
				Command = kind,
				PlaybackSpeed = 1.0f,
				GoalTransform = AnimationTransform.Identity,
				ParameterTransform = AnimationTransform.Identity,
				PositionWeight = 1.0f,
				RotationWeight = 1.0f,
				ProducerId = options.ProducerId == 0 ? ManagedProducerId : options.ProducerId,
				ProducerSequence = sequence,
				CorrelationId = options.CorrelationId == 0 ? sequence : options.CorrelationId,
				IdempotencyKey = options.IdempotencyKey == 0 ? sequence : options.IdempotencyKey,
				CancellationToken = options.CancellationToken == 0 ? sequence : options.CancellationToken,
				TargetTick = options.TargetTick
			};
		}

		private static bool TryReserveManagedProducerSequence(out ulong sequence)
		{
			sequence = 0;
			// Once signed monotonic space is exhausted, latch the producer closed.
			// Stopping all later increments is what guarantees the process can never
			// wrap through the negative range and eventually reuse identity one.
			if (Volatile.Read(ref s_producerSequenceExhausted) != 0) return false;
			long next = Interlocked.Increment(ref s_nextProducerSequence);
			if (next <= 0)
			{
				Interlocked.Exchange(ref s_producerSequenceExhausted, 1);
				return false;
			}
			if (Volatile.Read(ref s_producerSequenceExhausted) != 0) return false;
			sequence = (ulong)next;
			return true;
		}

		private AnimationCommandReceipt Submit(NativeAnimationCommandRequest request)
		{
			AnimationStatus binding = EnsureAnimator();
			request.Animator = _handle;
			NativeAnimationCommandReceipt receipt = NewReceipt(binding);
			receipt.ProducerId = request.ProducerId;
			receipt.ProducerSequence = request.ProducerSequence;
			receipt.CorrelationId = request.CorrelationId;
			receipt.IdempotencyKey = request.IdempotencyKey;
			receipt.CancellationToken = request.CancellationToken;
			receipt.TargetTick = request.TargetTick;
			if (request.ProducerSequence == 0)
			{
				receipt.Status = AnimationStatus.RangeOverflow;
				return new AnimationCommandReceipt(receipt);
			}
			if (binding == AnimationStatus.Ok)
			{
				receipt.Status = InternalCalls.Native_Animation_SubmitCommand(ref request, ref receipt);
			}
			return new AnimationCommandReceipt(receipt);
		}

		private NativeAnimationCommandReceipt NewReceipt(AnimationStatus status) => new()
		{
			Header = AnimationAbiHeader.For<NativeAnimationCommandReceipt>(AnimationRecordKind.CommandReceipt),
			Status = status,
			Animator = _handle
		};

		private NativeAnimationRootMotionApplicationReceipt NewRootMotionReceipt(
			AnimationStatus status,
			AnimationRootMotionApplicationMode mode) => new()
		{
			Header = AnimationAbiHeader.For<NativeAnimationRootMotionApplicationReceipt>(
				AnimationRecordKind.RootMotionApplicationReceipt),
			Status = status,
			Animator = _handle,
			Mode = mode
		};

		private unsafe delegate AnimationStatus CopyNativeValues<TNative>(
			ref AnimatorHandle animator,
			TNative* values,
			uint capacity,
			ref NativeAnimationBufferInfo info) where TNative : unmanaged;

		private unsafe AnimationBufferResult<TPublic> CopyBuffer<TNative, TPublic>(
			uint hardLimit,
			AnimationRecordKind recordKind,
			CopyNativeValues<TNative> copy,
			Func<TNative, ulong, TPublic> convert) where TNative : unmanaged
		{
			AnimationStatus binding = EnsureAnimator();
			NativeAnimationBufferInfo info = new()
			{
				Header = AnimationAbiHeader.For<NativeAnimationBufferInfo>(AnimationRecordKind.BufferInfo),
				Status = binding
			};
			if (binding != AnimationStatus.Ok) return new AnimationBufferResult<TPublic>(binding, Array.Empty<TPublic>(), info);

			AnimationStatus probe = copy(ref _handle, null, 0, ref info);
			if (info.RequiredCount == 0)
			{
				AnimationStatus emptyStatus = probe == AnimationStatus.BufferTooSmall ? AnimationStatus.Ok : probe;
				return new AnimationBufferResult<TPublic>(emptyStatus, Array.Empty<TPublic>(), info);
			}
			if ((probe != AnimationStatus.Ok && probe != AnimationStatus.BufferTooSmall)
				|| info.RequiredCount > hardLimit || info.RequiredCount > int.MaxValue)
			{
				AnimationStatus rejected = info.RequiredCount > hardLimit
					? AnimationStatus.HardLimitExceeded
					: probe;
				return new AnimationBufferResult<TPublic>(rejected, Array.Empty<TPublic>(), info);
			}

			TNative[] nativeValues = new TNative[checked((int)info.RequiredCount)];
			info = new NativeAnimationBufferInfo
			{
				Header = AnimationAbiHeader.For<NativeAnimationBufferInfo>(AnimationRecordKind.BufferInfo)
			};
			fixed (TNative* output = nativeValues)
			{
				AnimationStatus status = copy(ref _handle, output, (uint)nativeValues.Length, ref info);
				if (status != AnimationStatus.Ok || info.WrittenCount > (uint)nativeValues.Length)
				{
					return new AnimationBufferResult<TPublic>(status, Array.Empty<TPublic>(), info);
				}
			}

			TPublic[] publicValues = new TPublic[checked((int)info.WrittenCount)];
			for (int index = 0; index < publicValues.Length; ++index)
			{
				// Every provider record must match the requested kind. Validate the
				// common prefix before converting any element so a corrupt batch has
				// no partial managed semantic success.
				AnimationAbiHeader* header = (AnimationAbiHeader*)Unsafe.AsPointer(ref nativeValues[index]);
				if (header->AbiVersion != AnimationAbiHeader.Version
					|| header->Kind != recordKind
					|| header->Size < (uint)sizeof(TNative))
				{
					return new AnimationBufferResult<TPublic>(AnimationStatus.InvalidEnvelope, Array.Empty<TPublic>(), info);
				}
				publicValues[index] = convert(nativeValues[index], info.PublicationGeneration);
			}
			return new AnimationBufferResult<TPublic>(AnimationStatus.Ok, publicValues, info);
		}
	}
}
