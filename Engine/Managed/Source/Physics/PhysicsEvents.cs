// PhysicsEvents.cs - protected-boundary managed event subscriptions.

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Collections.Generic;

namespace TerraEngine
{
	public sealed class PhysicsEventSubscription : IDisposable
	{
		internal NativePhysicsEventSubscription Native;
		public PhysicsStatus Status => Native.Status;
		public ulong Id => Native.Id;
		public void Dispose() => PhysicsEvents.Release(this);
	}

	public static class PhysicsEvents
	{
		private static readonly object s_subscriptionLock = new();
		private static readonly List<PhysicsEventSubscription> s_subscriptions = new();
		public static event Action<PhysicsEvent>? Published;

		public static PhysicsEventSubscription Subscribe(uint categoryMask = uint.MaxValue, uint collisionLayerMask = uint.MaxValue, bool includeSensors = true, bool includeContactDetails = true)
		{
			NativePhysicsEventSubscriptionRequest request = new() { Header = PhysicsAbiHeader.For<NativePhysicsEventSubscriptionRequest>(), CategoryMask = categoryMask, CollisionLayerMask = collisionLayerMask, IncludeSensors = includeSensors ? (byte)1 : (byte)0, IncludeContactDetails = includeContactDetails ? (byte)1 : (byte)0 };
			NativePhysicsEventSubscription subscription = new() { Header = PhysicsAbiHeader.For<NativePhysicsEventSubscription>() };
			InternalCalls.Native_Physics_SubscribeEvents(ref request, ref subscription);
			PhysicsEventSubscription result = new() { Native = subscription };
			if (subscription.Id != 0)
			{
				lock (s_subscriptionLock) s_subscriptions.Add(result);
			}
			return result;
		}

		internal static void Release(PhysicsEventSubscription subscription)
		{
			lock (s_subscriptionLock) s_subscriptions.Remove(subscription);
			if (subscription.Native.Id == 0) return;
			InternalCalls.Native_Physics_UnsubscribeEvents(ref subscription.Native);
			subscription.Native.Id = 0;
		}

		// Gameplay reload and Play Mode stop clear delegates before unloading the
		// collectible assembly, then release every native filter generation.
		internal static void ResetManagedSubscriptions()
		{
			PhysicsEventSubscription[] subscriptions;
			lock (s_subscriptionLock)
			{
				subscriptions = s_subscriptions.ToArray();
				s_subscriptions.Clear();
				Published = null;
			}
			foreach (PhysicsEventSubscription subscription in subscriptions)
			{
				if (subscription.Native.Id == 0) continue;
				InternalCalls.Native_Physics_UnsubscribeEvents(ref subscription.Native);
				subscription.Native.Id = 0;
			}
		}

		// ScriptingSystem calls this only at its protected post-physics boundary.
		// One throwing subscriber is isolated so later subscribers still observe the packet.
		internal static void DispatchProtected(PhysicsEvent[] events)
		{
			Action<PhysicsEvent>? subscribers = Published;
			if (subscribers == null) return;
			foreach (PhysicsEvent physicsEvent in events)
			{
				foreach (Action<PhysicsEvent> callback in subscribers.GetInvocationList())
				{
					try { callback(physicsEvent); }
					catch (Exception exception) { Log.Error($"Physics event callback was isolated: {exception.Message}"); }
				}
			}
		}

		// hostfxr and NativeAOT both enter through this fixed-width copy boundary.
		// No managed subscriber can retain native packet memory after this returns.
		[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
		public static unsafe void DispatchProtectedNative(void* events, uint eventCount, ulong worldId, uint worldGeneration, ulong tick)
		{
			try
			{
				DispatchProtectedNativeCore(events, eventCount, worldId, worldGeneration, tick);
			}
			catch (Exception exception)
			{
				// No managed exception may unwind through an unmanaged callback frame.
				Log.Error($"Physics event batch dispatch failed safely: {exception.Message}");
			}
		}

		// The generated NativeAOT bootstrap cannot call a method decorated with
		// UnmanagedCallersOnly directly, so it forwards to this ordinary managed core.
		public static unsafe void DispatchProtectedNativeCore(void* events, uint eventCount, ulong worldId, uint worldGeneration, ulong tick)
		{
			if (eventCount == 0 || events == null) return;
			if (eventCount > 4096)
			{
				Log.Error("Physics event packet exceeded the managed hard limit and was rejected.");
				return;
			}

			NativePhysicsEvent* source = (NativePhysicsEvent*)events;
			PhysicsEvent[] copied = new PhysicsEvent[eventCount];
			for (uint index = 0; index < eventCount; ++index)
			{
				NativePhysicsEvent item = source[index];
				if (item.Header.AbiVersion != PhysicsAbiHeader.Version
					|| item.Header.StructSize < (uint)Marshal.SizeOf<NativePhysicsEvent>()
					|| item.World.Value != worldId || item.World.Generation != worldGeneration
					|| item.Tick != tick)
				{
					Log.Error("Physics event packet contained stale or incompatible records and was rejected atomically.");
					return;
				}
				copied[index] = item.ToPublic();
			}
			DispatchProtected(copied);
		}
	}
}
