// PhysicsTypes.cs - fixed-width managed physics values and ABI packet mirrors.
//
// These structs intentionally mirror Engine/Scripting/ScriptGlue.h. Every
// mutable request carries a version/size header so a stale script assembly
// cannot reinterpret native memory after a hot reload.

using System;
using System.Runtime.InteropServices;
using System.Text;

namespace TerraEngine
{
	public enum PhysicsStatus : uint { Success, Accepted, InvalidArgument, InvalidState, StaleTarget, LimitExceeded, Conflict, Cancelled, Expired, ProviderFailure, VersionMismatch }
	public enum ForceMode : uint { Force, Acceleration, Impulse, VelocityChange }
	public enum PhysicsQueryKind : uint { Raycast, ShapeCast, Overlap, Point }
	public enum PhysicsQueryHitMode : uint { Any, Closest, All }
	public enum PhysicsEventType : uint { CollisionEnter, CollisionStay, CollisionExit, Contact, TriggerEnter, TriggerStay, TriggerExit, Activated, Slept, ConstraintBroken }
	public enum PhysicsCharacterGroundState : uint { OnGround, OnSteepGround, NotSupported, InAir }
	public enum PhysicsMotionType : uint { Static, Dynamic, Kinematic }
	public enum PhysicsMassMode : uint { AutomaticFromDensity, ExplicitMass }
	public enum PhysicsMotionQuality : uint { Discrete, LinearCast }
	public enum PhysicsInterpolationMode : uint { None, Interpolate, Extrapolate }
	[Flags]
	public enum PhysicsDegreesOfFreedom : uint
	{
		None = 0,
		TranslateX = 1U << 0,
		TranslateY = 1U << 1,
		TranslateZ = 1U << 2,
		RotateX = 1U << 3,
		RotateY = 1U << 4,
		RotateZ = 1U << 5,
		All = TranslateX | TranslateY | TranslateZ | RotateX | RotateY | RotateZ
	}
	public enum PhysicsCharacterShape : uint { Capsule, Cylinder }
	public enum PhysicsCharacterRotationPolicy : uint { Free, AlignToUp, Locked }
	internal enum NativePhysicsBodyCommandKind : uint { Force, Acceleration, Impulse, PointImpulse, VelocityChange, SetVelocity, Teleport, KinematicTarget, Wake, Sleep }
	internal enum NativePhysicsCharacterCommandKind : uint { Move, Jump, Resize, Teleport, RefreshConfiguration }

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	public struct PhysicsAbiHeader
	{
		public const uint Version = 1;
		public uint AbiVersion;
		public uint StructSize;
		public static PhysicsAbiHeader For<T>() where T : struct => new() { AbiVersion = Version, StructSize = (uint)Marshal.SizeOf<T>() };
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	public struct PhysicsUuid
	{
		public ulong First;
		public ulong Second;
		public bool IsValid => First != 0 || Second != 0;

		// Authoring records need stable IDs before they cross into native scene
		// storage. Guid supplies 128 copied bits without borrowing engine memory.
		public static PhysicsUuid New()
		{
			byte[] bytes = Guid.NewGuid().ToByteArray();
			return new PhysicsUuid { First = BitConverter.ToUInt64(bytes, 0), Second = BitConverter.ToUInt64(bytes, 8) };
		}
	}
	[StructLayout(LayoutKind.Sequential, Pack = 8)] public struct PhysicsWorldId { public ulong Value; public uint Generation; private uint _padding; public bool IsValid => Value != 0 && Generation != 0; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] public struct PhysicsBodyHandle { public PhysicsWorldId World; public uint SlotIndex; public uint SlotGeneration; public bool IsValid => World.IsValid && SlotGeneration != 0 && SlotIndex != uint.MaxValue; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] public struct PhysicsConstraintHandle { public PhysicsWorldId World; public uint SlotIndex; public uint SlotGeneration; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] public struct PhysicsCharacterHandle { public PhysicsWorldId World; public uint SlotIndex; public uint SlotGeneration; public bool IsValid => World.IsValid && SlotGeneration != 0 && SlotIndex != uint.MaxValue; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] public struct PhysicsQueryHandle { public PhysicsWorldId World; public uint ResultSlot; public uint ResultGeneration; public ulong SubmittedTick; public ulong ExpiryTick; public bool IsValid => World.IsValid && ResultGeneration != 0 && ResultSlot != uint.MaxValue; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] public struct PhysicsPose { public Vector3 Position; public Quaternion Rotation; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] public struct PhysicsIdentity { public PhysicsUuid ObjectId; public uint ObjectGeneration; public uint ComponentGeneration; public PhysicsUuid RecordId; public uint RecordGeneration; public uint BindingGeneration; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] public struct PhysicsCommandReceipt { public PhysicsStatus Status; public ulong CommandId; public ulong EffectiveTick; public ulong CorrelationId; public string Diagnostic; public bool Accepted => Status == PhysicsStatus.Accepted || Status == PhysicsStatus.Success; internal PhysicsCommandReceipt(NativePhysicsCommandReceipt value) { Status = value.Status; CommandId = value.CommandId; EffectiveTick = value.EffectiveTick; CorrelationId = value.CorrelationId; Diagnostic = value.Diagnostic; } }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] public struct PhysicsQueryFilter { public uint CollisionMask; public uint BodyTypeMask; public bool IncludeSensors; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] public struct PhysicsQueryRequest { public PhysicsWorldId World; public PhysicsQueryKind Kind; public PhysicsQueryHitMode HitMode; public PhysicsPose Pose; public Vector3 Direction; public Vector3 ShapeExtent; public float Distance; public uint MaximumHits; public PhysicsQueryFilter Filter; public ulong SubmittedTick; public ulong ExpiryTick; public ulong CorrelationId; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] public struct PhysicsQueryHit { public PhysicsBodyHandle Body; public PhysicsUuid ObjectId; public uint ObjectGeneration; public uint ComponentGeneration; public PhysicsUuid MaterialId; public Vector3 Position; public Vector3 Normal; public float Distance; public float Fraction; public uint SubShapeKey; public byte CollisionLayer; public byte MotionType; public bool Sensor; }
	public sealed class PhysicsQueryResult { public PhysicsStatus Status { get; internal init; } public ulong Tick { get; internal init; } public ulong CorrelationId { get; internal init; } public bool Truncated { get; internal init; } public string Diagnostic { get; internal init; } = string.Empty; public PhysicsQueryHit[] Hits { get; internal init; } = Array.Empty<PhysicsQueryHit>(); }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] public struct PhysicsBodyState { public PhysicsBodyHandle Body; public PhysicsIdentity Identity; public PhysicsPose Pose; public Vector3 LinearVelocity; public Vector3 AngularVelocity; public ulong Tick; public bool Sleeping; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] public struct PhysicsCharacterState { public PhysicsCharacterHandle Character; public PhysicsIdentity Identity; public PhysicsPose Pose; public Vector3 LinearVelocity; public Vector3 GroundNormal; public Vector3 SupportVelocity; public PhysicsBodyHandle SupportBody; public ulong Tick; public PhysicsCharacterGroundState GroundState; public uint ContactCount; public bool Supported; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] public struct PhysicsEvent { public PhysicsEventType Type; public bool HasSecondParticipant; public PhysicsWorldId World; public ulong Tick; public PhysicsIdentity First; public PhysicsIdentity Second; public Vector3 Point; public Vector3 Normal; public Vector3 RelativeVelocity; public float Penetration; public float Impulse; public ulong CorrelationId; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] public struct PhysicsSettingsSnapshot { public PhysicsStatus Status; public uint SettingsGeneration; public Vector3 Gravity; public float FixedTimeStep; public uint MaxSubsteps; public uint MaxQueryBatchSize; public uint MaxHitsPerQuery; public uint MaxEventHistory; public PhysicsWorldId World; public string Diagnostic; }
	public struct PhysicsCollisionLayer
	{
		public uint Id;
		public string Name;
		public uint CollisionMask;
		public uint BroadPhaseGroup;
		public bool Enabled;
	}

	public sealed class PhysicsConfiguration
	{
		public PhysicsStatus Status { get; internal set; }
		public uint SettingsGeneration { get; internal set; }
		public Vector3 Gravity { get; set; }
		public float FixedTimeStep { get; set; }
		public uint MaxSubsteps { get; set; }
		public float MaxAccumulatedTime { get; set; }
		public uint VelocityIterations { get; set; }
		public uint PositionIterations { get; set; }
		public float MaxCastDistance { get; set; }
		public float MaxShapeExtent { get; set; }
		public uint MaxHitsPerQuery { get; set; }
		public uint MaxRequestsPerBatch { get; set; }
		public uint RetainedResultTicks { get; set; }
		public bool DebugVisualizationEnabled { get; set; }
		public uint RetainedDebugTicks { get; set; }
		public uint MaxDebugPrimitives { get; set; }
		public uint TransformPublicationChunkSize { get; set; }
		public uint ProviderExtractionBatchSize { get; set; }
		public uint ParallelPreparationThreshold { get; set; }
		public uint ParallelExtractionThreshold { get; set; }
		public uint AggregateDiagnosticsIntervalFrames { get; set; }
		public bool ContinuousCollisionEnabled { get; set; }
		public PhysicsWorldId World { get; internal set; }
		public string Diagnostic { get; internal set; } = string.Empty;
		public PhysicsCollisionLayer[] Layers { get; set; } = new PhysicsCollisionLayer[32];
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)] internal struct NativePhysicsBodyCommandRequest { public PhysicsAbiHeader Header; public PhysicsBodyHandle Body; public PhysicsIdentity Identity; public NativePhysicsBodyCommandKind Kind; private uint _reserved; public Vector3 Value; public Vector3 Point; public PhysicsPose Pose; public Vector3 AngularValue; public float Duration; public ulong EffectiveTick; public ulong ProducerId; public ulong ProducerSequence; public ulong CorrelationId; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] internal struct NativePhysicsCharacterCommandRequest { public PhysicsAbiHeader Header; public PhysicsCharacterHandle Character; public PhysicsIdentity Identity; public NativePhysicsCharacterCommandKind Kind; private uint _reserved; public Vector3 DesiredVelocity; public PhysicsPose Pose; public float Scalar; public ulong EffectiveTick; public ulong ProducerId; public ulong ProducerSequence; public ulong CorrelationId; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] internal unsafe struct NativePhysicsCommandReceipt { public PhysicsAbiHeader Header; public PhysicsStatus Status; private uint _reserved; public ulong CommandId; public ulong EffectiveTick; public ulong CorrelationId; public fixed byte DiagnosticBytes[192]; public string Diagnostic { get { fixed (byte* text = DiagnosticBytes) return Marshal.PtrToStringUTF8((IntPtr)text) ?? string.Empty; } } }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] internal struct NativePhysicsQueryRequest { public PhysicsAbiHeader Header; public PhysicsWorldId World; public PhysicsQueryKind Kind; public PhysicsQueryHitMode HitMode; public PhysicsPose Pose; public Vector3 Direction; public Vector3 ShapeExtent; public float Distance; public uint MaximumHits; public uint CollisionMask; public uint BodyTypeMask; public byte IncludeSensors; private byte _r0, _r1, _r2, _r3, _r4, _r5, _r6; public ulong SubmittedTick; public ulong ExpiryTick; public ulong CorrelationId; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] internal unsafe struct NativePhysicsQueryReceipt { public PhysicsAbiHeader Header; public PhysicsStatus Status; private uint _reserved; public PhysicsQueryHandle Handle; public ulong CorrelationId; public fixed byte DiagnosticBytes[192]; public string Diagnostic { get { fixed (byte* text = DiagnosticBytes) return Marshal.PtrToStringUTF8((IntPtr)text) ?? string.Empty; } } }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] internal unsafe struct NativePhysicsQueryResultInfo { public PhysicsAbiHeader Header; public PhysicsStatus Status; public uint HitCount; public uint Truncated; public ulong Tick; public ulong CorrelationId; public fixed byte DiagnosticBytes[192]; public string Diagnostic { get { fixed (byte* text = DiagnosticBytes) return Marshal.PtrToStringUTF8((IntPtr)text) ?? string.Empty; } } }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] internal struct NativePhysicsQueryHit { public PhysicsBodyHandle Body; public PhysicsUuid ObjectId; public uint ObjectGeneration; public uint ComponentGeneration; public PhysicsUuid MaterialId; public Vector3 Position; public Vector3 Normal; public float Distance; public float Fraction; public uint SubShapeKey; public byte CollisionLayer; public byte MotionType; public byte Sensor; private byte _reserved; public PhysicsQueryHit ToPublic() => new() { Body = Body, ObjectId = ObjectId, ObjectGeneration = ObjectGeneration, ComponentGeneration = ComponentGeneration, MaterialId = MaterialId, Position = Position, Normal = Normal, Distance = Distance, Fraction = Fraction, SubShapeKey = SubShapeKey, CollisionLayer = CollisionLayer, MotionType = MotionType, Sensor = Sensor != 0 }; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] internal struct NativePhysicsBodyState { public PhysicsAbiHeader Header; public PhysicsBodyHandle Body; public PhysicsIdentity Identity; public PhysicsPose Pose; public Vector3 LinearVelocity; public Vector3 AngularVelocity; public ulong Tick; public byte Sleeping; private byte _r0, _r1, _r2, _r3, _r4, _r5, _r6; public PhysicsBodyState ToPublic() => new() { Body = Body, Identity = Identity, Pose = Pose, LinearVelocity = LinearVelocity, AngularVelocity = AngularVelocity, Tick = Tick, Sleeping = Sleeping != 0 }; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] internal struct NativePhysicsCharacterState { public PhysicsAbiHeader Header; public PhysicsCharacterHandle Character; public PhysicsIdentity Identity; public PhysicsPose Pose; public Vector3 LinearVelocity; public Vector3 GroundNormal; public Vector3 SupportVelocity; public PhysicsBodyHandle SupportBody; public ulong Tick; public PhysicsCharacterGroundState GroundState; public uint ContactCount; public byte Supported; private byte _r0, _r1, _r2, _r3, _r4, _r5, _r6; public PhysicsCharacterState ToPublic() => new() { Character = Character, Identity = Identity, Pose = Pose, LinearVelocity = LinearVelocity, GroundNormal = GroundNormal, SupportVelocity = SupportVelocity, SupportBody = SupportBody, Tick = Tick, GroundState = GroundState, ContactCount = ContactCount, Supported = Supported != 0 }; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] internal struct NativePhysicsEventSubscriptionRequest { public PhysicsAbiHeader Header; public uint CategoryMask; public uint CollisionLayerMask; public byte IncludeSensors; public byte IncludeContactDetails; private byte _r0, _r1, _r2, _r3, _r4, _r5; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] internal unsafe struct NativePhysicsEventSubscription { public PhysicsAbiHeader Header; public PhysicsStatus Status; public uint Generation; public ulong Id; public fixed byte DiagnosticBytes[192]; public string Diagnostic { get { fixed (byte* text = DiagnosticBytes) return Marshal.PtrToStringUTF8((IntPtr)text) ?? string.Empty; } } }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] internal struct NativePhysicsEvent { public PhysicsAbiHeader Header; public PhysicsEventType Type; public uint HasSecondParticipant; public PhysicsWorldId World; public ulong Tick; public PhysicsIdentity First; public PhysicsIdentity Second; public Vector3 Point; public Vector3 Normal; public Vector3 RelativeVelocity; public float Penetration; public float Impulse; public ulong CorrelationId; public PhysicsEvent ToPublic() => new() { Type = Type, HasSecondParticipant = HasSecondParticipant != 0, World = World, Tick = Tick, First = First, Second = Second, Point = Point, Normal = Normal, RelativeVelocity = RelativeVelocity, Penetration = Penetration, Impulse = Impulse, CorrelationId = CorrelationId }; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] internal unsafe struct NativePhysicsSettingsSnapshot { public PhysicsAbiHeader Header; public PhysicsStatus Status; public uint SettingsGeneration; public Vector3 Gravity; public float FixedTimeStep; public uint MaxSubsteps; public uint MaxQueryBatchSize; public uint MaxHitsPerQuery; public uint MaxEventHistory; public ulong WorldId; public uint WorldGeneration; private uint _reserved; public fixed byte DiagnosticBytes[192]; public PhysicsSettingsSnapshot ToPublic() { fixed (byte* text = DiagnosticBytes) return new() { Status = Status, SettingsGeneration = SettingsGeneration, Gravity = Gravity, FixedTimeStep = FixedTimeStep, MaxSubsteps = MaxSubsteps, MaxQueryBatchSize = MaxQueryBatchSize, MaxHitsPerQuery = MaxHitsPerQuery, MaxEventHistory = MaxEventHistory, World = new PhysicsWorldId { Value = WorldId, Generation = WorldGeneration }, Diagnostic = Marshal.PtrToStringUTF8((IntPtr)text) ?? string.Empty }; } }

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal unsafe struct NativePhysicsCollisionLayer
	{
		public uint Id;
		public uint BroadPhaseGroup;
		public uint CollisionMask;
		public uint Enabled;
		public fixed byte NameBytes[64];

		public PhysicsCollisionLayer ToPublic()
		{
			fixed (byte* text = NameBytes)
			{
				return new PhysicsCollisionLayer { Id = Id, BroadPhaseGroup = BroadPhaseGroup, CollisionMask = CollisionMask, Enabled = Enabled != 0, Name = Marshal.PtrToStringUTF8((IntPtr)text) ?? string.Empty };
			}
		}

		public void SetName(string? name)
		{
			byte[] bytes = Encoding.UTF8.GetBytes(name ?? string.Empty);
			fixed (byte* target = NameBytes)
			{
				for (int index = 0; index < 64; ++index) target[index] = 0;
				int count = Math.Min(bytes.Length, 63);
				for (int index = 0; index < count; ++index) target[index] = bytes[index];
			}
		}
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal unsafe struct NativePhysicsSettingsRecord
	{
		public PhysicsAbiHeader Header;
		public PhysicsStatus Status;
		public uint SettingsGeneration;
		public Vector3 Gravity;
		public float FixedTimeStep;
		public uint MaxSubsteps;
		public float MaxAccumulatedTime;
		public uint VelocityIterations;
		public uint PositionIterations;
		public float MaxCastDistance;
		public float MaxShapeExtent;
		public uint MaxHitsPerQuery;
		public uint MaxRequestsPerBatch;
		public uint RetainedResultTicks;
		public uint DebugVisualizationEnabled;
		public uint RetainedDebugTicks;
		public uint MaxDebugPrimitives;
		public uint TransformPublicationChunkSize;
		public uint ProviderExtractionBatchSize;
		public uint ParallelPreparationThreshold;
		public uint ParallelExtractionThreshold;
		public uint AggregateDiagnosticsIntervalFrames;
		public uint ContinuousCollisionEnabled;
		public ulong WorldId;
		public uint WorldGeneration;
		private uint _reserved;
		public fixed byte LayerBytes[32 * 80];
		public fixed byte DiagnosticBytes[192];

		public PhysicsConfiguration ToPublic()
		{
			PhysicsCollisionLayer[] layers = new PhysicsCollisionLayer[32];
			fixed (byte* bytes = LayerBytes)
			{
				NativePhysicsCollisionLayer* nativeLayers = (NativePhysicsCollisionLayer*)bytes;
				for (int index = 0; index < layers.Length; ++index) layers[index] = nativeLayers[index].ToPublic();
			}
			string diagnostic;
			fixed (byte* text = DiagnosticBytes) diagnostic = Marshal.PtrToStringUTF8((IntPtr)text) ?? string.Empty;
			return new PhysicsConfiguration
			{
				Status = Status, SettingsGeneration = SettingsGeneration, Gravity = Gravity, FixedTimeStep = FixedTimeStep,
				MaxSubsteps = MaxSubsteps, MaxAccumulatedTime = MaxAccumulatedTime, VelocityIterations = VelocityIterations,
				PositionIterations = PositionIterations, MaxCastDistance = MaxCastDistance, MaxShapeExtent = MaxShapeExtent,
				MaxHitsPerQuery = MaxHitsPerQuery, MaxRequestsPerBatch = MaxRequestsPerBatch, RetainedResultTicks = RetainedResultTicks,
				DebugVisualizationEnabled = DebugVisualizationEnabled != 0, RetainedDebugTicks = RetainedDebugTicks,
				MaxDebugPrimitives = MaxDebugPrimitives, TransformPublicationChunkSize = TransformPublicationChunkSize,
				ProviderExtractionBatchSize = ProviderExtractionBatchSize, ParallelPreparationThreshold = ParallelPreparationThreshold,
				ParallelExtractionThreshold = ParallelExtractionThreshold, AggregateDiagnosticsIntervalFrames = AggregateDiagnosticsIntervalFrames,
				ContinuousCollisionEnabled = ContinuousCollisionEnabled != 0,
				World = new PhysicsWorldId { Value = WorldId, Generation = WorldGeneration }, Diagnostic = diagnostic, Layers = layers
			};
		}

		public static NativePhysicsSettingsRecord FromPublic(PhysicsConfiguration settings)
		{
			if (settings == null) throw new ArgumentNullException(nameof(settings));
			if (settings.Layers == null || settings.Layers.Length != 32) throw new ArgumentException("Physics settings require exactly 32 collision layers.", nameof(settings));
			NativePhysicsSettingsRecord native = new()
			{
				Header = PhysicsAbiHeader.For<NativePhysicsSettingsRecord>(), Gravity = settings.Gravity, FixedTimeStep = settings.FixedTimeStep,
				MaxSubsteps = settings.MaxSubsteps, MaxAccumulatedTime = settings.MaxAccumulatedTime, VelocityIterations = settings.VelocityIterations,
				PositionIterations = settings.PositionIterations, MaxCastDistance = settings.MaxCastDistance, MaxShapeExtent = settings.MaxShapeExtent,
				MaxHitsPerQuery = settings.MaxHitsPerQuery, MaxRequestsPerBatch = settings.MaxRequestsPerBatch, RetainedResultTicks = settings.RetainedResultTicks,
				DebugVisualizationEnabled = settings.DebugVisualizationEnabled ? 1U : 0U, RetainedDebugTicks = settings.RetainedDebugTicks,
				MaxDebugPrimitives = settings.MaxDebugPrimitives, TransformPublicationChunkSize = settings.TransformPublicationChunkSize,
				ProviderExtractionBatchSize = settings.ProviderExtractionBatchSize, ParallelPreparationThreshold = settings.ParallelPreparationThreshold,
				ParallelExtractionThreshold = settings.ParallelExtractionThreshold, AggregateDiagnosticsIntervalFrames = settings.AggregateDiagnosticsIntervalFrames,
				ContinuousCollisionEnabled = settings.ContinuousCollisionEnabled ? 1U : 0U
			};
			byte* bytes = native.LayerBytes;
			NativePhysicsCollisionLayer* layers = (NativePhysicsCollisionLayer*)bytes;
			for (int index = 0; index < 32; ++index)
			{
				PhysicsCollisionLayer source = settings.Layers[index];
				layers[index].Id = source.Id;
				layers[index].BroadPhaseGroup = source.BroadPhaseGroup;
				layers[index].CollisionMask = source.CollisionMask;
				layers[index].Enabled = source.Enabled ? 1U : 0U;
				layers[index].SetName(source.Name);
			}
			return native;
		}
	}
	[StructLayout(LayoutKind.Sequential, Pack = 8)] internal unsafe struct NativePhysicsIdentitySnapshot { public PhysicsAbiHeader Header; public PhysicsStatus Status; private uint _reserved; public PhysicsIdentity Identity; public fixed byte DiagnosticBytes[192]; }

	// These fixed-layout records mirror ScriptGlue.h. They flatten native tagged
	// variants and arrays so NativeAOT can copy authoring data without marshalling
	// an STL container or provider-owned pointer.
	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativePhysicsColliderRecord
	{
		public PhysicsAbiHeader Header;
		public PhysicsUuid RecordId;
		public uint RecordVersion;
		public ColliderShapeKind ShapeKind;
		public Vector3 LocalCenter;
		public Quaternion LocalRotation;
		public Vector3 HalfExtents;
		public float Radius;
		public float Height;
		public float TopRadius;
		public float BottomRadius;
		public PhysicsUuid ColliderAssetId;
		public PhysicsUuid MaterialId;
		public ColliderQueryFlags QueryFlags;
		public byte CollisionLayerId;
		public byte EventPolicy;
		public byte UseDefaultMaterial;
		public byte IsTrigger;
		public byte UseLayerOverride;
		public byte Enabled;
		public byte HasDebugColorOverride;
		private byte _reserved;
		public float DebugRed;
		public float DebugGreen;
		public float DebugBlue;
		public float DebugAlpha;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)] internal struct NativePhysicsConstraintLimits { public uint Enabled; public float Minimum; public float Maximum; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] internal struct NativePhysicsConstraintSpring { public uint Enabled; public float Stiffness; public float Damping; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] internal struct NativePhysicsConstraintMotor { public PhysicsConstraintMotorMode Mode; public float TargetPosition; public float TargetVelocity; public float MaximumForce; }
	[StructLayout(LayoutKind.Sequential, Pack = 8)] internal struct NativePhysicsConstraintAxisSettings { public NativePhysicsConstraintLimits Limits; public NativePhysicsConstraintSpring Spring; public NativePhysicsConstraintMotor Motor; }

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal unsafe struct NativePhysicsConstraintRecord
	{
		public PhysicsAbiHeader Header;
		public PhysicsUuid RecordId;
		public uint RecordVersion;
		public PhysicsConstraintKind Kind;
		public PhysicsConstraintEndpointKind FirstEndpointKind;
		public PhysicsConstraintEndpointKind SecondEndpointKind;
		public PhysicsUuid FirstObjectId;
		public PhysicsUuid SecondObjectId;
		public PhysicsPose FirstFrame;
		public PhysicsPose SecondFrame;
		public Vector3 FirstAxis;
		public Vector3 SecondAxis;
		public NativePhysicsConstraintLimits Limits;
		public NativePhysicsConstraintSpring Spring;
		public NativePhysicsConstraintMotor Motor;
		public NativePhysicsConstraintAxisSettings TranslationX;
		public NativePhysicsConstraintAxisSettings TranslationY;
		public NativePhysicsConstraintAxisSettings TranslationZ;
		public NativePhysicsConstraintAxisSettings RotationX;
		public NativePhysicsConstraintAxisSettings RotationY;
		public NativePhysicsConstraintAxisSettings RotationZ;
		public PhysicsUuid FirstGearConstraintId;
		public PhysicsUuid SecondGearConstraintId;
		public float GearRatio;
		public float BreakForce;
		public float BreakTorque;
		public uint ResetGeneration;
		public byte CollideConnected;
		public byte Enabled;
		private fixed byte _reserved[6];
	}
}
