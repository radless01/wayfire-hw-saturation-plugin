# wayfire-hw-saturation

A Wayfire plugin that adjusts display saturation in hardware by applying a DRM/KMS color transformation matrix (CTM) to the output CRTC.

This project does not do software post-processing in the compositor. Instead, it writes the color transform directly to the hardware path when supported, allowing saturation changes to happen at the DRM layer.

## Features

- Per-output saturation control for Wayfire
- Hardware-accelerated saturation via DRM CTM
- Supports values from 0.0 to 3.0
- Restores the original CTM when the plugin is unloaded or the output is destroyed
- Works with Wayfire 0.11+ and libdrm-based DRM outputs

## What the plugin does

The plugin computes a 3x3 saturation matrix based on the luminance formula:

- L = 0.2126 R + 0.7152 G + 0.0722 B
- Output = L + S * (Input - L)

Where:

- S = 0.0 means grayscale
- S = 1.0 means unchanged
- S > 1.0 increases saturation

This is applied atomically using the DRM `CTM` property on the relevant CRTC.

## Requirements

- Wayfire >= 0.11.0
- libdrm >= 2.4.120
- A DRM/KMS-backed output that exposes a CRTC CTM property
- A compositor environment running Wayfire on a supported Linux system

## Building

```bash
git clone https://github.com/radless01/wayfire-hw-saturation.git
cd wayfire-hw-saturation
meson setup build
ninja -C build
sudo ninja -C build install
```

The project installs both:

- the compiled plugin under Wayfire's plugin directory
- the metadata XML file under Wayfire's metadata directory

## Configuration

The plugin exposes the option:

- `hw_saturation/value`

Valid range:

- minimum: `0.0`
- maximum: `3.0`
- default: `1.0`

Example Wayfire configuration:

```ini
[core]
plugins = .. hw_saturation ..

[hw_saturation]
value = 1.6
```

A value of `1.0` leaves colors unchanged, `0.0` produces grayscale, and values above `1.0` increase saturation.

## Project structure

```text
.
├── meson.build
├── metadata/
│   ├── hw_saturation.xml
│   └── meson.build
└── src/
    ├── hw_saturation.cpp
    └── meson.build
```

## Notes

- This plugin operates at the KMS/DRM layer and is intended for hardware-aware color adjustment.
- It is most useful on systems where the GPU/monitor stack supports CTM operations and the compositor is using DRM-backed outputs.
- If the target output does not expose a CTM property, the plugin will fail gracefully and log an error.

## License

This repository does not currently include an explicit license file.
