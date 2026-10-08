# wayfire-hw-saturation-plugin

A Wayfire plugin that modifies display saturation and brightness directly in hardware by applying color transformations to the active display pipeline.

This project avoids software compositing tricks and instead uses hardware color management when available. It supports both the modern DRM plane color pipeline API (with separate saturation and brightness adjustments) and a legacy CRTC CTM fallback for broader compatibility.

## Dependecies
### Arch Linux
```bash
sudo pacman -S --needed \
base-devel meson ninja pkgconfig \
wayfire libdrm glm
```

## Build and install

```bash
git clone https://github.com/radless01/wayfire-hw-saturation-plugin.git
cd wayfire-hw-saturation-plugin
meson setup build --prefix=/usr --buildtype=release
meson compile -C build
sudo meson install -C build
```

This installs:
- The plugin binary into Wayfire's plugin directory
- The metadata definition into Wayfire's metadata directory

## Configuration

The plugin exposes two independent options:

### Saturation

- **Option**: `hw-saturation/value`
- **Range**: 0.0 to 3.0
- **Default**: 1.0
- **Interpretation**:
  - `1.0` keeps the image unchanged
  - `0.0` produces grayscale
  - Greater than `1.0` increases saturation

### Brightness

- **Option**: `hw-saturation/brightness`
- **Range**: 1.0 to 2.0
- **Default**: 1.0
- **Interpretation**:
  - `1.0` is the default brightness
  - Values above `1.0` increase brightness (scaled by 10% per unit)

### Example configuration

```ini
[core]
plugins = .. hw-saturation ..

[hw-saturation]
value = 1.6
brightness = 1.2
```

## Features

- Per-output saturation and brightness control for Wayfire
- Hardware-accelerated color adjustment via DRM color pipelines
- Dual-path support:
  - **Primary**: DRM plane color pipeline (AMD, newer Intel/NVIDIA)
    - Brightness via MULTIPLIER colorop
    - Saturation via CTM_3x4 colorop
  - **Fallback**: Legacy CRTC CTM (universal DRM/KMS support)
    - Combined saturation and brightness in a single 3×3 matrix
- Saturation range: 0.0 (grayscale) to 3.0 (highly saturated)
- Brightness range: 1.0 (default) to 2.0 (very bright)
- Restores original color configuration when unloaded
- Works with Wayfire 0.11+ and libdrm-based DRM outputs

## How it works

### Saturation Matrix

The plugin computes a 3×3 saturation matrix using the standard luminance formula:

- L = 0.2126 R + 0.7152 G + 0.0722 B
- Output = L + S × (Input - L)

Where:
- S = 0.0 means grayscale
- S = 1.0 means unchanged
- S > 1.0 increases saturation

### Color Pipeline Path

On systems with the DRM plane color pipeline:
1. **MULTIPLIER colorop** applies brightness scaling
2. **CTM_3x4 colorop** applies the saturation matrix
3. Both are applied atomically in the hardware display pipeline

### Fallback CTM Path

On systems without plane color pipelines:
1. A 3×3 transformation matrix combines both saturation and brightness
2. Applied to the CRTC's CTM property
3. Provides compatibility with a wider range of hardware

## Requirements

- Wayfire >= 0.11.0
- libdrm >= 2.4.120
- A Linux system using Wayfire with DRM/KMS-backed outputs

### Hardware Support

- **Color Pipeline**: AMD GPUs with RDNA architecture, newer Intel Arc, recent NVIDIA (requires driver support)
- **CRTC CTM Fallback**: Most modern GPUs and displays that support DRM CTM properties

## Repository layout

```text
.
├── LICENSE
├── README.md
├── meson.build
├── metadata/
│   └── hw-saturation.xml
└── src/
    └── main.cpp
```

## Implementation Details

### Color Pipeline Discovery

When using the plane color pipeline path, the plugin:
1. Discovers the active primary plane for the CRTC
2. Enumerates available color pipelines
3. Finds pipelines containing both MULTIPLIER and CTM_3x4 colorops
4. Discovers all colorops in the chain to properly bypass unused ones

### Property Management

All colorops in the selected chain are explicitly bypassed in the atomic request, with only MULTIPLIER and CTM_3x4 enabled. This is required by the DRM color-pipeline API and ensures compatibility across supported hardware.

### DRM Property Encoding

Values are encoded as DRM S31.32 fixed-point format:
- Bit 63: Sign bit
- Bits 62..0: Magnitude (32 bits integer + 31 bits fractional)

## Notes

- This plugin operates at the KMS/DRM layer for true hardware color adjustment
- Most useful on systems where the GPU and display stack support the relevant color properties
- If neither color pipeline nor CRTC CTM is available, the plugin logs failures and remains gracefully disabled
- Brightness values above 1.0 increase brightness gradually (10% per unit increase)
- Saturation is independent of brightness and can be combined with any brightness value

## Troubleshooting

### Color pipeline not available

If your GPU doesn't support the plane color pipeline, the plugin automatically falls back to CRTC CTM. Check logs for:
```
[hw_saturation] SUCCESS: CRTC CTM committed
```

### Properties not found

Check that your DRM/KMS driver supports either:
- Plane color pipelines (COLOR_PIPELINE property)
- Or CRTC CTM property

Use `drm_info` or `modetest` from libdrm to inspect available properties:
```bash
modetest -M -p  # Shows properties for all DRM objects
```

## License

This project is licensed under the GNU General Public License v3.0 or later.
See the [LICENSE](LICENSE) file for details.
