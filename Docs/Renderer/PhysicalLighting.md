# Physical Lighting & PBR

TerraEngine uses a physically-based lighting model and a Cook-Torrance BRDF to ensure energy-conserving, realistic material rendering.

## Light Units

- **Directional Lights**: Intensity is measured in **Lux** ($lm/m^2$). This represents the illuminance hitting a surface directly. For a bright sun, values around 100,000 lux are typical.
- **Point, Spot, and Area Lights**: Intensity is measured in **Lumens** ($lm$). This represents the total luminous power emitted by the light source in all directions.

## Attenuation

Local lights (Point, Spot, Area) follow the **Inverse-Square Law**. To prevent division by zero, a small epsilon is added:
$$ Attenuation = \frac{1}{d^2 + \epsilon} $$

To smoothly fade the light to zero at its maximum `Range` boundary and avoid hard cutoffs, an Epic Games-style windowing function is applied on top of the inverse-square falloff:
$$ Window = \left( \max\left(0, 1 - \left(\frac{d^2}{range^2}\right)^2\right) \right)^2 $$

## Shading Model (Cook-Torrance BRDF)

The engine uses the industry-standard Cook-Torrance microfacet BRDF for specular reflection:
- **Normal Distribution Function (D)**: Trowbridge-Reitz (GGX). Controls the size and shape of the specular highlight based on roughness.
- **Geometry/Visibility Function (V)**: Smith Joint Masking-Shadowing Function (Schlick-GGX approximation). Accounts for microfacets shadowing each other.
- **Fresnel Function (F)**: Fresnel-Schlick approximation. Increases specular reflectivity at grazing angles, especially for non-metals.

The diffuse component uses a standard energy-conserving Lambertian model divided by $\pi$. Total energy conservation is strictly maintained: $(Diffuse + Specular) \le Incoming Energy$.

## Editor Workflow

The Light Component inspector is organized into intuitive, Unity-style groups:
1. **General**: Light Type, Color, physical Intensity (auto-formatting to lm/lux), and Indirect Multiplier.
2. **Shape**: Falloff Range, and dynamic properties like Inner/Outer Angle (Spot) or Width/Height (Area).
3. **Shadows**: Real-time shadow toggles, bias, and strength.
