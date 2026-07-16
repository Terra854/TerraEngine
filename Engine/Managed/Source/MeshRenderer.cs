// MeshRenderer.cs — Managed API for mesh rendering components and asset binding.
//
// This file defines the managed MeshRenderer class, which allows scripts to
// assign mesh and material assets to scene objects. It bridges the managed
// asset references to the native MeshRenderComponent and handles the resolution
// of asset names to internal engine identifiers.

namespace TerraEngine
{
	// MeshRenderer is the managed-facing contract for this subsystem; keep it predictable for scripts.
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

		public bool TaskShaderCullingEnabled
		{
			get => GetProperty<bool>("taskShaderCullingEnabled");
			set => SetProperty("taskShaderCullingEnabled", value);
		}
	}
}
