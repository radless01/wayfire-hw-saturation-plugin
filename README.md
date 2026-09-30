# wayfire-hw-saturation

A Wayfire plugin that modifies display saturation directly in hardware by applying a DRM/KMS color transformation matrix (CTM) to the active CRTC.

This project avoids software compositing tricks and instead writes the color adjustment to the hardware path when the output exposes the necessary DRM properties. The result is a low-latency saturation control for Wayfire.

## Features

- Per-output saturation control for Wayfire
- Hardware-accelerated color adjustment via DRM CTM
- Supports saturation values from 0.0 to 3.0
- Restores the original CTM when the plugin is unloaded or the output is destroyed
- Works with Wayfire 0.11+ and libdrm-based DRM outputs

## How it works

The plugin computes a 3x3 saturation matrix using the luminance formula:

- L = 0.2126 R + 0.7152 G + 0.0722 B
- Output = L + S * (Input - L)

Where:

- S = 0.0 means grayscale
- S = 1.0 means unchanged
- S > 1.0 increases saturation

The matrix is then applied atomically using the DRM `CTM` property on the relevant CRTC.

## Requirements

- Wayfire >= 0.11.0
- libdrm >= 2.4.120
- A DRM/KMS-backed output that exposes a CRTC CTM property
- A Linux system using Wayfire

## Build and install

```bash
git clone https://github.com/radless01/wayfire-hw-saturation.git
cd wayfire-hw-saturation
meson setup build
ninja -C build
sudo ninja -C build install
```

This installs:

- the plugin binary into Wayfire's plugin directory
- the metadata definition into Wayfire's metadata directory

## Configuration

The plugin exposes the option:

- `hw_saturation/value`

Valid range:

- minimum: `0.0`
- maximum: `3.0`
- default: `1.0`

Example configuration:

```ini
[core]
plugins = .. hw_saturation ..

[hw_saturation]
value = 1.6
```

Interpretation:

- `1.0` keeps the image unchanged
- `0.0` produces grayscale
- greater than `1.0` increases saturation

## Repository layout

```text
.
├── LICENSE
├── README.md
├── meson.build
├── metadata/
│   ├── hw_saturation.xml
│   └── meson.build
└── src/
    ├── hw_saturation.cpp
    └── meson.build
```

## Notes

- This plugin operates at the KMS/DRM layer and is meant for hardware-aware color adjustment.
- It is most useful on systems where the GPU and display stack support CTM operations.
- If the target output does not expose a CTM property, the plugin logs the failure and exits gracefully.

## License

This project is licensed under the GNU General Public License v3.0 or later.
See the [LICENSE](LICENSE) file for details.
