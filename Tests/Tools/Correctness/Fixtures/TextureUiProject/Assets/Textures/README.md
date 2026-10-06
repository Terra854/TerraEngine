# Generated texture witnesses

After configuring, building and installing the repository, run the installed `Tests/Tools/Correctness/TerraEngineTextureUiFixtureGenerator.exe --output-root <absolute fresh directory>`. Generate into a fresh staging directory and copy its eight files into the copied fixture project's `Assets/Textures` directory. The generator rejects a nonempty destination and never deletes existing assets.

All files contain legal UASTC solid-color blocks. Expected pixels are authored directly by coordinates, independently of the runtime decoder and row-copy code. Coordinates start at the top-left of mip zero.

| File | Dimensions | Authored pixels / descriptor |
| --- | --- | --- |
| witness-linear.tetf | 8×4 | Four red columns, then four blue columns; `(4,0)` is blue. |
| quadrants-linear.tetf | 8×8 | 4×4 red, blue, green, yellow quadrants in reading order. |
| edges-linear.tetf | 5×3 | Four red columns, one blue column; padding is excluded. |
| alpha-linear.tetf | 8×8 | Same quadrants, with alpha 0, 64, 128, 255 respectively. |
| midtone-srgb.tetf | 8×4 | Left RGBA `(64,128,192,255)`; right `(192,96,32,128)`; sRGB. |
| mips-linear.tetf | 8×4, 4×2, 2×1, 1×1 | Solid RGBA `(31,63,95,127)`, `(96,128,160,64)`, `(180,40,90,200)`, `(17,33,65,0)` respectively. |
| control-bc7.tetf | 8×8 | Same quadrants; requested BC7 format 145. |
| control-astc.tetf | 8×8 | Same quadrants; requested ASTC format 157. |

Linear witnesses request RGBA format 37 and the sRGB witness requests 43, so ordinary ResourceManager/UI loading selects uncompressed RGBA output. This preserves the installed GPU witness of row placement. Compressed controls retain 4×4 blocks of 16 bytes; unsupported adapter formats must be reported separately.

CPU tests require exact RGBA bytes, cropped rows, contiguous mip offsets, and descriptors. GPU samples must account for sRGB decoding, alpha blending and the fixture background; compare interior texels without filtering across color boundaries. Allow two output-channel units for uncompressed presentation and eight for compressed controls after the stated color/blending conversion. Scaled screenshots establish visible UI behavior, not exact texture bytes; retain backend pixel evidence separately.

The final UI row presents the uploaded mip chain at 128x64, 4x2, 2x1 and 1x1 display sizes. Compare backend pixels to the authored mip colors, with sampler filtering and alpha blending accounted for; tiny controls are not a screenshot-label oracle.
