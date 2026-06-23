namespace TerraEngine
{
	/// <summary>
	/// Asset categories that can be requested through visibility streaming.
	/// Values mirror native AssetType so the bridge can stay allocation-light.
	/// </summary>
	public enum StreamableAssetType
	{
		Mesh = 0,
		Texture = 1,
		Material = 2,
		ShaderCollection = 3
	}

	/// <summary>
	/// Runtime hints for loading assets before rendering requires them. These
	/// calls are intentionally non-blocking; the renderer still uses fallback
	/// resources if an asset is not ready by the time it becomes visible.
	/// </summary>
	public static class ResourceStreaming
	{
		public static bool Preload(string assetReference, StreamableAssetType assetType)
		{
			if (string.IsNullOrWhiteSpace(assetReference))
			{
				return false;
			}

			return InternalCalls.Native_ResourceStreaming_Preload(assetReference, (int)assetType);
		}
	}
}
