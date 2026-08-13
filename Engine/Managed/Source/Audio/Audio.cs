// Audio.cs - project-wide managed master-mix controls.

namespace TerraEngine.Audio
{
	public static class Audio
	{
		public static AudioMasterState Master
		{
			get
			{
				NativeAudioMixState state = new() { Header = AudioAbiHeader.For<NativeAudioMixState>() };
				state.Status = InternalCalls.Native_Audio_ReadMasterMix(ref state);
				return new(state.Status, state.Muted != 0, state.Volume, state.Generation);
			}
		}

		public static AudioStatus SetMaster(float volume, bool muted)
		{
			NativeAudioMixState state = new()
			{
				Header = AudioAbiHeader.For<NativeAudioMixState>(),
				Volume = volume,
				Muted = muted ? 1U : 0U
			};
			return InternalCalls.Native_Audio_SetMasterMix(ref state);
		}
	}
}
