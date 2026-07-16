// CharacterController.cs - deferred, generation-safe character commands.

namespace TerraEngine
{
	public sealed class CharacterController : Component
	{
		public PhysicsCharacterHandle Handle { get; internal set; }
		public PhysicsIdentity Identity { get; internal set; }
		public uint SchemaVersion => GetProperty<uint>("schemaVersion");

		public PhysicsCharacterShape Shape
		{
			get => (PhysicsCharacterShape)GetProperty<uint>("shapeKind");
			set => SetProperty("shapeKind", (uint)value);
		}

		public float Radius { get => GetProperty<float>("radius"); set => SetProperty("radius", value); }
		public float Height { get => GetProperty<float>("height"); set => SetProperty("height", value); }
		public Vector3 LocalCenter { get => GetProperty<Vector3>("localCenter"); set => SetProperty("localCenter", value); }
		public Vector3 UpDirection { get => GetProperty<Vector3>("upDirection"); set => SetProperty("upDirection", value); }
		public float StepHeight { get => GetProperty<float>("stepHeight"); set => SetProperty("stepHeight", value); }
		public float SupportVolumeRadius { get => GetProperty<float>("supportVolumeRadius"); set => SetProperty("supportVolumeRadius", value); }
		public float SupportVolumeHeight { get => GetProperty<float>("supportVolumeHeight"); set => SetProperty("supportVolumeHeight", value); }
		public float MaximumSlopeAngleDegrees { get => GetProperty<float>("maximumSlopeAngleDegrees"); set => SetProperty("maximumSlopeAngleDegrees", value); }
		public float ContactPadding { get => GetProperty<float>("contactPadding"); set => SetProperty("contactPadding", value); }
		public float Mass { get => GetProperty<float>("mass"); set => SetProperty("mass", value); }
		public float MaximumStrength { get => GetProperty<float>("maximumStrength"); set => SetProperty("maximumStrength", value); }
		public uint CollisionLayer { get => GetProperty<uint>("collisionLayerId"); set => SetProperty("collisionLayerId", value); }
		public uint CollisionMask { get => GetProperty<uint>("collisionMask"); set => SetProperty("collisionMask", value); }
		public float GroundAdhesion { get => GetProperty<float>("groundAdhesion"); set => SetProperty("groundAdhesion", value); }
		public bool AllowStairWalking { get => GetProperty<bool>("allowStairWalking"); set => SetProperty("allowStairWalking", value); }
		public bool UseGravity { get => GetProperty<bool>("useGravity"); set => SetProperty("useGravity", value); }
		public float GravityScale { get => GetProperty<float>("gravityScale"); set => SetProperty("gravityScale", value); }
		public float JumpSpeed { get => GetProperty<float>("jumpSpeed"); set => SetProperty("jumpSpeed", value); }

		public PhysicsCharacterRotationPolicy RotationPolicy
		{
			get => (PhysicsCharacterRotationPolicy)GetProperty<uint>("rotationPolicy");
			set => SetProperty("rotationPolicy", (uint)value);
		}

		public bool PhysicsEnabled { get => GetProperty<bool>("enabled"); set => SetProperty("enabled", value); }

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
