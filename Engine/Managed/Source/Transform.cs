// Transform.cs — Contains the managed Transform API surface, properties, native-call bindings, and script/editor workflow helpers for C# scripting API types that mirror native engine objects, components, and services.

namespace TerraEngine
{
	// Transform exposes only the canonical world contract. Each setter submits a
	// complete typed transaction so scripts cannot bypass revision validation.
	public class Transform : Component
	{
		public WorldPose Pose
		{
			get
			{
				uint status = InternalCalls.Native_Transform_ReadWorldPose(Object.ID, out NativeWorldPose nativePose);
				if (status != 0U)
					throw new System.InvalidOperationException($"Unable to read canonical transform (status {status}).");
				return nativePose.ToWorldPose();
			}
			set
			{
				NativeWorldPose nativePose = NativeWorldPose.FromWorldPose(value);
				uint status = InternalCalls.Native_Transform_SubmitWorldPose(Object.ID, in nativePose);
				if (status != 1U)
					throw new System.InvalidOperationException($"Unable to queue canonical transform (status {status}).");
			}
		}

		public WorldPosition Position
		{
			get => Pose.Position;
			set { WorldPose pose = Pose; pose.Position = value; Pose = pose; }
		}

		public Quaternion Rotation
		{
			get => Pose.Rotation;
			set { WorldPose pose = Pose; pose.Rotation = value; Pose = pose; }
		}

		public Vector3 Scale
		{
			get => Pose.Scale;
			set { WorldPose pose = Pose; pose.Scale = value; Pose = pose; }
		}

		public void Translate(WorldDelta delta)
		{
			NativeWorldDelta nativeDelta = NativeWorldDelta.FromWorldDelta(delta);
			uint status = InternalCalls.Native_Transform_TranslateWorld(Object.ID, in nativeDelta);
			if (status != 1U)
				throw new System.InvalidOperationException($"Unable to queue canonical translation (status {status}).");
		}
	}
}
