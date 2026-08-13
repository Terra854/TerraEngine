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

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct AudioAbiHeader
	{
		internal const uint Version = 1;
		public uint AbiVersion;
		public uint StructSize;
		public static AudioAbiHeader For<T>() where T : struct => new()
		{
			AbiVersion = Version,
			StructSize = checked((uint)Marshal.SizeOf<T>())
		};
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAudioCommandRequest
	{
		public AudioAbiHeader Header;
		public uint ObjectEntity;
		public uint ComponentGeneration;
		public uint PlaybackGeneration;
		public AudioCommandKind Kind;
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
		[MarshalAs(UnmanagedType.ByValTStr, SizeConst = 192)] public string Diagnostic;
	}

	[StructLayout(LayoutKind.Sequential, Pack = 8)]
	internal struct NativeAudioPlaybackState
	{
		public AudioAbiHeader Header;
		public AudioStatus Status;
		public AudioPlaybackState State;
		public uint PlaybackGeneration;
		public uint Audible;
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
		string Diagnostic);

	public readonly record struct AudioSourceState(
		AudioStatus Status,
		AudioPlaybackState State,
		uint PlaybackGeneration,
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
