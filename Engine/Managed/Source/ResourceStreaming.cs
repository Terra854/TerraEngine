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

	public enum StreamingPayloadState : uint
	{
		Registered = 0,
		Deferred = 1,
		LoadingRam = 2,
		RamResident = 3,
		UploadingVram = 4,
		VramResident = 5,
		PendingRelease = 6,
		Released = 7,
		Failed = 8
	}

	public readonly struct ResourceStreamingStatus
	{
		internal ResourceStreamingStatus(NativeResourceStreamingStatus value)
		{
			Status = value.Status;
			AssetType = (StreamableAssetType)value.AssetType;
			PayloadState = (StreamingPayloadState)value.PayloadState;
			Resident = value.Resident != 0;
			CpuBytes = value.CpuBytes;
			GpuBytes = value.GpuBytes;
			Generation = value.Generation;
			Diagnostic = value.Diagnostic;
		}

		public RuntimeStatus Status { get; }
		public StreamableAssetType AssetType { get; }
		public StreamingPayloadState PayloadState { get; }
		public bool Resident { get; }
		public ulong CpuBytes { get; }
		public ulong GpuBytes { get; }
		public ulong Generation { get; }
		public string Diagnostic { get; }
		public bool Ready => Resident || PayloadState == StreamingPayloadState.RamResident || PayloadState == StreamingPayloadState.VramResident;
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

		public static ResourceStreamingStatus GetStatus(string assetReference, StreamableAssetType assetType)
		{
			NativeResourceStreamingStatus native = new() { Header = NativeRuntimeAbiHeader.For<NativeResourceStreamingStatus>() };
		if (string.IsNullOrWhiteSpace(assetReference))
		{
			native.Status = RuntimeStatus.InvalidArgument;
			return new ResourceStreamingStatus(native);
		}
			InternalCalls.Native_ResourceStreaming_ReadStatus(assetReference, (int)assetType, ref native);
			return new ResourceStreamingStatus(native);
		}

		// Release is queued for the protected frame boundary because GPU-backed
		// resources may still be referenced by the current render submission.
		public static RuntimeStatus Release(string assetReference, StreamableAssetType assetType)
		{
			if (string.IsNullOrWhiteSpace(assetReference)) return RuntimeStatus.InvalidArgument;
			return InternalCalls.Native_ResourceStreaming_RequestRelease(assetReference, (int)assetType);
		}
	}
}
