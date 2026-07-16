// Light.cs — Managed API for light components and illumination properties.
//
// This file defines the managed Light class, allowing scripts to adjust the
// color and intensity of scene light sources. It wraps the underlying native
// LightComponent and provides a simplified C# interface for dynamic lighting
// adjustments during gameplay.

namespace TerraEngine
{
	public enum LightType : uint
	{
		Directional = 0,
		Point = 1,
		Spot = 2,
		Area = 3
	}

	public enum LightUnit : uint
	{
		Lumens = 0,
		Lux = 1
	}

	public enum LightAppearance : uint
	{
		Color = 0,
		FilterAndTemperature = 1
	}

	// Light is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public class Light : Component
	{
		public LightType Type
		{
			get => (LightType)GetProperty<uint>("type");
			set => SetProperty("type", (uint)value);
		}

		public LightUnit Unit
		{
			get => (LightUnit)GetProperty<uint>("unit");
			set => SetProperty("unit", (uint)value);
		}

		public LightAppearance Appearance
		{
			get => (LightAppearance)GetProperty<uint>("appearance");
			set => SetProperty("appearance", (uint)value);
		}

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

		public float Temperature
		{
			get => GetProperty<float>("temperature");
			set => SetProperty("temperature", value);
		}

		public float Range
		{
			get => GetProperty<float>("range");
			set => SetProperty("range", value);
		}

		public float InnerConeAngleRadians
		{
			get => GetProperty<float>("innerConeAngle");
			set => SetProperty("innerConeAngle", value);
		}

		public float OuterConeAngleRadians
		{
			get => GetProperty<float>("outerConeAngle");
			set => SetProperty("outerConeAngle", value);
		}

		public float Width
		{
			get => GetProperty<float>("width");
			set => SetProperty("width", value);
		}

		public float Height
		{
			get => GetProperty<float>("height");
			set => SetProperty("height", value);
		}

		public bool CastsShadows
		{
			get => GetProperty<bool>("castsShadows");
			set => SetProperty("castsShadows", value);
		}

		public float ShadowStrength
		{
			get => GetProperty<float>("shadowStrength");
			set => SetProperty("shadowStrength", value);
		}

		public float ShadowBias
		{
			get => GetProperty<float>("shadowBias");
			set => SetProperty("shadowBias", value);
		}

		public float IndirectMultiplier
		{
			get => GetProperty<float>("indirectMultiplier");
			set => SetProperty("indirectMultiplier", value);
		}

		public float AuthoredPriority
		{
			get => GetProperty<float>("authoredPriority");
			set => SetProperty("authoredPriority", value);
		}

		public float StabilityPreference
		{
			get => GetProperty<float>("stabilityPreference");
			set => SetProperty("stabilityPreference", value);
		}
	}
}
