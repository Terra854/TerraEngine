namespace TerraEngine
{
	public class Camera : Component
	{
		// These wrappers translate the engine's reflected property names into the
		// shorter camera vocabulary gameplay scripts expect to write by hand.
		public float FieldOfView
		{
			get => GetProperty<float>("verticalFovDegrees");
			set => SetProperty("verticalFovDegrees", value);
		}

		public float NearClipPlane
		{
			get => GetProperty<float>("nearPlane");
			set => SetProperty("nearPlane", value);
		}

		public float FarClipPlane
		{
			get => GetProperty<float>("farPlane");
			set => SetProperty("farPlane", value);
		}
	}
}
