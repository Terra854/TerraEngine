// Rigidbody.cs - generation-safe managed rigid body command façade.

using System;

namespace TerraEngine
{
	public sealed class Rigidbody : Component
	{
		// The engine fills this identity when a component is resolved. Scripts can
		// retain it, but every native call revalidates it at the protected boundary.
		public PhysicsBodyHandle Handle { get; internal set; }
		public PhysicsIdentity Identity { get; internal set; }

		public bool UseGravity
		{
			get => GetProperty<bool>("useGravity");
			set => SetProperty("useGravity", value);
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
			NativePhysicsBodyCommandRequest request = new()
			{
				Header = PhysicsAbiHeader.For<NativePhysicsBodyCommandRequest>(), Body = Handle, Identity = Identity, Kind = kind,
				Value = value, Point = point, Pose = pose, AngularValue = angularValue, Duration = duration,
				EffectiveTick = effectiveTick, ProducerId = 1, ProducerSequence = correlationId == 0 ? 1UL : correlationId, CorrelationId = correlationId
			};
			NativePhysicsCommandReceipt receipt = new() { Header = PhysicsAbiHeader.For<NativePhysicsCommandReceipt>() };
			InternalCalls.Native_Physics_SubmitBodyCommand(ref request, ref receipt);
			return new PhysicsCommandReceipt(receipt);
		}
	}
}
