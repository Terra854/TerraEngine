// AudioSource.cs - generation-safe gameplay controls for AudioSourceComponent.

using System.Threading;

namespace TerraEngine.Audio
{
	public sealed class AudioSource : Component
	{
		private static long s_nextCorrelation;
		private NativeAudioSourceBinding _binding;
		private bool _bound;

		internal override void OnObjectAssigned()
		{
			// Binding copies the protected source attachment. An early lookup may
			// precede capture, so retry only until the first successful bind.
			TryBind();
		}

		private AudioStatus TryBind()
		{
			if (_bound) return AudioStatus.Success;
			_binding = new() { Header = AudioAbiHeader.For<NativeAudioSourceBinding>() };
			_binding.Status = InternalCalls.Native_Audio_BindSource(Object.ID, ref _binding);
			_bound = _binding.Status == AudioStatus.Success;
			return _binding.Status;
		}

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

		// The engine allocates Play IDs and freezes the current instance when
		// admitting controls. A new facade therefore controls observed autoplay.
		public AudioCommandReceipt Play() => Submit(AudioCommandKind.Play);
		public AudioCommandReceipt Pause() => Submit(AudioCommandKind.Pause);
		public AudioCommandReceipt Resume() => Submit(AudioCommandKind.Resume);
		public AudioCommandReceipt Stop() => Submit(AudioCommandKind.Stop);

		public AudioSourceState ReadState()
		{
			NativeAudioPlaybackState state = new() { Header = AudioAbiHeader.For<NativeAudioPlaybackState>() };
			state.Status = TryBind();
			if (_bound) state.Status = InternalCalls.Native_Audio_ReadPlayback(ref _binding, ref state);
			return new(state.Status, state.State, state.PlaybackGeneration, state.Audible != 0,
				state.Frame, state.LogicalFrame, state.Gain, state.Pitch);
		}

		private AudioCommandReceipt Submit(AudioCommandKind kind)
		{
			AudioStatus bindingStatus = TryBind();
			if (!_bound) return new(bindingStatus, 0, 0, 0, "Audio source has not been captured.");
			ulong correlation = checked((ulong)Interlocked.Increment(ref s_nextCorrelation));
			NativeAudioCommandRequest request = new()
			{
				Header = AudioAbiHeader.For<NativeAudioCommandRequest>(),
				Target = _binding.Target,
				TargetMode = AudioTargetMode.BindCurrentAtAdmission,
				Kind = kind,
				ProducerId = 1,
				CorrelationId = correlation,
				IdempotencyKey = correlation
			};
			NativeAudioCommandReceipt receipt = new() { Header = AudioAbiHeader.For<NativeAudioCommandReceipt>() };
			receipt.Status = InternalCalls.Native_Audio_SubmitCommand(ref request, ref receipt);
			return new(receipt.Status, receipt.BatchSequence, receipt.RequestedFrame,
				receipt.CorrelationId, receipt.Diagnostic ?? string.Empty, receipt.ResolvedPlaybackGeneration);
		}
	}
}
