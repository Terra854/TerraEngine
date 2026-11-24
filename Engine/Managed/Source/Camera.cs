// Camera.cs — Managed API for camera components and perspective settings.
//
// This file defines the managed Camera class, allowing scripts to manipulate
// perspective parameters like Field of View and clipping planes. It provides
// idiomatic C# properties that wrap the underlying native CameraComponent
// state managed by the renderer.

namespace TerraEngine
{
	// Camera is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public class Camera : Component
	{
		private GraphicsSettingsTarget _graphicsTarget;
		private bool _hasGraphicsTarget;

		internal override void OnObjectAssigned()
		{
			_graphicsTarget = default;
			_hasGraphicsTarget = false;
		}

		internal GraphicsSettingsTarget GetGraphicsSettingsTarget()
		{
			// A camera can be queried before its protected publication is ready.
			// Retry until the first successful bind, then keep that identity so a
			// retired managed wrapper cannot silently target a replacement camera.
			if (Object is null) return new(new NativeGraphicsSettingsTargetPacket { Scope = 1U, Flags = 1U });
			if (!_hasGraphicsTarget)
			{
				if (Graphics.TryResolveGraphicsCamera(Object.ID, out var target))
				{
					_graphicsTarget = target;
					_hasGraphicsTarget = true;
				}
				else return new(new NativeGraphicsSettingsTargetPacket { Scope = 1U, Flags = 1U });
			}
			return _graphicsTarget;
		}

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

		public float AspectRatio
		{
			get => GetProperty<float>("aspectRatio");
			set => SetProperty("aspectRatio", value);
		}

		public Vector3 Up
		{
			get => GetProperty<Vector3>("up");
			set => SetProperty("up", value);
		}
	}
}
