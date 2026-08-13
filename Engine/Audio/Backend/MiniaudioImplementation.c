/*
 * MiniaudioImplementation.c - private miniaudio implementation assembly.
 *
 * stb_vorbis exposes its declarations when STB_VORBIS_HEADER_ONLY is defined.
 * They must be visible before miniaudio's implementation is compiled; otherwise
 * miniaudio omits its Vorbis decoding backend even if stb_vorbis is linked as a
 * separate object. The full stb_vorbis implementation remains compiled once by
 * the dependency target, while this file supplies declarations only.
 */
#define STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
