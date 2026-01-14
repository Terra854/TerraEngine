namespace TerraEngine
{
    public class Transform : Component
    {
        public Vector3 Position
        {
            get => GetProperty<Vector3>("Position");
            set => SetProperty<Vector3>("Position", value);
        }

        public Quaternion Rotation
        {
            get => GetProperty<Quaternion>("Rotation");
            set => SetProperty<Quaternion>("Rotation", value);
        }

        public Vector3 Scale
        {
            get => GetProperty<Vector3>("Scale");
            set => SetProperty<Vector3>("Scale", value);
        }
    }
}
