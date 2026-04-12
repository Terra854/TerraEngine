namespace TerraEngine
{
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
