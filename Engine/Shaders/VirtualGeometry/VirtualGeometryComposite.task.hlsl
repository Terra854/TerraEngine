// The visibility composite needs exactly one mesh workgroup. Keeping the
// dispatch in a task shader makes this engine graphics pass use the same
// task/mesh/fragment stage topology as the other mesh-native render paths.
struct CompositeTaskPayload
{
	uint dispatchToken;
};

groupshared CompositeTaskPayload gCompositeTaskPayload;

[numthreads(1, 1, 1)]
void main(uint3 groupId : SV_GroupID)
{
	gCompositeTaskPayload.dispatchToken = groupId.x;
	DispatchMesh(1u, 1u, 1u, gCompositeTaskPayload);
}
