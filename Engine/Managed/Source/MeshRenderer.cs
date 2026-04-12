namespace TerraEngine
{
	public class MeshRenderer : Component
	{
		// Scripts work with readable asset names/paths while the engine bridge
		// resolves them to UUIDs behind the scenes for the renderer.
		public string Mesh
		{
			get => InternalCalls.ReadNativeUtf8(InternalCalls.Native_MeshRenderer_GetMesh(Object.ID));
			set => InternalCalls.Native_MeshRenderer_SetMesh(Object.ID, value ?? string.Empty);
		}

		public string Material
		{
			get => InternalCalls.ReadNativeUtf8(InternalCalls.Native_MeshRenderer_GetMaterial(Object.ID));
			set => InternalCalls.Native_MeshRenderer_SetMaterial(Object.ID, value ?? string.Empty);
		}
	}
}
