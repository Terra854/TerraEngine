struct SkyTaskPayload
{
	uint dispatchToken;
};

groupshared SkyTaskPayload gSkyTaskPayload;

[numthreads(1, 1, 1)]
void TSMain(uint3 groupId : SV_GroupID)
{
	// The sky pass only needs one mesh workgroup because the mesh stage emits a
	// single fullscreen triangle procedurally.
	gSkyTaskPayload.dispatchToken = groupId.x;
	DispatchMesh(1u, 1u, 1u, gSkyTaskPayload);
}
