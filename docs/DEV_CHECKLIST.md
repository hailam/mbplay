# Developer Checklist

## Build Commands

### Debug Build
```bash
xmake f -m debug
xmake build mandelbrotplay_c
xmake build mandelbrot_interactive
```

### Release Build
```bash
xmake f -m release
xmake build mandelbrotplay_c
xmake build mandelbrot_interactive
```

### Strict Mode (all warnings enabled)
```bash
xmake f --strict=y
xmake build
```

### Strict + Warnings as Errors
```bash
xmake f --strict=y --werror=y
xmake build
```

## Sanitizer Testing

### Address Sanitizer (memory safety)
```bash
xmake f --asan=y -m debug
xmake build mandelbrot_interactive
./build/macosx/arm64/debug/mandelbrot_interactive
```

### Thread Sanitizer (data races)
```bash
xmake f --tsan=y -m debug
xmake build mandelbrot_interactive
./build/macosx/arm64/debug/mandelbrot_interactive
```

Note: ASan and TSan cannot be used together. Run them separately.

## Running the Applications

### CLI Target (mandelbrotplay_c)
```bash
# Generate a PNG image
./build/macosx/arm64/release/mandelbrotplay_c -w 1920 -h 1080 -o output.png

# With viewer
./build/macosx/arm64/release/mandelbrotplay_c -w 800 -h 600

# Headless (no viewer)
./build/macosx/arm64/release/mandelbrotplay_c -n -o output.png
```

### Interactive Viewer (mandelbrot_interactive)
```bash
./build/macosx/arm64/release/mandelbrot_interactive
```

## Tile Cache Location

Default cache path: `~/.mandelbrot/tiles`

Override with environment variable:
```bash
MB_CACHE_DIR=/custom/path ./mandelbrot_interactive
```

macOS default fallback: `~/Library/Caches/Mandelbrot/tiles`

## Build Options Reference

| Option | Default | Description |
|--------|---------|-------------|
| `--strict` | `n` | Enable strict warnings |
| `--werror` | `n` | Treat warnings as errors |
| `--asan` | `n` | Address/UB sanitizer |
| `--tsan` | `n` | Thread sanitizer |
| `--native` | `y` | -march=native optimization |
| `--lto` | `y` (release) | Link-time optimization |

Note: `--native` and `--lto` are automatically disabled when sanitizers are active.

## Keyboard Shortcuts (Interactive Viewer)

| Key | Action |
|-----|--------|
| `R` | Reset view |
| `+`/`=` | Zoom in |
| `-` | Zoom out |
| Arrow keys | Pan |
| `I` | Toggle HUD |
| `M` | Toggle minimap |
| `C` | Toggle coordinates |
| `P` | Toggle preset menu |
| `1-9` | Jump to preset location |
| `ESC` | Close preset menu / Exit |
