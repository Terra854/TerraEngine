# Animation recording gate output

This fixture creates the exact `Assets/Captured` parent directory used by the
installed-editor recording gates. The gate copies this file before the editor
starts, so Play Mode may publish only the separately declared `.teanim` output
without granting permission to create arbitrary directories or overwrite an
existing asset.

The README itself is part of the protected pre-Play authored-state inventory and
must remain byte-identical after Play Mode exits.
