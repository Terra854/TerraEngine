// AudioSource.cs - generation-safe gameplay controls for AudioSourceComponent.

using System.Threading;

namespace TerraEngine.Audio
{
	public sealed class AudioSource : Component
	{
		private static long s_nextCorrelation;
		private uint _playbackGeneration;

		public string Clip
		{
			get => GetStringProperty("clipAsset");
			set => SetStringProperty("clipAsset", value ?? string.Empty);
		}

		public float Volume
		{
			get => GetProperty<float>("volume");
			set => SetProperty("volume", value);
		}

		public bool Loop
		{
			get => GetProperty<bool>("loop");
			set => SetProperty("loop", value);
		}

		public AudioCommandReceipt Play() => Submit(AudioCommandKind.Play, ++_playbackGeneration);
		public AudioCommandReceipt Pause() => Submit(AudioCommandKind.Pause, _playbackGeneration);
		public AudioCommandReceipt Resume() => Submit(AudioCommandKind.Resume, _playbackGeneration);
		public AudioCommandReceipt Stop() => Submit(AudioCommandKind.Stop, _playbackGeneration);

		public AudioSourceState ReadState()
		{
			NativeAudioPlaybackState state = new() { Header = AudioAbiHeader.For<NativeAudioPlaybackState>() };
			state.Status = InternalCalls.Native_Audio_ReadPlayback(
				Object.ID,
				checked((uint)InternalCalls.Native_Object_GetComponentGeneration(Object.ID)),
				ref state);
			return new(state.Status, state.State, state.PlaybackGeneration, state.Audible != 0,
				state.Frame, state.LogicalFrame, state.Gain, state.Pitch);
		}

		private AudioCommandReceipt Submit(AudioCommandKind kind, uint playbackGeneration)
		{
			ulong correlation = checked((ulong)Interlocked.Increment(ref s_nextCorrelation));
			NativeAudioCommandRequest request = new()
			{
				Header = AudioAbiHeader.For<NativeAudioCommandRequest>(),
				ObjectEntity = Object.ID,
				ComponentGeneration = checked((uint)InternalCalls.Native_Object_GetComponentGeneration(Object.ID)),
				PlaybackGeneration = playbackGeneration,
				Kind = kind,
				ProducerId = 1,
				CorrelationId = correlation,
				IdempotencyKey = correlation
			};
			NativeAudioCommandReceipt receipt = new() { Header = AudioAbiHeader.For<NativeAudioCommandReceipt>() };
			receipt.Status = InternalCalls.Native_Audio_SubmitCommand(ref request, ref receipt);
			return new(receipt.Status, receipt.BatchSequence, receipt.RequestedFrame,
				receipt.CorrelationId, receipt.Diagnostic ?? string.Empty);
		}
	}
}
