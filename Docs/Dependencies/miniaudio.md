# miniaudio dependency

TerraEngine Audio uses unmodified miniaudio **0.11.26**, pinned to commit
`877ea4a7c2601c7b0f2a44e83dfac42e6470fea6`. The implementation is private to the
`terra.miniaudio` provider. TerraEngine's private implementation wrapper includes
the bundled `extras/stb_vorbis.c` declarations before compiling miniaudio, while
the full stb_vorbis implementation is linked once from the pinned dependency.
This ordering is required for miniaudio to register Ogg Vorbis decoding. No SDK
headers or provider-private types are installed.

The dependency is distributed under MIT-0 (see its upstream license). Runtime
backends use miniaudio's native WASAPI, ALSA, and CoreAudio facilities as available;
tests use the provider-neutral fake backend and do not require an audio device.
The install tree carries this notice and does not expose miniaudio headers.
