// Math.cs — Managed implementations of common engine math types.
//
// This file provides C# versions of the core math structures used by the engine,
// including Vector2, Vector3, Vector4, and Quaternion. These types are designed
// to be binary-compatible with the native engine's GLM-based math types for
// efficient data transfer across the managed-native boundary.

using System;
using System.Runtime.InteropServices;

namespace TerraEngine
{
	[StructLayout(LayoutKind.Sequential)]
	// Vector2 is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public struct Vector2
	{
		public float X, Y;

		public Vector2(float x, float y)
		{
			X = x;
			Y = y;
		}

		// ToString bridges managed callers to engine state; validate arguments before forwarding to native code.
		public override string ToString() => $"({X}, {Y})";
	}

    [StructLayout(LayoutKind.Sequential)]
    // Vector3 is the managed-facing contract for this subsystem; keep it predictable for scripts.
    public struct Vector3
    {
        public float X, Y, Z;

        public Vector3(float x, float y, float z)
        {
            X = x; Y = y; Z = z;
        }

        // ToString bridges managed callers to engine state; validate arguments before forwarding to native code.
        public override string ToString() => $"({X}, {Y}, {Z})";
    }

	[StructLayout(LayoutKind.Sequential)]
	// Vector4 is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public struct Vector4
	{
		public float X, Y, Z, W;

		public Vector4(float x, float y, float z, float w)
		{
			X = x;
			Y = y;
			Z = z;
			W = w;
		}

		// ToString bridges managed callers to engine state; validate arguments before forwarding to native code.
		public override string ToString() => $"({X}, {Y}, {Z}, {W})";
	}

	[StructLayout(LayoutKind.Sequential)]
	// Quaternion is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public struct Quaternion
	{
		public float X, Y, Z, W;

        public Quaternion(float x, float y, float z, float w)
		{
			X = x; Y = y; Z = z; W = w;
		}

		public static Quaternion Identity => new Quaternion(0, 0, 0, 1);
	}
}
