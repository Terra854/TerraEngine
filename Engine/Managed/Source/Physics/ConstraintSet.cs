// ConstraintSet.cs - copied, atomic constraint collection authoring for managed scripts.

using System;

namespace TerraEngine
{
	public enum PhysicsConstraintKind : uint { Fixed, Point, Distance, Hinge, Slider, Cone, SwingTwist, SixDegreesOfFreedom, Gear }
	public enum PhysicsConstraintEndpointKind : uint { ConnectedObject, WorldAnchor }
	public enum PhysicsConstraintMotorMode : uint { Disabled, Position, Velocity }

	public struct PhysicsConstraintLimits
	{
		public bool Enabled;
		public float Minimum;
		public float Maximum;
	}

	public struct PhysicsConstraintSpring
	{
		public bool Enabled;
		public float Stiffness;
		public float Damping;
	}

	public struct PhysicsConstraintMotor
	{
		public PhysicsConstraintMotorMode Mode;
		public float TargetPosition;
		public float TargetVelocity;
		public float MaximumForce;
	}

	public struct PhysicsConstraintAxisSettings
	{
		public PhysicsConstraintLimits Limits;
		public PhysicsConstraintSpring Spring;
		public PhysicsConstraintMotor Motor;
	}

	public struct ConstraintDefinition
	{
		public PhysicsUuid RecordId;
		public uint RecordVersion;
		public PhysicsConstraintKind Kind;
		public PhysicsConstraintEndpointKind FirstEndpointKind;
		public PhysicsUuid FirstObjectId;
		public PhysicsConstraintEndpointKind SecondEndpointKind;
		public PhysicsUuid SecondObjectId;
		public PhysicsPose FirstFrame;
		public PhysicsPose SecondFrame;
		public Vector3 FirstAxis;
		public Vector3 SecondAxis;
		public PhysicsConstraintLimits Limits;
		public PhysicsConstraintSpring Spring;
		public PhysicsConstraintMotor Motor;
		public PhysicsConstraintAxisSettings TranslationX;
		public PhysicsConstraintAxisSettings TranslationY;
		public PhysicsConstraintAxisSettings TranslationZ;
		public PhysicsConstraintAxisSettings RotationX;
		public PhysicsConstraintAxisSettings RotationY;
		public PhysicsConstraintAxisSettings RotationZ;
		public PhysicsUuid FirstGearConstraintId;
		public PhysicsUuid SecondGearConstraintId;
		public float GearRatio;
		public bool CollideConnected;
		public float BreakForce;
		public float BreakTorque;
		public bool Enabled;
		public uint ResetGeneration;

		public static ConstraintDefinition BetweenObjects(PhysicsUuid firstObjectId, PhysicsUuid secondObjectId, PhysicsConstraintKind kind = PhysicsConstraintKind.Fixed)
		{
			ConstraintDefinition value = Defaults(kind);
			value.FirstEndpointKind = PhysicsConstraintEndpointKind.ConnectedObject;
			value.FirstObjectId = firstObjectId;
			value.SecondEndpointKind = PhysicsConstraintEndpointKind.ConnectedObject;
			value.SecondObjectId = secondObjectId;
			return value;
		}

		public static ConstraintDefinition ToWorld(PhysicsUuid objectId, PhysicsConstraintKind kind = PhysicsConstraintKind.Fixed)
		{
			ConstraintDefinition value = Defaults(kind);
			value.FirstEndpointKind = PhysicsConstraintEndpointKind.ConnectedObject;
			value.FirstObjectId = objectId;
			value.SecondEndpointKind = PhysicsConstraintEndpointKind.WorldAnchor;
			return value;
		}

		private static ConstraintDefinition Defaults(PhysicsConstraintKind kind) => new()
		{
			RecordId = PhysicsUuid.New(),
			RecordVersion = 1,
			Kind = kind,
			FirstFrame = new PhysicsPose { Rotation = Quaternion.Identity },
			SecondFrame = new PhysicsPose { Rotation = Quaternion.Identity },
			FirstAxis = new Vector3(0, 1, 0),
			SecondAxis = new Vector3(0, 1, 0),
			GearRatio = 1,
			BreakForce = float.PositiveInfinity,
			BreakTorque = float.PositiveInfinity,
			Enabled = true,
			ResetGeneration = 1
		};

		private static NativePhysicsConstraintLimits ToNative(PhysicsConstraintLimits value) => new() { Enabled = value.Enabled ? 1U : 0U, Minimum = value.Minimum, Maximum = value.Maximum };
		private static NativePhysicsConstraintSpring ToNative(PhysicsConstraintSpring value) => new() { Enabled = value.Enabled ? 1U : 0U, Stiffness = value.Stiffness, Damping = value.Damping };
		private static NativePhysicsConstraintMotor ToNative(PhysicsConstraintMotor value) => new() { Mode = value.Mode, TargetPosition = value.TargetPosition, TargetVelocity = value.TargetVelocity, MaximumForce = value.MaximumForce };
		private static NativePhysicsConstraintAxisSettings ToNative(PhysicsConstraintAxisSettings value) => new() { Limits = ToNative(value.Limits), Spring = ToNative(value.Spring), Motor = ToNative(value.Motor) };

		private static PhysicsConstraintLimits FromNative(NativePhysicsConstraintLimits value) => new() { Enabled = value.Enabled != 0, Minimum = value.Minimum, Maximum = value.Maximum };
		private static PhysicsConstraintSpring FromNative(NativePhysicsConstraintSpring value) => new() { Enabled = value.Enabled != 0, Stiffness = value.Stiffness, Damping = value.Damping };
		private static PhysicsConstraintMotor FromNative(NativePhysicsConstraintMotor value) => new() { Mode = value.Mode, TargetPosition = value.TargetPosition, TargetVelocity = value.TargetVelocity, MaximumForce = value.MaximumForce };
		private static PhysicsConstraintAxisSettings FromNative(NativePhysicsConstraintAxisSettings value) => new() { Limits = FromNative(value.Limits), Spring = FromNative(value.Spring), Motor = FromNative(value.Motor) };

		internal unsafe NativePhysicsConstraintRecord ToNative() => new()
		{
			Header = PhysicsAbiHeader.For<NativePhysicsConstraintRecord>(), RecordId = RecordId, RecordVersion = RecordVersion, Kind = Kind,
			FirstEndpointKind = FirstEndpointKind, SecondEndpointKind = SecondEndpointKind,
			FirstObjectId = FirstObjectId, SecondObjectId = SecondObjectId, FirstFrame = FirstFrame, SecondFrame = SecondFrame,
			FirstAxis = FirstAxis, SecondAxis = SecondAxis, Limits = ToNative(Limits), Spring = ToNative(Spring), Motor = ToNative(Motor),
			TranslationX = ToNative(TranslationX), TranslationY = ToNative(TranslationY), TranslationZ = ToNative(TranslationZ),
			RotationX = ToNative(RotationX), RotationY = ToNative(RotationY), RotationZ = ToNative(RotationZ),
			FirstGearConstraintId = FirstGearConstraintId, SecondGearConstraintId = SecondGearConstraintId, GearRatio = GearRatio,
			BreakForce = BreakForce, BreakTorque = BreakTorque, ResetGeneration = ResetGeneration,
			CollideConnected = CollideConnected ? (byte)1 : (byte)0, Enabled = Enabled ? (byte)1 : (byte)0
		};

		internal static unsafe ConstraintDefinition FromNative(NativePhysicsConstraintRecord value) => new()
		{
			RecordId = value.RecordId, RecordVersion = value.RecordVersion, Kind = value.Kind,
			FirstEndpointKind = value.FirstEndpointKind, SecondEndpointKind = value.SecondEndpointKind,
			FirstObjectId = value.FirstObjectId, SecondObjectId = value.SecondObjectId,
			FirstFrame = value.FirstFrame, SecondFrame = value.SecondFrame, FirstAxis = value.FirstAxis, SecondAxis = value.SecondAxis,
			Limits = FromNative(value.Limits), Spring = FromNative(value.Spring), Motor = FromNative(value.Motor),
			TranslationX = FromNative(value.TranslationX), TranslationY = FromNative(value.TranslationY), TranslationZ = FromNative(value.TranslationZ),
			RotationX = FromNative(value.RotationX), RotationY = FromNative(value.RotationY), RotationZ = FromNative(value.RotationZ),
			FirstGearConstraintId = value.FirstGearConstraintId, SecondGearConstraintId = value.SecondGearConstraintId,
			GearRatio = value.GearRatio, BreakForce = value.BreakForce, BreakTorque = value.BreakTorque,
			ResetGeneration = value.ResetGeneration, CollideConnected = value.CollideConnected != 0, Enabled = value.Enabled != 0
		};
	}

	public sealed class ConstraintSet : Component
	{
		public PhysicsIdentity Identity { get; internal set; }
		public ConstraintRecord GetRecord(PhysicsConstraintHandle handle, PhysicsUuid recordId, uint recordGeneration) => new(handle, Identity, recordId, recordGeneration);

		public ConstraintDefinition[] Records
		{
			get
			{
				PhysicsStatus status = TryGetRecords(out ConstraintDefinition[] records);
				if (status != PhysicsStatus.Success) throw new InvalidOperationException($"Could not read ConstraintSet records: {status}.");
				return records;
			}
			set
			{
				PhysicsStatus status = ReplaceRecords(value ?? Array.Empty<ConstraintDefinition>());
				if (status != PhysicsStatus.Success && status != PhysicsStatus.Accepted)
					throw new ArgumentException($"Could not replace ConstraintSet records: {status}.", nameof(value));
			}
		}

		public unsafe PhysicsStatus TryGetRecords(out ConstraintDefinition[] records)
		{
			records = Array.Empty<ConstraintDefinition>();
			PhysicsStatus status = InternalCalls.Native_Physics_CopyConstraintRecords(Object.ID, null, 0, out uint count);
			if (status != PhysicsStatus.Success && status != PhysicsStatus.LimitExceeded) return status;
			if (count == 0) return PhysicsStatus.Success;
			NativePhysicsConstraintRecord[] native = new NativePhysicsConstraintRecord[count];
			fixed (NativePhysicsConstraintRecord* pointer = native)
				status = InternalCalls.Native_Physics_CopyConstraintRecords(Object.ID, pointer, count, out _);
			if (status != PhysicsStatus.Success) return status;
			records = new ConstraintDefinition[count];
			for (int index = 0; index < records.Length; ++index) records[index] = ConstraintDefinition.FromNative(native[index]);
			return PhysicsStatus.Success;
		}

		public unsafe PhysicsStatus ReplaceRecords(ReadOnlySpan<ConstraintDefinition> records)
		{
			NativePhysicsConstraintRecord[] native = new NativePhysicsConstraintRecord[records.Length];
			for (int index = 0; index < records.Length; ++index) native[index] = records[index].ToNative();
			if (native.Length == 0) return InternalCalls.Native_Physics_ReplaceConstraintRecords(Object.ID, null, 0);
			fixed (NativePhysicsConstraintRecord* pointer = native)
				return InternalCalls.Native_Physics_ReplaceConstraintRecords(Object.ID, pointer, (uint)native.Length);
		}
	}

	public readonly struct ConstraintRecord
	{
		public ConstraintRecord(PhysicsConstraintHandle handle, PhysicsIdentity owner, PhysicsUuid recordId, uint recordGeneration) { Handle = handle; Owner = owner; RecordId = recordId; RecordGeneration = recordGeneration; }
		public PhysicsConstraintHandle Handle { get; }
		public PhysicsIdentity Owner { get; }
		public PhysicsUuid RecordId { get; }
		public uint RecordGeneration { get; }
	}
}
