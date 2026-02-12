# Mandelbrot Viewer

A high-performance Mandelbrot set explorer for macOS with GPU acceleration via Metal.

## Features

- GPU-accelerated rendering using Apple Metal
- Arbitrary precision zoom using perturbation theory
- Real-time interactive exploration
- Tile-based caching for smooth navigation
- Multiple precision tiers for deep zoom (up to 1024-bit precision)

## Requirements

- macOS 11.0+
- Apple Silicon or Intel Mac with Metal support
- [xmake](https://xmake.io/) build system

## Building

```bash
# Install xmake (if not already installed)
brew install xmake

# Configure and build (release mode)
xmake f -m release
xmake build

# Or build both targets
xmake build mandelbrotplay_c
xmake build mandelbrot_interactive
```

### Build Options

```bash
# Enable strict warnings
xmake f --strict=y

# Enable Address Sanitizer (for debugging)
xmake f --asan=y -m debug

# Enable Thread Sanitizer (for race detection)
xmake f --tsan=y -m debug
```

## Running

### Interactive Viewer

```bash
./build/macosx/arm64/release/mandelbrot_interactive
```

### CLI Generator

```bash
# Generate a 1920x1080 PNG
./build/macosx/arm64/release/mandelbrotplay_c -w 1920 -h 1080 -o output.png

# See all options
./build/macosx/arm64/release/mandelbrotplay_c --help
```

## Controls (Interactive Viewer)

| Key | Action |
|-----|--------|
| Mouse drag | Pan view |
| Scroll wheel | Zoom in/out |
| `+`/`=` | Zoom in |
| `-` | Zoom out |
| `R` | Reset to default view |
| `1-9` | Jump to preset locations |
| `P` | Show preset menu |
| `I` | Toggle HUD |
| `M` | Toggle minimap |
| `C` | Toggle coordinate display |
| `ESC` | Exit |

## Cache

Computed tiles are cached to disk for faster subsequent navigation.

Default location: `~/.mandelbrot/tiles`

Override with environment variable:
```bash
MB_CACHE_DIR=/path/to/cache ./mandelbrot_interactive
```

## License

MIT License. See [LICENSE](LICENSE) for details.

## Third-Party Libraries

See [THIRD_PARTY.md](THIRD_PARTY.md) for a list of dependencies and their licenses.
