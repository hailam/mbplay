#!/bin/bash
# Converts a Metal shader file to a C header with the shader as a string constant
# Usage: metal2header.sh input.metal output.h

if [ $# -ne 2 ]; then
    echo "Usage: $0 input.metal output.h"
    exit 1
fi

INPUT="$1"
OUTPUT="$2"

if [ ! -f "$INPUT" ]; then
    echo "Error: Input file '$INPUT' not found"
    exit 1
fi

# Generate header guard from output filename
GUARD=$(basename "$OUTPUT" | tr '[:lower:]' '[:upper:]' | tr '.' '_')

cat > "$OUTPUT" << 'HEADER_START'
// Auto-generated from mandelbrot.metal - DO NOT EDIT
// Regenerate with: scripts/metal2header.sh src/gpu/mandelbrot.metal src/gpu/mandelbrot_shader.h

HEADER_START

echo "#ifndef ${GUARD}" >> "$OUTPUT"
echo "#define ${GUARD}" >> "$OUTPUT"
echo "" >> "$OUTPUT"
echo "static const char *METAL_SHADER_SOURCE =" >> "$OUTPUT"

# Convert each line to a C string literal
while IFS= read -r line || [ -n "$line" ]; do
    # Escape backslashes and quotes
    escaped=$(echo "$line" | sed 's/\\/\\\\/g' | sed 's/"/\\"/g')
    echo "\"${escaped}\\n\"" >> "$OUTPUT"
done < "$INPUT"

echo ";" >> "$OUTPUT"
echo "" >> "$OUTPUT"
echo "#endif // ${GUARD}" >> "$OUTPUT"

echo "Generated $OUTPUT from $INPUT"
