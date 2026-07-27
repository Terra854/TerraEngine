// AnimationEvents.cs - copied, filtered managed animation event delivery.

using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;

namespace TerraEngine.Animation
{
	public enum AnimationEventPayloadType : uint
	{
		None = 0,
		String = 1,
		Integer = 2,
		Float = 3,
		Boolean = 4
	}

	[Flags]
	public enum AnimationEventPayloadCategory : uint
	{
		None = 1U << 0,
		String = 1U << 1,
		Integer = 1U << 2,
		Float = 1U << 3,
		Boolean = 1U << 4,
		All = None | String | Integer | Float | Boolean
	}

	public enum AnimationPlaybackDirection : int
	{
		Reverse = -1,
		Stationary = 0,
		Forward = 1
	}

	/// <summary>One immutable event copied from a complete native publication.</summary>
	public readonly struct AnimationEvent
	{
		internal AnimationEvent(NativeAnimationEvent value, ulong publicationGeneration)
		{
			Animator = value.Animator;
			Clip = new AnimationClipAsset(new AnimationAssetReference(
				value.ClipAssetId, value.ClipGeneration, AnimationAssetKind.Clip));
			Controller = new AnimationControllerAsset(new AnimationAssetReference(
				value.ControllerAssetId, value.ControllerGeneration, AnimationAssetKind.Controller));
			ControllerStateId = value.ControllerStateId;
			SourceStateId = value.SourceStateId;
			EventId = value.EventId;
			SourceTick = value.SourceTick;
			LoopCount = value.LoopCount;
			LayerIndex = value.LayerIndex;
			PayloadType = (AnimationEventPayloadType)value.PayloadType;
			Direction = (AnimationPlaybackDirection)value.Direction;
			Historical = value.Historical != 0;
			PoseGeneration = value.PoseGeneration;
			ProducerOrder = value.ProducerOrder;
			CorrelationId = value.CorrelationId;
			Tag = value.ReadTag();
			Payload = value.ReadPayload();
			PublicationGeneration = value.PublicationGeneration;
		}

		public AnimatorHandle Animator { get; }
		public AnimationClipAsset Clip { get; }
		public AnimationControllerAsset Controller { get; }
		public ulong ControllerStateId { get; }
		public ulong SourceStateId { get; }
		public ulong EventId { get; }
		public long SourceTick { get; }
		public long LoopCount { get; }
		public uint LayerIndex { get; }
		public AnimationEventPayloadType PayloadType { get; }
		public AnimationPlaybackDirection Direction { get; }
		public bool Historical { get; }
		public uint PoseGeneration { get; }
		public ulong ProducerOrder { get; }
		public ulong CorrelationId { get; }
		public string Tag { get; }
		public string Payload { get; }
		public ulong PublicationGeneration { get; }

		public bool TryGetInteger(out int value)
		{
			value = default;
			return PayloadType == AnimationEventPayloadType.Integer
				&& int.TryParse(Payload, System.Globalization.NumberStyles.Integer,
					System.Globalization.CultureInfo.InvariantCulture, out value);
		}

		public bool TryGetFloat(out float value)
		{
			value = default;
			return PayloadType == AnimationEventPayloadType.Float
				&& float.TryParse(Payload, System.Globalization.NumberStyles.Float,
					System.Globalization.CultureInfo.InvariantCulture, out value);
		}

		public bool TryGetBoolean(out bool value)
		{
			value = default;
			return PayloadType == AnimationEventPayloadType.Boolean && bool.TryParse(Payload, out value);
		}
	}

	/// <summary>Optional selectors; omitted fields act as wildcards.</summary>
	public readonly struct AnimationEventFilter
	{
		public const uint AnyLayer = uint.MaxValue;

		public AnimationEventFilter(
			AnimatorHandle animator = default,
			AnimationAssetReference controller = default,
			ulong stateId = 0,
			uint layerIndex = AnyLayer,
			ulong eventId = 0,
			string? tag = null,
			AnimationEventPayloadCategory payloadCategories = AnimationEventPayloadCategory.All)
		{
			Animator = animator;
			Controller = controller;
			StateId = stateId;
			LayerIndex = layerIndex;
			EventId = eventId;
			Tag = tag ?? string.Empty;
			PayloadCategories = payloadCategories;
		}

		public AnimatorHandle Animator { get; }
		public AnimationAssetReference Controller { get; }
		public ulong StateId { get; }
		public uint LayerIndex { get; }
		public ulong EventId { get; }
		public string Tag { get; }
		public AnimationEventPayloadCategory PayloadCategories { get; }
		// A default struct bypasses optional constructor arguments. Spell out the
		// wildcard values so the advertised All filter really accepts every layer
		// and payload category.
		public static AnimationEventFilter All => new(
			layerIndex: AnyLayer,
			payloadCategories: AnimationEventPayloadCategory.All);

		internal bool Matches(in AnimationEvent value)
		{
			if (Animator.IsValid && value.Animator != Animator) return false;
			if (Controller.Id.IsValid)
			{
				AnimationAssetReference actual = value.Controller.Reference;
				if (actual.Id != Controller.Id) return false;
				if (Controller.Generation != 0 && actual.Generation != Controller.Generation) return false;
			}
			if (StateId != 0 && value.SourceStateId != StateId) return false;
			if (LayerIndex != AnyLayer && value.LayerIndex != LayerIndex) return false;
			if (EventId != 0 && value.EventId != EventId) return false;
			if (!string.IsNullOrEmpty(Tag) && !string.Equals(Tag, value.Tag, StringComparison.Ordinal)) return false;
			AnimationEventPayloadCategory category = value.PayloadType switch
			{
				AnimationEventPayloadType.None => AnimationEventPayloadCategory.None,
				AnimationEventPayloadType.String => AnimationEventPayloadCategory.String,
				AnimationEventPayloadType.Integer => AnimationEventPayloadCategory.Integer,
				AnimationEventPayloadType.Float => AnimationEventPayloadCategory.Float,
				AnimationEventPayloadType.Boolean => AnimationEventPayloadCategory.Boolean,
				_ => 0
			};
			return (PayloadCategories & category) != 0;
		}
	}

	/// <summary>A reload-safe deterministic subscription token.</summary>
	public sealed class AnimationEventSubscription : IDisposable
	{
		private int _disposed;
		internal ulong Id;
		internal ulong DomainGeneration;
		internal AnimationStatus CreationStatus;

		public AnimationStatus Status => CreationStatus;
		public bool IsActive => Volatile.Read(ref _disposed) == 0
			&& AnimationEvents.IsSubscriptionActive(Id, DomainGeneration);

		public void Dispose()
		{
			if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
			AnimationEvents.Release(this);
		}

		internal void InvalidateForReload()
		{
			Interlocked.Exchange(ref _disposed, 1);
			Id = 0;
		}
	}

	/// <summary>Protected-boundary subscription and copied batch dispatcher.</summary>
	public static class AnimationEvents
	{
		internal const int MaximumManagedBatchEvents = 4096;
		internal const int MaximumSubscriptions = 4096;

		private sealed class SubscriptionEntry
		{
			internal AnimationEventSubscription Token = null!;
			internal AnimationEventFilter Filter;
			internal Action<AnimationEvent> Callback = null!;
			internal ulong Sequence;
		}

		private static readonly object s_subscriptionLock = new();
		private static readonly List<SubscriptionEntry> s_subscriptions = new();
		private static ulong s_domainGeneration = 1;
		private static ulong s_nextSubscriptionId = 1;
		private static ulong s_nextSubscriptionSequence = 1;

		public static AnimationEventSubscription Subscribe(
			Action<AnimationEvent> callback,
			AnimationEventFilter? filter = null)
		{
			if (callback == null) throw new ArgumentNullException(nameof(callback));
			lock (s_subscriptionLock)
			{
				if (s_subscriptions.Count >= MaximumSubscriptions)
				{
					Log.Error($"Animation event subscription limit ({MaximumSubscriptions}) was reached; the subscription was rejected.");
					return new AnimationEventSubscription { CreationStatus = AnimationStatus.Rejected };
				}

				ulong id = s_nextSubscriptionId++;
				ulong sequence = s_nextSubscriptionSequence++;
				if (id == 0 || sequence == 0)
				{
					// A wrapped identity could alias an ancient token. Fail closed instead
					// of recycling it while this process remains alive.
					return new AnimationEventSubscription { CreationStatus = AnimationStatus.InternalError };
				}

				AnimationEventSubscription token = new()
				{
					Id = id,
					DomainGeneration = s_domainGeneration,
					CreationStatus = AnimationStatus.Ok
				};
				s_subscriptions.Add(new SubscriptionEntry
				{
					Token = token,
					Filter = filter ?? AnimationEventFilter.All,
					Callback = callback,
					Sequence = sequence
				});
				return token;
			}
		}

		internal static bool IsSubscriptionActive(ulong id, ulong domainGeneration)
		{
			if (id == 0) return false;
			lock (s_subscriptionLock)
			{
				if (domainGeneration != s_domainGeneration) return false;
				return s_subscriptions.Exists(entry => entry.Token.Id == id
					&& entry.Token.DomainGeneration == domainGeneration);
			}
		}

		internal static void Release(AnimationEventSubscription subscription)
		{
			lock (s_subscriptionLock)
			{
				s_subscriptions.RemoveAll(entry => ReferenceEquals(entry.Token, subscription));
				subscription.Id = 0;
			}
		}

		// Gameplay reload and Play Mode stop call this before the collectible
		// assembly unloads, so no delegate can keep an old domain alive.
		internal static void ResetManagedSubscriptions()
		{
			AnimationEventSubscription[] tokens;
			lock (s_subscriptionLock)
			{
				tokens = new AnimationEventSubscription[s_subscriptions.Count];
				for (int index = 0; index < tokens.Length; ++index) tokens[index] = s_subscriptions[index].Token;
				s_subscriptions.Clear();
				++s_domainGeneration;
				if (s_domainGeneration == 0) s_domainGeneration = 1;
			}
			foreach (AnimationEventSubscription token in tokens) token.InvalidateForReload();
		}

		internal static void DispatchProtected(AnimationEvent[] events)
		{
			if (events.Length == 0) return;
			Array.Sort(events, CompareEvents);

			SubscriptionEntry[] subscribers;
			lock (s_subscriptionLock) subscribers = s_subscriptions.ToArray();
			foreach (AnimationEvent animationEvent in events)
			{
				foreach (SubscriptionEntry subscriber in subscribers)
				{
					if (!subscriber.Token.IsActive || !subscriber.Filter.Matches(animationEvent)) continue;
					foreach (Action<AnimationEvent> callback in subscriber.Callback.GetInvocationList())
					{
						try
						{
							callback(animationEvent);
						}
						catch (Exception exception)
						{
							// Exceptions never unwind through ScriptingSystem or prevent a later
							// animator/subscriber from receiving the immutable publication.
							Log.Error($"Animation event callback was isolated (subscription={subscriber.Token.Id}, correlation={animationEvent.CorrelationId}): {exception}");
						}
					}
				}
			}
		}

		private static int CompareEvents(AnimationEvent left, AnimationEvent right)
		{
			int comparison = left.SourceTick.CompareTo(right.SourceTick);
			if (comparison != 0) return comparison;
			comparison = left.Animator.SlotIndex.CompareTo(right.Animator.SlotIndex);
			if (comparison != 0) return comparison;
			comparison = left.Animator.Generation.CompareTo(right.Animator.Generation);
			if (comparison != 0) return comparison;
			comparison = left.LayerIndex.CompareTo(right.LayerIndex);
			if (comparison != 0) return comparison;
			comparison = left.ProducerOrder.CompareTo(right.ProducerOrder);
			if (comparison != 0) return comparison;
			comparison = left.EventId.CompareTo(right.EventId);
			return comparison != 0 ? comparison : left.CorrelationId.CompareTo(right.CorrelationId);
		}

		[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
		public static unsafe void DispatchProtectedNative(
			void* events,
			uint eventCount,
			long sourceTick,
			ulong publicationGeneration,
			uint overflowed,
			uint droppedCountLowerBound)
		{
			try
			{
				DispatchProtectedNativeCore(events, eventCount, sourceTick, publicationGeneration,
					overflowed, droppedCountLowerBound);
			}
			catch (Exception exception)
			{
				// No managed exception may cross an unmanaged callback frame.
				Log.Error($"Animation event batch dispatch failed safely: {exception}");
			}
		}

		// Export glue may call this ordinary core because managed code cannot invoke
		// an UnmanagedCallersOnly method directly.
		public static unsafe void DispatchProtectedNativeCore(
			void* events,
			uint eventCount,
			long sourceTick,
			ulong publicationGeneration,
			uint overflowed,
			uint droppedCountLowerBound)
		{
			if (overflowed != 0)
			{
				Log.Error($"Animation event queue overflowed at tick {sourceTick}; at least {droppedCountLowerBound} deterministic records were dropped.");
			}
			if (eventCount == 0) return;
			if (events == null || eventCount > MaximumManagedBatchEvents)
			{
				Log.Error("Animation event packet exceeded the managed hard limit or used null storage and was rejected atomically.");
				return;
			}

			NativeAnimationEvent* source = (NativeAnimationEvent*)events;
			AnimationEvent[] copied = new AnimationEvent[eventCount];
			for (uint index = 0; index < eventCount; ++index)
			{
				NativeAnimationEvent item = source[index];
				if (item.Header.AbiVersion != AnimationAbiHeader.Version
					|| item.Header.Size < (uint)Marshal.SizeOf<NativeAnimationEvent>()
					|| item.Header.Kind != AnimationRecordKind.Event
					|| !item.Animator.IsValid
					|| item.EventId == 0
					|| item.PublicationGeneration != publicationGeneration
					|| item.PayloadType > (uint)AnimationEventPayloadType.Boolean
					|| item.Direction < (int)AnimationPlaybackDirection.Reverse
					|| item.Direction > (int)AnimationPlaybackDirection.Forward)
				{
					Log.Error("Animation event packet contained stale or incompatible records and was rejected atomically.");
					return;
				}
				copied[index] = new AnimationEvent(item, publicationGeneration);
			}
			DispatchProtected(copied);
		}
	}
}
