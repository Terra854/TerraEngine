// Transform.cs — Contains the managed Transform API surface, properties, native-call bindings, and script/editor workflow helpers for C# scripting API types that mirror native engine objects, components, and services.

namespace TerraEngine
{
    // Transform is the managed-facing contract for this subsystem; keep it predictable for scripts.
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
