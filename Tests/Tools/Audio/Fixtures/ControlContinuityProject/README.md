# Installed audio gameplay fixture

Use the installed `RunInstalledAudioControlAcceptance.ps1`; it copies this project
into a fresh work directory and provides matching installed managed references.
The scene imports `Assets/Audio/Continuity.wav` with the pinned GUID in its `.meta`
file. This one-second, mono PCM16, 8 kHz clip contains the repeated exact sample
sequence `0, 1024, 0, -1024`, loops, and needs no network asset or system library.

The real `OnUpdate` callback observes audible autoplay and positive-delta cursor
advancement before a fresh facade calls Pause without Play. The initial stages
compile against bridge v1 and retain its rejected/stuck pause evidence; the full
acceptance requires v2. Later stages prove resume, stop, same-boundary Play/Pause,
another producer's Play, component replacement and rejection of the old facade.
Missing provider, clip, callback or stage is incomplete acceptance, never success.
