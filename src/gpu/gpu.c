#include "gpu.h"
#include "../color/color.h"

#ifdef MB_GPU_METAL

#include <cmt/cmt.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// =============================================================================
// Embedded Metal Shader Source (compiled at runtime)
// =============================================================================

static const char *METAL_SHADER_SOURCE =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"\n"
"struct ComputeParams {\n"
"    float cx_scale;\n"
"    float cy_scale;\n"
"    float width_half;\n"
"    float height_half;\n"
"    uint max_iter;\n"
"    uint width;\n"
"    uint height;\n"
"};\n"
"\n"
"static bool is_in_cardioid_or_bulb(float cx, float cy) {\n"
"    float cx_shifted = cx - 0.25f;\n"
"    float cy2 = cy * cy;\n"
"    float q = cx_shifted * cx_shifted + cy2;\n"
"    if (q * (q + cx_shifted) <= 0.25f * cy2) return true;\n"
"    float cx_plus1 = cx + 1.0f;\n"
"    if (cx_plus1 * cx_plus1 + cy2 <= 0.0625f) return true;\n"
"    return false;\n"
"}\n"
"\n"
"kernel void mandelbrot_compute(\n"
"    device uint *iterations [[buffer(0)]],\n"
"    constant ComputeParams &params [[buffer(1)]],\n"
"    uint2 gid [[thread_position_in_grid]]\n"
") {\n"
"    if (gid.x >= params.width || gid.y >= params.height) return;\n"
"    float cx = (float(gid.x) - params.width_half) * params.cx_scale;\n"
"    float cy = (float(gid.y) - params.height_half) * params.cy_scale;\n"
"    uint iteration = params.max_iter;\n"
"    if (!is_in_cardioid_or_bulb(cx, cy)) {\n"
"        float zx = 0.0f, zy = 0.0f, zx2 = 0.0f, zy2 = 0.0f;\n"
"        iteration = 0;\n"
"        while (zx2 + zy2 < 4.0f && iteration < params.max_iter) {\n"
"            zy = 2.0f * zx * zy + cy;\n"
"            zx = zx2 - zy2 + cx;\n"
"            zx2 = zx * zx;\n"
"            zy2 = zy * zy;\n"
"            iteration++;\n"
"        }\n"
"    }\n"
"    iterations[gid.y * params.width + gid.x] = iteration;\n"
"}\n"
"\n"
"kernel void mandelbrot_compute_row(\n"
"    device uint *iterations [[buffer(0)]],\n"
"    constant ComputeParams &params [[buffer(1)]],\n"
"    constant uint &row_y [[buffer(2)]],\n"
"    uint gid [[thread_position_in_grid]]\n"
") {\n"
"    if (gid >= params.width) return;\n"
"    float cx = (float(gid) - params.width_half) * params.cx_scale;\n"
"    float cy = (float(row_y) - params.height_half) * params.cy_scale;\n"
"    uint iteration = params.max_iter;\n"
"    if (!is_in_cardioid_or_bulb(cx, cy)) {\n"
"        float zx = 0.0f, zy = 0.0f, zx2 = 0.0f, zy2 = 0.0f;\n"
"        iteration = 0;\n"
"        while (zx2 + zy2 < 4.0f && iteration < params.max_iter) {\n"
"            zy = 2.0f * zx * zy + cy;\n"
"            zx = zx2 - zy2 + cx;\n"
"            zx2 = zx * zx;\n"
"            zy2 = zy * zy;\n"
"            iteration++;\n"
"        }\n"
"    }\n"
"    iterations[gid] = iteration;\n"
"}\n";

// =============================================================================
// Compute Parameters (must match Metal shader struct)
// =============================================================================

typedef struct {
    float cx_scale;
    float cy_scale;
    float width_half;
    float height_half;
    uint32_t max_iter;
    uint32_t width;
    uint32_t height;
} ComputeParams;

// =============================================================================
// GPU State
// =============================================================================

static MtDevice *device = NULL;
static MtCommandQueue *commandQueue = NULL;
static MtLibrary *library = NULL;
static MtComputePipelineState *pipelineFull = NULL;
static MtComputePipelineState *pipelineRow = NULL;
static MtBuffer *iterBuffer = NULL;
static MtBuffer *rowIterBuffer = NULL;
static MtBuffer *paramsBuffer = NULL;
static MtBuffer *rowIndexBuffer = NULL;

static MBConfig gpuConfig;

// =============================================================================
// GPU API Implementation
// =============================================================================

bool gpu_is_available(void) {
    MtDevice *d = mtCreateSystemDefaultDevice();
    bool available = (d != NULL);
    if (d) {
        mtRelease(d);
    }
    return available;
}

int gpu_init(const MBConfig *cfg) {
    gpuConfig = *cfg;

    // Create Metal device
    device = mtCreateSystemDefaultDevice();
    if (!device) {
        return -1;
    }

    // Create command queue
    commandQueue = mtNewCommandQueue(device);
    if (!commandQueue) {
        gpu_cleanup();
        return -1;
    }

    // Compile shader source at runtime
    NsError *error = NULL;
    library = mtNewLibraryWithSource(device, (char*)METAL_SHADER_SOURCE, NULL, &error);
    if (!library) {
        fprintf(stderr, "Metal shader compilation failed\n");
        gpu_cleanup();
        return -1;
    }

    // Create compute pipeline for full image
    MtFunction *kernelFull = mtNewFunctionWithName(library, "mandelbrot_compute");
    if (kernelFull) {
        pipelineFull = mtNewComputePipelineStateWithFunction(device, kernelFull, NULL);
        mtRelease(kernelFull);
    }

    // Create compute pipeline for row-by-row
    MtFunction *kernelRow = mtNewFunctionWithName(library, "mandelbrot_compute_row");
    if (kernelRow) {
        pipelineRow = mtNewComputePipelineStateWithFunction(device, kernelRow, NULL);
        mtRelease(kernelRow);
    }

    if (!pipelineFull && !pipelineRow) {
        gpu_cleanup();
        return -1;
    }

    // Allocate buffers
    size_t fullIterSize = (size_t)cfg->width * cfg->height * sizeof(uint32_t);
    size_t rowIterSize = (size_t)cfg->width * sizeof(uint32_t);

    iterBuffer = mtDeviceNewBufferWithLength(device, fullIterSize, MtResourceStorageModeShared);
    rowIterBuffer = mtDeviceNewBufferWithLength(device, rowIterSize, MtResourceStorageModeShared);
    paramsBuffer = mtDeviceNewBufferWithLength(device, sizeof(ComputeParams), MtResourceStorageModeShared);
    rowIndexBuffer = mtDeviceNewBufferWithLength(device, sizeof(uint32_t), MtResourceStorageModeShared);

    if (!iterBuffer || !rowIterBuffer || !paramsBuffer || !rowIndexBuffer) {
        gpu_cleanup();
        return -1;
    }

    // Initialize params buffer
    ComputeParams *params = mtBufferContents(paramsBuffer);
    params->cx_scale = (float)cfg->cx_scale;
    params->cy_scale = (float)cfg->cy_scale;
    params->width_half = (float)cfg->width_half;
    params->height_half = (float)cfg->height_half;
    params->max_iter = (uint32_t)cfg->max_iter;
    params->width = (uint32_t)cfg->width;
    params->height = (uint32_t)cfg->height;

    return 0;
}

void gpu_compute_row(int y, PixelColor *output) {
    if (!pipelineRow) {
        return;
    }

    // Set row index
    uint32_t *rowY = mtBufferContents(rowIndexBuffer);
    *rowY = (uint32_t)y;

    // Create command buffer and encoder
    MtCommandBuffer *cmdBuffer = mtNewCommandBuffer(commandQueue);
    MtComputeCommandEncoder *encoder = mtNewComputeCommandEncoder(cmdBuffer);

    mtComputeCommandEncoderSetComputePipelineState(encoder, pipelineRow);
    mtComputeCommandEncoderSetBufferOffsetAtIndex(encoder, rowIterBuffer, 0, 0);
    mtComputeCommandEncoderSetBufferOffsetAtIndex(encoder, paramsBuffer, 0, 1);
    mtComputeCommandEncoderSetBufferOffsetAtIndex(encoder, rowIndexBuffer, 0, 2);

    // Dispatch threads for one row
    MtSize gridSize = {(NsUInteger)gpuConfig.width, 1, 1};
    MtSize threadgroupSize = {256, 1, 1};  // 256 threads per group for 1D
    mtComputeCommandEncoderDispatchThread_threadsPerThreadgroup(encoder, gridSize, threadgroupSize);

    mtComputeCommandEncoderEndEncoding(encoder);
    mtCommandBufferCommit(cmdBuffer);
    mtCommandBufferWaitUntilCompleted(cmdBuffer);

    // Apply color mapping on CPU
    uint32_t *iterations = mtBufferContents(rowIterBuffer);
    for (int x = 0; x < gpuConfig.width; x++) {
        color_from_iteration(&output[x], iterations[x]);
    }

    mtRelease(cmdBuffer);
}

void gpu_compute_full(PixelColor *output) {
    if (!pipelineFull) {
        return;
    }

    // Create command buffer and encoder
    MtCommandBuffer *cmdBuffer = mtNewCommandBuffer(commandQueue);
    MtComputeCommandEncoder *encoder = mtNewComputeCommandEncoder(cmdBuffer);

    mtComputeCommandEncoderSetComputePipelineState(encoder, pipelineFull);
    mtComputeCommandEncoderSetBufferOffsetAtIndex(encoder, iterBuffer, 0, 0);
    mtComputeCommandEncoderSetBufferOffsetAtIndex(encoder, paramsBuffer, 0, 1);

    // Dispatch threads for full image
    MtSize gridSize = {(NsUInteger)gpuConfig.width, (NsUInteger)gpuConfig.height, 1};
    MtSize threadgroupSize = {16, 16, 1};  // 16x16 = 256 threads per group
    mtComputeCommandEncoderDispatchThread_threadsPerThreadgroup(encoder, gridSize, threadgroupSize);

    mtComputeCommandEncoderEndEncoding(encoder);
    mtCommandBufferCommit(cmdBuffer);
    mtCommandBufferWaitUntilCompleted(cmdBuffer);

    // Apply color mapping on CPU
    uint32_t *iterations = mtBufferContents(iterBuffer);
    size_t totalPixels = (size_t)gpuConfig.width * gpuConfig.height;
    for (size_t i = 0; i < totalPixels; i++) {
        color_from_iteration(&output[i], iterations[i]);
    }

    mtRelease(cmdBuffer);
}

void gpu_cleanup(void) {
    if (rowIndexBuffer) {
        mtRelease(rowIndexBuffer);
        rowIndexBuffer = NULL;
    }
    if (rowIterBuffer) {
        mtRelease(rowIterBuffer);
        rowIterBuffer = NULL;
    }
    if (iterBuffer) {
        mtRelease(iterBuffer);
        iterBuffer = NULL;
    }
    if (paramsBuffer) {
        mtRelease(paramsBuffer);
        paramsBuffer = NULL;
    }
    if (pipelineRow) {
        mtRelease(pipelineRow);
        pipelineRow = NULL;
    }
    if (pipelineFull) {
        mtRelease(pipelineFull);
        pipelineFull = NULL;
    }
    if (library) {
        mtRelease(library);
        library = NULL;
    }
    if (commandQueue) {
        mtRelease(commandQueue);
        commandQueue = NULL;
    }
    if (device) {
        mtRelease(device);
        device = NULL;
    }
}

#else // !MB_GPU_METAL

#include <stdio.h>

// Stub implementations for non-Metal platforms

bool gpu_is_available(void) {
    return false;
}

int gpu_init(const MBConfig *cfg) {
    (void)cfg;
    return -1;
}

void gpu_compute_row(int y, PixelColor *output) {
    (void)y;
    (void)output;
}

void gpu_compute_full(PixelColor *output) {
    (void)output;
}

void gpu_cleanup(void) {
}

#endif // MB_GPU_METAL
