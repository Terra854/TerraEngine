// AudioTypes.cs - fixed-width managed mirrors of the native audio ABI.

using System;
using System.Runtime.InteropServices;

namespace TerraEngine.Audio
{
	public enum AudioStatus : uint
	{
		Success,
		Queued,
		Accepted,
		NotFound,
		StaleTarget,
		InvalidArgument,
		LimitExceeded,
		ProviderFailure,
		VersionMismatch
	}

	public enum AudioCommandKind : uint { Play, Pause, Resume, Stop }
	public enum AudioPlaybackState : uint { Stopped, Playing, Paused }
	public enum AudioTargetMode : uint { ExactInstance = 0, BindCurrentAtAdmission = 1 }

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct AudioAbiHeader
	{
		internal const uint Version = 2;
		public uint AbiVersion;
		public uint StructSize;
		public static AudioAbiHeader For<T>() where T : struct => new()
		{
			AbiVersion = Version,
			StructSize = checked((uint)Marshal.SizeOf<T>())
		};
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAudioSourceTarget
	{
		// UUID halves retain the same raw 16 bytes used by the native bridge.
		public PhysicsUuid ObjectId;
		public ulong SceneGeneration;
		public ulong ObjectGeneration;
		public ulong ComponentGeneration;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAudioSourceBinding
	{
		public AudioAbiHeader Header;
		public AudioStatus Status;
		public uint ObjectEntity;
		public NativeAudioSourceTarget Target;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAudioCommandRequest
	{
		public AudioAbiHeader Header;
		public AudioCommandKind Kind;
		public AudioTargetMode TargetMode;
		public NativeAudioSourceTarget Target;
		public ulong PlaybackGeneration;
		public ulong RequestedFrame;
		public ulong ProducerId;
		public ulong CorrelationId;
		public ulong IdempotencyKey;
	}

	[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi, Pack = 8)]
	internal struct NativeAudioCommandReceipt
	{
		public AudioAbiHeader Header;
		public AudioStatus Status;
		public uint Rejection;
		public ulong BatchSequence;
		public ulong RequestedFrame;
		public ulong CorrelationId;
		public ulong ResolvedPlaybackGeneration;
		[MarshalAs(UnmanagedType.ByValTStr, SizeConst = 192)] public string Diagnostic;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAudioPlaybackState
	{
		public AudioAbiHeader Header;
		public AudioStatus Status;
		public AudioPlaybackState State;
		public ulong PlaybackGeneration;
		public uint Audible;
		public uint Reserved;
		public ulong Frame;
		public ulong LogicalFrame;
		public float Gain;
		public float Pitch;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAudioMixState
	{
		public AudioAbiHeader Header;
		public AudioStatus Status;
		public uint Muted;
		public ulong Generation;
		public float Volume;
		public uint Reserved;
	}

	public readonly record struct AudioCommandReceipt(
		AudioStatus Status,
		ulong BatchSequence,
		ulong RequestedFrame,
		ulong CorrelationId,
		string Diagnostic,
		ulong ResolvedPlaybackGeneration = 0);

	public readonly record struct AudioSourceState(
		AudioStatus Status,
		AudioPlaybackState State,
		ulong PlaybackGeneration,
		bool Audible,
		ulong Frame,
		ulong LogicalFrame,
		float Gain,
		float Pitch);

	public readonly record struct AudioMasterState(
		AudioStatus Status,
		bool Muted,
		float Volume,
		ulong Generation);
}
