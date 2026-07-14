// ConstraintSet.cs - stable constraint record identity facade for managed scripts.

namespace TerraEngine
{
	public sealed class ConstraintSet : Component
	{
		public PhysicsIdentity Identity { get; internal set; }
		public ConstraintRecord GetRecord(PhysicsConstraintHandle handle, PhysicsUuid recordId, uint recordGeneration) => new(handle, Identity, recordId, recordGeneration);
	}

	public readonly struct ConstraintRecord
	{
		public ConstraintRecord(PhysicsConstraintHandle handle, PhysicsIdentity owner, PhysicsUuid recordId, uint recordGeneration) { Handle = handle; Owner = owner; RecordId = recordId; RecordGeneration = recordGeneration; }
		public PhysicsConstraintHandle Handle { get; }
		public PhysicsIdentity Owner { get; }
		public PhysicsUuid RecordId { get; }
		public uint RecordGeneration { get; }
	}
}
