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
"}\n"
"\n"
"struct TileParams {\n"
"    float center_x;\n"
"    float center_y;\n"
"    float scale;\n"
"    uint tile_x;\n"
"    uint tile_y;\n"
"    uint tile_size;\n"
"    uint max_iter;\n"
"    uint vp_half_w;\n"
"    uint vp_half_h;\n"
"};\n"
"\n"
"kernel void mandelbrot_compute_tile(\n"
"    device uint *iterations [[buffer(0)]],\n"
"    constant TileParams &params [[buffer(1)]],\n"
"    uint2 gid [[thread_position_in_grid]]\n"
") {\n"
"    if (gid.x >= params.tile_size || gid.y >= params.tile_size) return;\n"
"    float px = float(params.tile_x + gid.x);\n"
"    float py = float(params.tile_y + gid.y);\n"
"    float cx = params.center_x + (px - float(params.vp_half_w)) * params.scale;\n"
"    float cy = params.center_y + (py - float(params.vp_half_h)) * params.scale;\n"
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
"    iterations[gid.y * params.tile_size + gid.x] = iteration;\n"
"}\n"
"\n"
"struct PerturbParams {\n"
"    float center_x, center_y, scale;\n"
"    float ref_cx, ref_cy;\n"
"    uint tile_x, tile_y, tile_size, max_iter;\n"
"    uint ref_escape_iter, vp_half_w, vp_half_h;\n"
"};\n"
"\n"
"kernel void mandelbrot_compute_perturb(\n"
"    device uint *iterations [[buffer(0)]],\n"
"    constant PerturbParams &params [[buffer(1)]],\n"
"    constant float2 *ref_orbit [[buffer(2)]],\n"
"    uint2 gid [[thread_position_in_grid]]\n"
") {\n"
"    if (gid.x >= params.tile_size || gid.y >= params.tile_size) return;\n"
"\n"
"    float px = float(params.tile_x + gid.x);\n"
"    float py = float(params.tile_y + gid.y);\n"
"\n"
"    // Delta from reference: dC = C_pixel - C_ref\n"
"    float delta_cx = params.center_x + (px - float(params.vp_half_w)) * params.scale - params.ref_cx;\n"
"    float delta_cy = params.center_y + (py - float(params.vp_half_h)) * params.scale - params.ref_cy;\n"
"\n"
"    float delta_x = 0.0f, delta_y = 0.0f;\n"
"    uint iteration = 0;\n"
"    uint max_safe = min(params.max_iter, params.ref_escape_iter);\n"
"\n"
"    while (iteration < max_safe) {\n"
"        float2 z_ref = ref_orbit[iteration];\n"
"        float zx = z_ref.x + delta_x;\n"
"        float zy = z_ref.y + delta_y;\n"
"\n"
"        if (zx*zx + zy*zy >= 4.0f) break;\n"
"\n"
"        // d_n+1 = 2*Z_ref*d + d^2 + dC\n"
"        float new_dx = 2.0f*(z_ref.x*delta_x - z_ref.y*delta_y)\n"
"                     + delta_x*delta_x - delta_y*delta_y + delta_cx;\n"
"        float new_dy = 2.0f*(z_ref.x*delta_y + z_ref.y*delta_x)\n"
"                     + 2.0f*delta_x*delta_y + delta_cy;\n"
"\n"
"        // Glitch detection: delta magnitude too large relative to reference\n"
"        float delta_mag = new_dx*new_dx + new_dy*new_dy;\n"
"        float ref_mag = z_ref.x*z_ref.x + z_ref.y*z_ref.y;\n"
"        if (delta_mag > ref_mag * 1e6f && ref_mag > 1e-10f) {\n"
"            iterations[gid.y * params.tile_size + gid.x] = 0xFFFFFFFE;\n"
"            return;\n"
"        }\n"
"\n"
"        delta_x = new_dx;\n"
"        delta_y = new_dy;\n"
"        iteration++;\n"
"    }\n"
"\n"
"    // Mark glitch if pixel needs more iterations than reference\n"
"    if (iteration >= params.ref_escape_iter && iteration < params.max_iter) {\n"
"        iterations[gid.y * params.tile_size + gid.x] = 0xFFFFFFFE;\n"
"        return;\n"
"    }\n"
"\n"
"    iterations[gid.y * params.tile_size + gid.x] = iteration;\n"
"}\n"
"\n"
"struct PerturbParamsV2 {\n"
"    uint tile_size, max_iter, ref_escape_iter;\n"
"};\n"
"\n"
"kernel void mandelbrot_compute_perturb_v2(\n"
"    device uint *iterations [[buffer(0)]],\n"
"    constant PerturbParamsV2 &params [[buffer(1)]],\n"
"    constant float2 *ref_orbit [[buffer(2)]],\n"
"    constant float2 *pixel_deltas [[buffer(3)]],\n"
"    uint2 gid [[thread_position_in_grid]]\n"
") {\n"
"    if (gid.x >= params.tile_size || gid.y >= params.tile_size) return;\n"
"\n"
"    uint idx = gid.y * params.tile_size + gid.x;\n"
"    float2 delta_c = pixel_deltas[idx];  // Pre-computed on CPU!\n"
"\n"
"    float delta_x = 0.0f, delta_y = 0.0f;\n"
"    uint iteration = 0;\n"
"    uint max_safe = min(params.max_iter, params.ref_escape_iter);\n"
"\n"
"    while (iteration < max_safe) {\n"
"        float2 z_ref = ref_orbit[iteration];\n"
"        float zx = z_ref.x + delta_x;\n"
"        float zy = z_ref.y + delta_y;\n"
"\n"
"        if (zx*zx + zy*zy >= 4.0f) break;\n"
"\n"
"        // d_n+1 = 2*Z_ref*d + d^2 + dC\n"
"        float new_dx = 2.0f*(z_ref.x*delta_x - z_ref.y*delta_y)\n"
"                     + delta_x*delta_x - delta_y*delta_y + delta_c.x;\n"
"        float new_dy = 2.0f*(z_ref.x*delta_y + z_ref.y*delta_x)\n"
"                     + 2.0f*delta_x*delta_y + delta_c.y;\n"
"\n"
"        // Glitch detection: delta magnitude too large relative to reference\n"
"        float delta_mag = new_dx*new_dx + new_dy*new_dy;\n"
"        float ref_mag = z_ref.x*z_ref.x + z_ref.y*z_ref.y;\n"
"        if (delta_mag > ref_mag * 1e6f && ref_mag > 1e-10f) {\n"
"            iterations[idx] = 0xFFFFFFFE;\n"
"            return;\n"
"        }\n"
"\n"
"        delta_x = new_dx;\n"
"        delta_y = new_dy;\n"
"        iteration++;\n"
"    }\n"
"\n"
"    // Mark glitch if pixel needs more iterations than reference\n"
"    if (iteration >= params.ref_escape_iter && iteration < params.max_iter) {\n"
"        iterations[idx] = 0xFFFFFFFE;\n"
"        return;\n"
"    }\n"
"\n"
"    iterations[idx] = iteration;\n"
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

// Tile compute state
static MtComputePipelineState *pipelineTile = NULL;
static MtBuffer *tileIterBuffer = NULL;
static MtBuffer *tileParamsBuffer = NULL;
static int tileSize = 0;
static int tileMaxIter = 0;
static bool tilesInitialized = false;

// Perturbation compute state
static MtComputePipelineState *pipelinePerturb = NULL;
static MtBuffer *perturbParamsBuffer = NULL;
static MtBuffer *refOrbitBuffer = NULL;
static MtBuffer *perturbIterBuffer = NULL;
static uint32_t perturbMaxIter = 0;
static bool perturbInitialized = false;

// Perturbation V2 compute state (pre-computed deltas)
static MtComputePipelineState *pipelinePerturbV2 = NULL;
static MtBuffer *perturbParamsV2Buffer = NULL;
static MtBuffer *deltaBuffer = NULL;

// TileParams struct for GPU (must match Metal shader)
typedef struct {
    float center_x;
    float center_y;
    float scale;
    uint32_t tile_x;
    uint32_t tile_y;
    uint32_t tile_size;
    uint32_t max_iter;
    uint32_t vp_half_w;
    uint32_t vp_half_h;
} TileParams;

// PerturbParams struct for GPU (must match Metal shader)
typedef struct {
    float center_x, center_y, scale;
    float ref_cx, ref_cy;
    uint32_t tile_x, tile_y, tile_size, max_iter;
    uint32_t ref_escape_iter, vp_half_w, vp_half_h;
} PerturbParams;

// PerturbParamsV2 struct for GPU (must match Metal shader)
typedef struct {
    uint32_t tile_size;
    uint32_t max_iter;
    uint32_t ref_escape_iter;
} PerturbParamsV2;

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
    // Command buffer is autoreleased by CMT
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
    // Command buffer is autoreleased by CMT
}

void gpu_cleanup(void) {
    // Perturbation V2 buffers
    if (deltaBuffer) {
        mtRelease(deltaBuffer);
        deltaBuffer = NULL;
    }
    if (perturbParamsV2Buffer) {
        mtRelease(perturbParamsV2Buffer);
        perturbParamsV2Buffer = NULL;
    }
    if (pipelinePerturbV2) {
        mtRelease(pipelinePerturbV2);
        pipelinePerturbV2 = NULL;
    }

    // Perturbation buffers
    if (perturbIterBuffer) {
        mtRelease(perturbIterBuffer);
        perturbIterBuffer = NULL;
    }
    if (refOrbitBuffer) {
        mtRelease(refOrbitBuffer);
        refOrbitBuffer = NULL;
    }
    if (perturbParamsBuffer) {
        mtRelease(perturbParamsBuffer);
        perturbParamsBuffer = NULL;
    }
    if (pipelinePerturb) {
        mtRelease(pipelinePerturb);
        pipelinePerturb = NULL;
    }
    perturbInitialized = false;

    // Tile buffers
    if (tileParamsBuffer) {
        mtRelease(tileParamsBuffer);
        tileParamsBuffer = NULL;
    }
    if (tileIterBuffer) {
        mtRelease(tileIterBuffer);
        tileIterBuffer = NULL;
    }
    if (pipelineTile) {
        mtRelease(pipelineTile);
        pipelineTile = NULL;
    }
    tilesInitialized = false;

    // Row buffers
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

// =============================================================================
// Tile-based Compute API
// =============================================================================

int gpu_init_tiles(int tile_sz, int max_iter) {
    // If we already have a device, just create the tile-specific resources
    if (!device) {
        device = mtCreateSystemDefaultDevice();
        if (!device) return -1;

        commandQueue = mtNewCommandQueue(device);
        if (!commandQueue) {
            gpu_cleanup();
            return -1;
        }

        NsError *error = NULL;
        library = mtNewLibraryWithSource(device, (char*)METAL_SHADER_SOURCE, NULL, &error);
        if (!library) {
            fprintf(stderr, "Metal shader compilation failed\n");
            gpu_cleanup();
            return -1;
        }
    }

    // Create tile compute pipeline
    MtFunction *kernelTile = mtNewFunctionWithName(library, "mandelbrot_compute_tile");
    if (!kernelTile) {
        fprintf(stderr, "Could not find mandelbrot_compute_tile function\n");
        return -1;
    }
    pipelineTile = mtNewComputePipelineStateWithFunction(device, kernelTile, NULL);
    mtRelease(kernelTile);

    if (!pipelineTile) {
        fprintf(stderr, "Could not create tile pipeline\n");
        return -1;
    }

    // Allocate tile buffers
    size_t tileIterSize = (size_t)tile_sz * tile_sz * sizeof(uint32_t);
    tileIterBuffer = mtDeviceNewBufferWithLength(device, tileIterSize, MtResourceStorageModeShared);
    tileParamsBuffer = mtDeviceNewBufferWithLength(device, sizeof(TileParams), MtResourceStorageModeShared);

    if (!tileIterBuffer || !tileParamsBuffer) {
        gpu_cleanup();
        return -1;
    }

    tileSize = tile_sz;
    tileMaxIter = max_iter;
    tilesInitialized = true;

    return 0;
}

void gpu_compute_tile(const GPUTileParams *params, PixelColor *output) {
    if (!tilesInitialized || !pipelineTile) {
        return;
    }

    // Set tile parameters
    TileParams *tileParams = mtBufferContents(tileParamsBuffer);
    tileParams->center_x = params->center_x;
    tileParams->center_y = params->center_y;
    tileParams->scale = params->scale;
    tileParams->tile_x = params->tile_x;
    tileParams->tile_y = params->tile_y;
    tileParams->tile_size = params->tile_size;
    tileParams->max_iter = params->max_iter;
    tileParams->vp_half_w = params->vp_half_w;
    tileParams->vp_half_h = params->vp_half_h;

    // Create command buffer and encoder
    // CMT returns autoreleased objects, retain to control lifetime
    MtCommandBuffer *cmdBuffer = mtNewCommandBuffer(commandQueue);
    mtRetain(cmdBuffer);
    MtComputeCommandEncoder *encoder = mtNewComputeCommandEncoder(cmdBuffer);

    mtComputeCommandEncoderSetComputePipelineState(encoder, pipelineTile);
    mtComputeCommandEncoderSetBufferOffsetAtIndex(encoder, tileIterBuffer, 0, 0);
    mtComputeCommandEncoderSetBufferOffsetAtIndex(encoder, tileParamsBuffer, 0, 1);

    // Dispatch threads for tile (2D)
    MtSize gridSize = {(NsUInteger)params->tile_size, (NsUInteger)params->tile_size, 1};
    MtSize threadgroupSize = {16, 16, 1};  // 16x16 = 256 threads per group
    mtComputeCommandEncoderDispatchThread_threadsPerThreadgroup(encoder, gridSize, threadgroupSize);

    mtComputeCommandEncoderEndEncoding(encoder);
    mtCommandBufferCommit(cmdBuffer);
    mtCommandBufferWaitUntilCompleted(cmdBuffer);

    // Apply color mapping on CPU
    uint32_t *iterations = mtBufferContents(tileIterBuffer);
    size_t tilePixels = (size_t)params->tile_size * params->tile_size;
    for (size_t i = 0; i < tilePixels; i++) {
        color_from_iteration(&output[i], iterations[i]);
    }

    // Release our retained reference (balances the retain above)
    mtRelease(cmdBuffer);
}

bool gpu_tiles_initialized(void) {
    return tilesInitialized;
}

// =============================================================================
// Perturbation-based Compute API
// =============================================================================

int gpu_init_perturbation(uint32_t max_iter) {
    // Ensure we have basic GPU resources
    if (!device) {
        device = mtCreateSystemDefaultDevice();
        if (!device) return -1;

        commandQueue = mtNewCommandQueue(device);
        if (!commandQueue) {
            gpu_cleanup();
            return -1;
        }

        NsError *error = NULL;
        library = mtNewLibraryWithSource(device, (char*)METAL_SHADER_SOURCE, NULL, &error);
        if (!library) {
            fprintf(stderr, "Metal shader compilation failed\n");
            gpu_cleanup();
            return -1;
        }
    }

    // Create perturbation compute pipeline
    MtFunction *kernelPerturb = mtNewFunctionWithName(library, "mandelbrot_compute_perturb");
    if (!kernelPerturb) {
        fprintf(stderr, "Could not find mandelbrot_compute_perturb function\n");
        return -1;
    }
    pipelinePerturb = mtNewComputePipelineStateWithFunction(device, kernelPerturb, NULL);
    mtRelease(kernelPerturb);

    if (!pipelinePerturb) {
        fprintf(stderr, "Could not create perturbation pipeline\n");
        return -1;
    }

    // Allocate perturbation buffers
    // Reference orbit: array of float2 (8 bytes per iteration)
    size_t refOrbitSize = max_iter * 2 * sizeof(float);
    refOrbitBuffer = mtDeviceNewBufferWithLength(device, refOrbitSize, MtResourceStorageModeShared);

    // Params buffer
    perturbParamsBuffer = mtDeviceNewBufferWithLength(device, sizeof(PerturbParams), MtResourceStorageModeShared);

    // Iteration buffer (same size as tile buffer - use MB_INTERACTIVE_TILE_SIZE)
    size_t iterSize = MB_INTERACTIVE_TILE_SIZE * MB_INTERACTIVE_TILE_SIZE * sizeof(uint32_t);
    perturbIterBuffer = mtDeviceNewBufferWithLength(device, iterSize, MtResourceStorageModeShared);

    if (!refOrbitBuffer || !perturbParamsBuffer || !perturbIterBuffer) {
        gpu_cleanup();
        return -1;
    }

    // Create V2 perturbation pipeline
    MtFunction *kernelPerturbV2 = mtNewFunctionWithName(library, "mandelbrot_compute_perturb_v2");
    if (!kernelPerturbV2) {
        fprintf(stderr, "Could not find mandelbrot_compute_perturb_v2 function\n");
        gpu_cleanup();
        return -1;
    }
    pipelinePerturbV2 = mtNewComputePipelineStateWithFunction(device, kernelPerturbV2, NULL);
    mtRelease(kernelPerturbV2);

    if (!pipelinePerturbV2) {
        fprintf(stderr, "Could not create perturbation V2 pipeline\n");
        gpu_cleanup();
        return -1;
    }

    // Allocate V2 buffers
    perturbParamsV2Buffer = mtDeviceNewBufferWithLength(device, sizeof(PerturbParamsV2), MtResourceStorageModeShared);

    // Delta buffer: 2 floats per pixel (tile_size^2 * 2 * sizeof(float))
    size_t deltaBufferSize = MB_INTERACTIVE_TILE_SIZE * MB_INTERACTIVE_TILE_SIZE * 2 * sizeof(float);
    deltaBuffer = mtDeviceNewBufferWithLength(device, deltaBufferSize, MtResourceStorageModeShared);

    if (!perturbParamsV2Buffer || !deltaBuffer) {
        gpu_cleanup();
        return -1;
    }

    perturbMaxIter = max_iter;
    perturbInitialized = true;

    return 0;
}

void gpu_update_reference_orbit(const ReferenceOrbit *orbit) {
    if (!perturbInitialized || !refOrbitBuffer || !orbit || !orbit->valid) {
        return;
    }

    // Copy reference orbit to GPU as float2 array
    float *buffer = mtBufferContents(refOrbitBuffer);
    uint32_t count = orbit->escape_iter < perturbMaxIter ? orbit->escape_iter : perturbMaxIter;

    for (uint32_t i = 0; i < count; i++) {
        buffer[i * 2 + 0] = (float)orbit->z_real[i];
        buffer[i * 2 + 1] = (float)orbit->z_imag[i];
    }
}

void gpu_compute_tile_perturb(const GPUPerturbTileParams *params,
                              PixelColor *output, uint32_t *iterations_out) {
    if (!perturbInitialized || !pipelinePerturb) {
        return;
    }

    // Set perturbation parameters
    PerturbParams *perturbParams = mtBufferContents(perturbParamsBuffer);
    perturbParams->center_x = params->center_x;
    perturbParams->center_y = params->center_y;
    perturbParams->scale = params->scale;
    perturbParams->ref_cx = params->ref_cx;
    perturbParams->ref_cy = params->ref_cy;
    perturbParams->tile_x = params->tile_x;
    perturbParams->tile_y = params->tile_y;
    perturbParams->tile_size = params->tile_size;
    perturbParams->max_iter = params->max_iter;
    perturbParams->ref_escape_iter = params->ref_escape_iter;
    perturbParams->vp_half_w = params->vp_half_w;
    perturbParams->vp_half_h = params->vp_half_h;

    // Create command buffer and encoder
    MtCommandBuffer *cmdBuffer = mtNewCommandBuffer(commandQueue);
    mtRetain(cmdBuffer);
    MtComputeCommandEncoder *encoder = mtNewComputeCommandEncoder(cmdBuffer);

    mtComputeCommandEncoderSetComputePipelineState(encoder, pipelinePerturb);
    mtComputeCommandEncoderSetBufferOffsetAtIndex(encoder, perturbIterBuffer, 0, 0);
    mtComputeCommandEncoderSetBufferOffsetAtIndex(encoder, perturbParamsBuffer, 0, 1);
    mtComputeCommandEncoderSetBufferOffsetAtIndex(encoder, refOrbitBuffer, 0, 2);

    // Dispatch threads for tile (2D)
    MtSize gridSize = {(NsUInteger)params->tile_size, (NsUInteger)params->tile_size, 1};
    MtSize threadgroupSize = {16, 16, 1};
    mtComputeCommandEncoderDispatchThread_threadsPerThreadgroup(encoder, gridSize, threadgroupSize);

    mtComputeCommandEncoderEndEncoding(encoder);
    mtCommandBufferCommit(cmdBuffer);
    mtCommandBufferWaitUntilCompleted(cmdBuffer);

    // Get iteration results
    uint32_t *iterations = mtBufferContents(perturbIterBuffer);
    size_t tilePixels = (size_t)params->tile_size * params->tile_size;

    // Copy iterations if requested (for glitch handling)
    if (iterations_out) {
        memcpy(iterations_out, iterations, tilePixels * sizeof(uint32_t));
    }

    // Apply color mapping
    for (size_t i = 0; i < tilePixels; i++) {
        uint32_t iter = iterations[i];
        // Glitch marker pixels will be colored later after CPU fallback
        if (iter == 0xFFFFFFFE) {
            output[i].r = 255;  // Temporary magenta for debugging
            output[i].g = 0;
            output[i].b = 255;
        } else {
            color_from_iteration(&output[i], iter);
        }
    }

    mtRelease(cmdBuffer);
}

bool gpu_perturbation_initialized(void) {
    return perturbInitialized;
}

void gpu_precompute_deltas(double center_x, double center_y, double scale,
                           double ref_cx, double ref_cy,
                           uint32_t tile_size, int vp_half_w, int vp_half_h,
                           uint32_t tile_px, uint32_t tile_py,
                           float *delta_buffer) {
    for (uint32_t ly = 0; ly < tile_size; ly++) {
        double py = (double)(tile_py + ly);
        double delta_cy = center_y + (py - vp_half_h) * scale - ref_cy;

        for (uint32_t lx = 0; lx < tile_size; lx++) {
            double px = (double)(tile_px + lx);
            double delta_cx = center_x + (px - vp_half_w) * scale - ref_cx;

            size_t idx = (ly * tile_size + lx) * 2;
            delta_buffer[idx + 0] = (float)delta_cx;
            delta_buffer[idx + 1] = (float)delta_cy;
        }
    }
}

void gpu_compute_tile_perturb_v2(const GPUPerturbParamsV2 *params,
                                  const float *deltas,
                                  PixelColor *output, uint32_t *iterations_out) {
    if (!perturbInitialized || !pipelinePerturbV2) {
        return;
    }

    // Copy deltas to GPU buffer
    float *gpuDeltas = mtBufferContents(deltaBuffer);
    size_t deltaCount = params->tile_size * params->tile_size * 2;
    memcpy(gpuDeltas, deltas, deltaCount * sizeof(float));

    // Set V2 parameters
    PerturbParamsV2 *perturbParams = mtBufferContents(perturbParamsV2Buffer);
    perturbParams->tile_size = params->tile_size;
    perturbParams->max_iter = params->max_iter;
    perturbParams->ref_escape_iter = params->ref_escape_iter;

    // Create command buffer and encoder
    MtCommandBuffer *cmdBuffer = mtNewCommandBuffer(commandQueue);
    mtRetain(cmdBuffer);
    MtComputeCommandEncoder *encoder = mtNewComputeCommandEncoder(cmdBuffer);

    mtComputeCommandEncoderSetComputePipelineState(encoder, pipelinePerturbV2);
    mtComputeCommandEncoderSetBufferOffsetAtIndex(encoder, perturbIterBuffer, 0, 0);
    mtComputeCommandEncoderSetBufferOffsetAtIndex(encoder, perturbParamsV2Buffer, 0, 1);
    mtComputeCommandEncoderSetBufferOffsetAtIndex(encoder, refOrbitBuffer, 0, 2);
    mtComputeCommandEncoderSetBufferOffsetAtIndex(encoder, deltaBuffer, 0, 3);

    // Dispatch threads for tile (2D)
    MtSize gridSize = {(NsUInteger)params->tile_size, (NsUInteger)params->tile_size, 1};
    MtSize threadgroupSize = {16, 16, 1};
    mtComputeCommandEncoderDispatchThread_threadsPerThreadgroup(encoder, gridSize, threadgroupSize);

    mtComputeCommandEncoderEndEncoding(encoder);
    mtCommandBufferCommit(cmdBuffer);
    mtCommandBufferWaitUntilCompleted(cmdBuffer);

    // Get iteration results
    uint32_t *iterations = mtBufferContents(perturbIterBuffer);
    size_t tilePixels = (size_t)params->tile_size * params->tile_size;

    // Copy iterations if requested (for glitch handling)
    if (iterations_out) {
        memcpy(iterations_out, iterations, tilePixels * sizeof(uint32_t));
    }

    // Apply color mapping
    for (size_t i = 0; i < tilePixels; i++) {
        uint32_t iter = iterations[i];
        // Glitch marker pixels will be colored later after CPU fallback
        if (iter == 0xFFFFFFFE) {
            output[i].r = 255;  // Temporary magenta for debugging
            output[i].g = 0;
            output[i].b = 255;
        } else {
            color_from_iteration(&output[i], iter);
        }
    }

    mtRelease(cmdBuffer);
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

int gpu_init_tiles(int tile_size, int max_iter) {
    (void)tile_size;
    (void)max_iter;
    return -1;
}

void gpu_compute_tile(const GPUTileParams *params, PixelColor *output) {
    (void)params;
    (void)output;
}

bool gpu_tiles_initialized(void) {
    return false;
}

int gpu_init_perturbation(uint32_t max_iter) {
    (void)max_iter;
    return -1;
}

void gpu_update_reference_orbit(const ReferenceOrbit *orbit) {
    (void)orbit;
}

void gpu_compute_tile_perturb(const GPUPerturbTileParams *params,
                              PixelColor *output, uint32_t *iterations) {
    (void)params;
    (void)output;
    (void)iterations;
}

bool gpu_perturbation_initialized(void) {
    return false;
}

void gpu_precompute_deltas(double center_x, double center_y, double scale,
                           double ref_cx, double ref_cy,
                           uint32_t tile_size, int vp_half_w, int vp_half_h,
                           uint32_t tile_px, uint32_t tile_py,
                           float *delta_buffer) {
    (void)center_x; (void)center_y; (void)scale;
    (void)ref_cx; (void)ref_cy;
    (void)tile_size; (void)vp_half_w; (void)vp_half_h;
    (void)tile_px; (void)tile_py;
    (void)delta_buffer;
}

void gpu_compute_tile_perturb_v2(const GPUPerturbParamsV2 *params,
                                  const float *deltas,
                                  PixelColor *output, uint32_t *iterations) {
    (void)params;
    (void)deltas;
    (void)output;
    (void)iterations;
}

#endif // MB_GPU_METAL
