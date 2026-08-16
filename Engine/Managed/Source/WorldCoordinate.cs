// WorldCoordinate.cs — Exact managed addresses for effectively unbounded scenes.

using System;
using System.Runtime.InteropServices;

namespace TerraEngine
{
	[StructLayout(LayoutKind.Sequential)]
	public struct WorldCell
	{
		public long X, Y, Z;

		public WorldCell(long x, long y, long z) { X = x; Y = y; Z = z; }
		public override string ToString() => $"({X}, {Y}, {Z})";
	}

	[StructLayout(LayoutKind.Sequential)]
	public struct WorldPosition
	{
		public WorldCell Cell;
		public double OffsetX, OffsetY, OffsetZ;

		public WorldPosition(WorldCell cell, double offsetX, double offsetY, double offsetZ)
		{
			Cell = cell; OffsetX = offsetX; OffsetY = offsetY; OffsetZ = offsetZ;
		}
	}

	[StructLayout(LayoutKind.Sequential)]
	public struct WorldDelta
	{
		public WorldCell CellDelta;
		public double OffsetX, OffsetY, OffsetZ;

		public WorldDelta(WorldCell cellDelta, double offsetX, double offsetY, double offsetZ)
		{
			CellDelta = cellDelta; OffsetX = offsetX; OffsetY = offsetY; OffsetZ = offsetZ;
		}
	}

	[StructLayout(LayoutKind.Sequential)]
	public struct WorldPose
	{
		public WorldPosition Position;
		public Quaternion Rotation;
		public Vector3 Scale;
		public ulong TransformRevision;
		public ulong CoordinateFrameRevision;
	}

	// Internal ABI records deliberately match ScriptGlue.h field-for-field.
	[StructLayout(LayoutKind.Sequential)]
	internal struct NativeWorldDelta
	{
		internal WorldCell CellDelta;
		internal double OffsetX, OffsetY, OffsetZ;
		internal static NativeWorldDelta FromWorldDelta(WorldDelta value) => new NativeWorldDelta
		{
			CellDelta = value.CellDelta, OffsetX = value.OffsetX, OffsetY = value.OffsetY, OffsetZ = value.OffsetZ
		};
	}

	[StructLayout(LayoutKind.Sequential)]
	internal struct NativeWorldPose
	{
		internal WorldPosition Position;
		internal Quaternion Rotation;
		internal Vector3 Scale;
		internal ulong TransformRevision;
		internal ulong CoordinateFrameRevision;

		internal WorldPose ToWorldPose() => new WorldPose
		{
			Position = Position,
			Rotation = Rotation,
			Scale = Scale,
			TransformRevision = TransformRevision,
			CoordinateFrameRevision = CoordinateFrameRevision
		};

		internal static NativeWorldPose FromWorldPose(WorldPose value) => new NativeWorldPose
		{
			Position = value.Position,
			Rotation = value.Rotation,
			Scale = value.Scale,
			TransformRevision = value.TransformRevision,
			CoordinateFrameRevision = value.CoordinateFrameRevision
		};
	}
}
