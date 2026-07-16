// Sky.cs — Managed authoring surface for the scene-owned sky component.

namespace TerraEngine
{
	public class Sky : Component
	{
		public string SourceAsset
		{
			get => GetStringProperty("sourceAsset");
			set => SetStringProperty("sourceAsset", value);
		}

		public float Exposure
		{
			get => GetProperty<float>("exposure");
			set => SetProperty("exposure", value);
		}

		public float RotationOffsetDegrees
		{
			get => GetProperty<float>("rotationOffsetDegrees");
			set => SetProperty("rotationOffsetDegrees", value);
		}

		public Vector3 HorizonTint
		{
			get => GetProperty<Vector3>("horizonTint");
			set => SetProperty("horizonTint", value);
		}

		public Vector3 ZenithTint
		{
			get => GetProperty<Vector3>("zenithTint");
			set => SetProperty("zenithTint", value);
		}

		public Vector3 GroundTint
		{
			get => GetProperty<Vector3>("groundTint");
			set => SetProperty("groundTint", value);
		}
	}
}
