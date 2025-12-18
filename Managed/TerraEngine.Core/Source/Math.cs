using System;
using System.Runtime.InteropServices;

namespace TerraEngine.Core
{
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
    public struct Quaternion
    {
        public float W, X, Y, Z;

        public Quaternion(float w, float x, float y, float z)
        {
            W = w; X = x; Y = y; Z = z;
        }

        public static Quaternion Identity => new Quaternion(1, 0, 0, 0);
    }
}
