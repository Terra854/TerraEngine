// ColliderSet.cs - stable collider-record identity facade for managed scripts.

namespace TerraEngine
{
	public sealed class ColliderSet : Component
	{
		public PhysicsIdentity Identity { get; internal set; }
		public ColliderRecord GetRecord(PhysicsUuid recordId, uint recordGeneration) => new(Identity, recordId, recordGeneration);
	}

	public readonly struct ColliderRecord
	{
		public ColliderRecord(PhysicsIdentity owner, PhysicsUuid recordId, uint recordGeneration) { Owner = owner; RecordId = recordId; RecordGeneration = recordGeneration; }
		public PhysicsIdentity Owner { get; }
		public PhysicsUuid RecordId { get; }
		public uint RecordGeneration { get; }
	}
}
