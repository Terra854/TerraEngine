// Rigidbody.cs - generation-safe managed rigid body command façade.

using System;
using System.Threading;

namespace TerraEngine
{
	public sealed class Rigidbody : Component
	{
		// Rigidbodies use producer 1 at the native boundary. A process-wide sequence
		// prevents separate managed bodies from reusing a queue ordering key while
		// preserving the dedicated character-controller producer lane.
		private const ulong ManagedRigidbodyProducerId = 1UL;
		private static long s_managedProducerSequence;
		private static readonly object s_submissionGate = new();

		// The engine fills this identity when a component is resolved. Scripts can
		// retain it, but every native call revalidates it at the protected boundary.
		public PhysicsBodyHandle Handle { get; internal set; }
		public PhysicsIdentity Identity { get; internal set; }
		public uint SchemaVersion => GetProperty<uint>("schemaVersion");

		public PhysicsMotionType MotionType
		{
			get => (PhysicsMotionType)GetProperty<uint>("motionType");
			set => SetProperty("motionType", (uint)value);
		}

		public PhysicsMassMode MassMode
		{
			get => (PhysicsMassMode)GetProperty<uint>("massMode");
			set => SetProperty("massMode", (uint)value);
		}

		public float Mass
		{
			get => GetProperty<float>("mass");
			set => SetProperty("mass", value);
		}

		public float LinearDamping
		{
			get => GetProperty<float>("linearDamping");
			set => SetProperty("linearDamping", value);
		}

		public float AngularDamping
		{
			get => GetProperty<float>("angularDamping");
			set => SetProperty("angularDamping", value);
		}

		public bool UseGravity
		{
			get => GetProperty<bool>("useGravity");
			set => SetProperty("useGravity", value);
		}

		public bool AllowSleeping
		{
			get => GetProperty<bool>("allowSleeping");
			set => SetProperty("allowSleeping", value);
		}

		public bool StartAwake
		{
			get => GetProperty<bool>("startAwake");
			set => SetProperty("startAwake", value);
		}

		public PhysicsMotionQuality ContinuousCollisionMode
		{
			get => (PhysicsMotionQuality)GetProperty<uint>("continuousCollisionMode");
			set => SetProperty("continuousCollisionMode", (uint)value);
		}

		public Vector3 InitialLinearVelocity
		{
			get => GetProperty<Vector3>("initialLinearVelocity");
			set => SetProperty("initialLinearVelocity", value);
		}

		public Vector3 InitialAngularVelocity
		{
			get => GetProperty<Vector3>("initialAngularVelocity");
			set => SetProperty("initialAngularVelocity", value);
		}

		public float MaxLinearVelocity
		{
			get => GetProperty<float>("maxLinearVelocity");
			set => SetProperty("maxLinearVelocity", value);
		}

		public float MaxAngularVelocity
		{
			get => GetProperty<float>("maxAngularVelocity");
			set => SetProperty("maxAngularVelocity", value);
		}

		public PhysicsDegreesOfFreedom AllowedDegreesOfFreedom
		{
			get => (PhysicsDegreesOfFreedom)GetProperty<uint>("allowedDegreesOfFreedom");
			set => SetProperty("allowedDegreesOfFreedom", (uint)value);
		}

		public PhysicsInterpolationMode InterpolationMode
		{
			get => (PhysicsInterpolationMode)GetProperty<uint>("interpolationMode");
			set => SetProperty("interpolationMode", (uint)value);
		}

		public uint CollisionLayer
		{
			get => GetProperty<uint>("collisionLayerId");
			set => SetProperty("collisionLayerId", value);
		}

		public bool UseCollisionMaskOverride
		{
			get => GetProperty<bool>("useCollisionMaskOverride");
			set => SetProperty("useCollisionMaskOverride", value);
		}

		public uint CollisionMaskOverride
		{
			get => GetProperty<uint>("collisionMaskOverride");
			set => SetProperty("collisionMaskOverride", value);
		}

		// PhysicsEnabled controls body creation independently from the common
		// Component.Enabled flag used by system-level component filtering.
		public bool PhysicsEnabled
		{
			get => GetProperty<bool>("enabled");
			set => SetProperty("enabled", value);
		}

		public PhysicsCommandReceipt AddForce(Vector3 value, ForceMode mode = ForceMode.Force, ulong effectiveTick = 0, ulong correlationId = 0)
		{
			return Submit(mode switch
			{
				ForceMode.Acceleration => NativePhysicsBodyCommandKind.Acceleration,
				ForceMode.Impulse => NativePhysicsBodyCommandKind.Impulse,
				ForceMode.VelocityChange => NativePhysicsBodyCommandKind.VelocityChange,
				_ => NativePhysicsBodyCommandKind.Force
			}, value, default, default, default, 0, effectiveTick, correlationId);
		}

		public PhysicsCommandReceipt AddImpulseAtPoint(Vector3 impulse, Vector3 point, ulong effectiveTick = 0, ulong correlationId = 0) => Submit(NativePhysicsBodyCommandKind.PointImpulse, impulse, point, default, default, 0, effectiveTick, correlationId);
		public PhysicsCommandReceipt SetLinearVelocity(Vector3 linearVelocity, ulong effectiveTick = 0, ulong correlationId = 0) => Submit(NativePhysicsBodyCommandKind.SetVelocity, linearVelocity, default, default, default, 0, effectiveTick, correlationId);
		public PhysicsCommandReceipt SetAngularVelocity(Vector3 angularVelocity, ulong effectiveTick = 0, ulong correlationId = 0) => Submit(NativePhysicsBodyCommandKind.SetVelocity, default, default, default, angularVelocity, 0, effectiveTick, correlationId);
		public PhysicsCommandReceipt Teleport(PhysicsPose pose, ulong effectiveTick = 0, ulong correlationId = 0) => Submit(NativePhysicsBodyCommandKind.Teleport, default, default, pose, default, 0, effectiveTick, correlationId);
		public PhysicsCommandReceipt MoveKinematic(PhysicsPose pose, float duration, ulong effectiveTick = 0, ulong correlationId = 0) => Submit(NativePhysicsBodyCommandKind.KinematicTarget, default, default, pose, default, duration, effectiveTick, correlationId);
		public PhysicsCommandReceipt WakeUp(ulong effectiveTick = 0, ulong correlationId = 0) => Submit(NativePhysicsBodyCommandKind.Wake, default, default, default, default, 0, effectiveTick, correlationId);
		public PhysicsCommandReceipt Sleep(ulong effectiveTick = 0, ulong correlationId = 0) => Submit(NativePhysicsBodyCommandKind.Sleep, default, default, default, default, 0, effectiveTick, correlationId);

		private PhysicsCommandReceipt Submit(NativePhysicsBodyCommandKind kind, Vector3 value, Vector3 point, PhysicsPose pose, Vector3 angularValue, float duration, ulong effectiveTick, ulong correlationId)
		{
			// A component added during the current frame may not have had a physics
			// publication when its wrapper was constructed, so resolve again here.
			if (!Handle.IsValid) Physics.BindComponent(this, Object);
			// The native queue admits only increasing producer sequences. Keep sequence
			// reservation and this immediate enqueue together, without locking component
			// binding or the deferred physics work that follows the protected boundary.
			lock (s_submissionGate)
			{
				ulong producerSequence = NextManagedProducerSequence();
				// A zero caller correlation means "unspecified". Use the unique producer
				// sequence as trace evidence while preserving caller-provided correlations.
				ulong resolvedCorrelation = correlationId == 0 ? producerSequence : correlationId;
				NativePhysicsBodyCommandRequest request = new()
				{
					Header = PhysicsAbiHeader.For<NativePhysicsBodyCommandRequest>(), Body = Handle, Identity = Identity, Kind = kind,
					Value = value, Point = point, Pose = pose, AngularValue = angularValue, Duration = duration,
					EffectiveTick = effectiveTick, ProducerId = ManagedRigidbodyProducerId, ProducerSequence = producerSequence, CorrelationId = resolvedCorrelation
				};
				NativePhysicsCommandReceipt receipt = new() { Header = PhysicsAbiHeader.For<NativePhysicsCommandReceipt>() };
				InternalCalls.Native_Physics_SubmitBodyCommand(ref request, ref receipt);
				return new PhysicsCommandReceipt(receipt);
			}
		}

		private static ulong NextManagedProducerSequence()
		{
			// The submission gate preserves arrival order; Interlocked reserves a
			// unique value. Skip the invalid zero value if the signed counter wraps.
			long sequence = Interlocked.Increment(ref s_managedProducerSequence);
			if (sequence == 0) sequence = Interlocked.Increment(ref s_managedProducerSequence);
			return unchecked((ulong)sequence);
		}
	}
}
