// Light.cs — Managed API for light components and illumination properties.
//
// This file defines the managed Light class, allowing scripts to adjust the
// color and intensity of scene light sources. It wraps the underlying native
// LightComponent and provides a simplified C# interface for dynamic lighting
// adjustments during gameplay.

namespace TerraEngine
{
	// Light is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public class Light : Component
	{
		public Vector3 Color
		{
			get => GetProperty<Vector3>("color");
			set => SetProperty("color", value);
		}

		public float Intensity
		{
			get => GetProperty<float>("intensity");
			set => SetProperty("intensity", value);
		}
	}
}
