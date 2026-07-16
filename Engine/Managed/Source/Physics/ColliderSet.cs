// ColliderSet.cs - copied, atomic collider collection authoring for managed scripts.

using System;

namespace TerraEngine
{
	public enum ColliderShapeKind : uint { Box, Sphere, Capsule, Cylinder, TaperedCapsule, ConvexHull, Mesh, Compound }
	public enum ColliderEventPolicy : byte { None, Collision, Trigger, All }

	[Flags]
	public enum ColliderQueryFlags : uint
	{
		None = 0,
		Ray = 1U << 0,
		ShapeCast = 1U << 1,
		Overlap = 1U << 2,
		Point = 1U << 3,
		All = Ray | ShapeCast | Overlap | Point
	}

	public struct ColliderDebugColor
	{
		public float Red;
		public float Green;
		public float Blue;
		public float Alpha;
		public static ColliderDebugColor White => new() { Red = 1, Green = 1, Blue = 1, Alpha = 1 };
	}

	// One definition owns every possible shape payload. ShapeKind selects which
	// dimensions or cooked asset ID native validation will consume.
	public struct ColliderDefinition
	{
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
		public bool UseDefaultMaterial;
		public bool IsTrigger;
		public bool UseLayerOverride;
		public byte CollisionLayerId;
		public ColliderEventPolicy EventPolicy;
		public ColliderQueryFlags QueryFlags;
		public bool Enabled;
		public bool HasDebugColorOverride;
		public ColliderDebugColor DebugColor;

		public static ColliderDefinition Box(Vector3 halfExtents)
		{
			ColliderDefinition value = Defaults(ColliderShapeKind.Box);
			value.HalfExtents = halfExtents;
			return value;
		}

		public static ColliderDefinition Sphere(float radius)
		{
			ColliderDefinition value = Defaults(ColliderShapeKind.Sphere);
			value.Radius = radius;
			return value;
		}

		public static ColliderDefinition Capsule(float radius, float height)
		{
			ColliderDefinition value = Defaults(ColliderShapeKind.Capsule);
			value.Radius = radius;
			value.Height = height;
			return value;
		}

		public static ColliderDefinition Cylinder(float radius, float height)
		{
			ColliderDefinition value = Defaults(ColliderShapeKind.Cylinder);
			value.Radius = radius;
			value.Height = height;
			return value;
		}

		public static ColliderDefinition TaperedCapsule(float topRadius, float bottomRadius, float height)
		{
			ColliderDefinition value = Defaults(ColliderShapeKind.TaperedCapsule);
			value.TopRadius = topRadius;
			value.BottomRadius = bottomRadius;
			value.Height = height;
			return value;
		}

		public static ColliderDefinition FromAsset(ColliderShapeKind kind, PhysicsUuid colliderAssetId)
		{
			if (kind != ColliderShapeKind.ConvexHull && kind != ColliderShapeKind.Mesh && kind != ColliderShapeKind.Compound)
				throw new ArgumentOutOfRangeException(nameof(kind), "Asset colliders must be ConvexHull, Mesh, or Compound.");
			ColliderDefinition value = Defaults(kind);
			value.ColliderAssetId = colliderAssetId;
			return value;
		}

		private static ColliderDefinition Defaults(ColliderShapeKind kind) => new()
		{
			RecordId = PhysicsUuid.New(),
			RecordVersion = 1,
			ShapeKind = kind,
			LocalRotation = Quaternion.Identity,
			UseDefaultMaterial = true,
			EventPolicy = ColliderEventPolicy.All,
			QueryFlags = ColliderQueryFlags.All,
			Enabled = true,
			DebugColor = ColliderDebugColor.White
		};

		internal NativePhysicsColliderRecord ToNative() => new()
		{
			Header = PhysicsAbiHeader.For<NativePhysicsColliderRecord>(), RecordId = RecordId, RecordVersion = RecordVersion,
			ShapeKind = ShapeKind, LocalCenter = LocalCenter, LocalRotation = LocalRotation, HalfExtents = HalfExtents,
			Radius = Radius, Height = Height, TopRadius = TopRadius, BottomRadius = BottomRadius,
			ColliderAssetId = ColliderAssetId, MaterialId = MaterialId, QueryFlags = QueryFlags,
			CollisionLayerId = CollisionLayerId, EventPolicy = (byte)EventPolicy,
			UseDefaultMaterial = UseDefaultMaterial ? (byte)1 : (byte)0, IsTrigger = IsTrigger ? (byte)1 : (byte)0,
			UseLayerOverride = UseLayerOverride ? (byte)1 : (byte)0, Enabled = Enabled ? (byte)1 : (byte)0,
			HasDebugColorOverride = HasDebugColorOverride ? (byte)1 : (byte)0,
			DebugRed = DebugColor.Red, DebugGreen = DebugColor.Green, DebugBlue = DebugColor.Blue, DebugAlpha = DebugColor.Alpha
		};

		internal static ColliderDefinition FromNative(NativePhysicsColliderRecord value) => new()
		{
			RecordId = value.RecordId, RecordVersion = value.RecordVersion, ShapeKind = value.ShapeKind,
			LocalCenter = value.LocalCenter, LocalRotation = value.LocalRotation, HalfExtents = value.HalfExtents,
			Radius = value.Radius, Height = value.Height, TopRadius = value.TopRadius, BottomRadius = value.BottomRadius,
			ColliderAssetId = value.ColliderAssetId, MaterialId = value.MaterialId, QueryFlags = value.QueryFlags,
			CollisionLayerId = value.CollisionLayerId, EventPolicy = (ColliderEventPolicy)value.EventPolicy,
			UseDefaultMaterial = value.UseDefaultMaterial != 0, IsTrigger = value.IsTrigger != 0,
			UseLayerOverride = value.UseLayerOverride != 0, Enabled = value.Enabled != 0,
			HasDebugColorOverride = value.HasDebugColorOverride != 0,
			DebugColor = new ColliderDebugColor { Red = value.DebugRed, Green = value.DebugGreen, Blue = value.DebugBlue, Alpha = value.DebugAlpha }
		};
	}

	public sealed class ColliderSet : Component
	{
		public PhysicsIdentity Identity { get; internal set; }
		public ColliderRecord GetRecord(PhysicsUuid recordId, uint recordGeneration) => new(Identity, recordId, recordGeneration);

		public ColliderDefinition[] Records
		{
			get
			{
				PhysicsStatus status = TryGetRecords(out ColliderDefinition[] records);
				if (status != PhysicsStatus.Success) throw new InvalidOperationException($"Could not read ColliderSet records: {status}.");
				return records;
			}
			set
			{
				PhysicsStatus status = ReplaceRecords(value ?? Array.Empty<ColliderDefinition>());
				if (status != PhysicsStatus.Success && status != PhysicsStatus.Accepted)
					throw new ArgumentException($"Could not replace ColliderSet records: {status}.", nameof(value));
			}
		}

		public unsafe PhysicsStatus TryGetRecords(out ColliderDefinition[] records)
		{
			records = Array.Empty<ColliderDefinition>();
			PhysicsStatus status = InternalCalls.Native_Physics_CopyColliderRecords(Object.ID, null, 0, out uint count);
			if (status != PhysicsStatus.Success && status != PhysicsStatus.LimitExceeded) return status;
			if (count == 0) return PhysicsStatus.Success;
			NativePhysicsColliderRecord[] native = new NativePhysicsColliderRecord[count];
			fixed (NativePhysicsColliderRecord* pointer = native)
				status = InternalCalls.Native_Physics_CopyColliderRecords(Object.ID, pointer, count, out _);
			if (status != PhysicsStatus.Success) return status;
			records = new ColliderDefinition[count];
			for (int index = 0; index < records.Length; ++index) records[index] = ColliderDefinition.FromNative(native[index]);
			return PhysicsStatus.Success;
		}

		// Native validates the complete candidate before one protected replacement.
		// Worker callbacks receive Accepted and observe the new list after flush.
		public unsafe PhysicsStatus ReplaceRecords(ReadOnlySpan<ColliderDefinition> records)
		{
			NativePhysicsColliderRecord[] native = new NativePhysicsColliderRecord[records.Length];
			for (int index = 0; index < records.Length; ++index) native[index] = records[index].ToNative();
			if (native.Length == 0) return InternalCalls.Native_Physics_ReplaceColliderRecords(Object.ID, null, 0);
			fixed (NativePhysicsColliderRecord* pointer = native)
				return InternalCalls.Native_Physics_ReplaceColliderRecords(Object.ID, pointer, (uint)native.Length);
		}
	}

	public readonly struct ColliderRecord
	{
		public ColliderRecord(PhysicsIdentity owner, PhysicsUuid recordId, uint recordGeneration) { Owner = owner; RecordId = recordId; RecordGeneration = recordGeneration; }
		public PhysicsIdentity Owner { get; }
		public PhysicsUuid RecordId { get; }
		public uint RecordGeneration { get; }
	}
}
