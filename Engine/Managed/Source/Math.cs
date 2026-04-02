using System;
using System.Runtime.InteropServices;

namespace TerraEngine
{
	[StructLayout(LayoutKind.Sequential)]
	public struct Vector2
	{
		public float X, Y;

		public Vector2(float x, float y)
		{
			X = x;
			Y = y;
		}

		public override string ToString() => $"({X}, {Y})";
	}

    [StructLayout(LayoutKind.Sequential)]
    public struct Vector3
    {
        public float X, Y, Z;

        public Vector3(float x, float y, float z)
        {
            X = x; Y = y; Z = z;
        }

        public override string ToString() => $"({X}, {Y}, {Z})";
    }

	[StructLayout(LayoutKind.Sequential)]
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

		public override string ToString() => $"({X}, {Y}, {Z}, {W})";
	}

    [StructLayout(LayoutKind.Sequential)]
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
