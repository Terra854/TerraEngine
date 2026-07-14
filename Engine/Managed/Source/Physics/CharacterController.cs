// CharacterController.cs - deferred, generation-safe character commands.

namespace TerraEngine
{
	public sealed class CharacterController : Component
	{
		public PhysicsCharacterHandle Handle { get; internal set; }
		public PhysicsIdentity Identity { get; internal set; }

		public PhysicsCommandReceipt SetDesiredVelocity(Vector3 velocity, ulong effectiveTick = 0, ulong correlationId = 0) => Submit(NativePhysicsCharacterCommandKind.Move, velocity, default, 0, effectiveTick, correlationId);
		public PhysicsCommandReceipt Jump(float speed, ulong effectiveTick = 0, ulong correlationId = 0) => Submit(NativePhysicsCharacterCommandKind.Jump, default, default, speed, effectiveTick, correlationId);
		public PhysicsCommandReceipt Resize(float height, ulong effectiveTick = 0, ulong correlationId = 0) => Submit(NativePhysicsCharacterCommandKind.Resize, default, default, height, effectiveTick, correlationId);
		public PhysicsCommandReceipt Teleport(PhysicsPose pose, ulong effectiveTick = 0, ulong correlationId = 0) => Submit(NativePhysicsCharacterCommandKind.Teleport, default, pose, 0, effectiveTick, correlationId);

		private PhysicsCommandReceipt Submit(NativePhysicsCharacterCommandKind kind, Vector3 velocity, PhysicsPose pose, float scalar, ulong effectiveTick, ulong correlationId)
		{
			if (!Handle.IsValid) Physics.BindComponent(this, Object);
			NativePhysicsCharacterCommandRequest request = new() { Header = PhysicsAbiHeader.For<NativePhysicsCharacterCommandRequest>(), Character = Handle, Identity = Identity, Kind = kind, DesiredVelocity = velocity, Pose = pose, Scalar = scalar, EffectiveTick = effectiveTick, ProducerId = 1, ProducerSequence = correlationId == 0 ? 1UL : correlationId, CorrelationId = correlationId };
			NativePhysicsCommandReceipt receipt = new() { Header = PhysicsAbiHeader.For<NativePhysicsCommandReceipt>() };
			InternalCalls.Native_Physics_SubmitCharacterCommand(ref request, ref receipt);
			return new PhysicsCommandReceipt(receipt);
		}
	}
}
