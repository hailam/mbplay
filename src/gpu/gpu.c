#include "gpu.h"
#include "../color/color.h"
#include "../precision/mp_real.h"
#include "../perturbation/perturb_cpu.h"

#include <stdlib.h>
#include <string.h>

// =============================================================================
// Perturbation V2 — CPU implementation (platform independent)
// =============================================================================
//
// Metal has no double type, so the former double2 V2 kernels could never
// compile — worse, their presence made the whole shader library fail to
// build at runtime, disabling every (valid) float kernel as well. The V2
// entry points below keep their public names but run on the CPU via
// perturb_cpu.c, which also removes the need for glitch markers (rebasing).

static double *cpuRefRe = NULL;      // CPU copy of the reference orbit
static double *cpuRefIm = NULL;
static uint32_t cpuRefLen = 0;       // usable entries in the copy
static uint32_t perturbMaxIter = 0;  // capacity of the copy
static bool perturbInitialized = false;

static int perturb_cpu_state_init(uint32_t max_iter) {
    free(cpuRefRe);
    free(cpuRefIm);
    cpuRefRe = (double *)malloc((size_t)max_iter * sizeof(double));
    cpuRefIm = (double *)malloc((size_t)max_iter * sizeof(double));
    cpuRefLen = 0;
    if (!cpuRefRe || !cpuRefIm) {
        free(cpuRefRe);
        free(cpuRefIm);
        cpuRefRe = NULL;
        cpuRefIm = NULL;
        return -1;
    }
    return 0;
}

static void perturb_cpu_state_free(void) {
    free(cpuRefRe);
    free(cpuRefIm);
    cpuRefRe = NULL;
    cpuRefIm = NULL;
    cpuRefLen = 0;
    perturbInitialized = false;
}

void gpu_update_reference_orbit(const ReferenceOrbit *orbit) {
    if (!perturbInitialized || !cpuRefRe || !orbit || !orbit->valid) {
        return;
    }

    // Stored entries run from 0 to escape_iter inclusive when the reference
    // escaped early, and from 0 to max_iter-1 when it never escaped.
    uint32_t count = orbit->escape_iter + 1;
    if (count > perturbMaxIter) count = perturbMaxIter;
    if (count > orbit->max_iter) count = orbit->max_iter;
    if (count == 0) return;

    memcpy(cpuRefRe, orbit->z_real, count * sizeof(double));
    memcpy(cpuRefIm, orbit->z_imag, count * sizeof(double));
    cpuRefLen = count;
}

void gpu_compute_tile_perturb_v2(const GPUPerturbParamsV2 *params,
                                  const double *deltas,
                                  PixelColor *output, uint32_t *iterations_out) {
    if (!perturbInitialized || cpuRefLen == 0 || !params || !deltas || !output) {
        return;
    }

    size_t tilePixels = (size_t)params->tile_size * params->tile_size;
    uint32_t *iters = iterations_out;
    uint32_t *local = NULL;
    if (!iters) {
        local = (uint32_t *)malloc(tilePixels * sizeof(uint32_t));
        if (!local) return;
        iters = local;
    }

    perturb_cpu_tile(cpuRefRe, cpuRefIm, cpuRefLen, deltas,
                     params->tile_size, params->max_iter, iters, NULL);

    for (size_t i = 0; i < tilePixels; i++) {
        color_from_iteration_classic(&output[i], iters[i], params->max_iter);
    }

    free(local);
}

void gpu_compute_tile_perturb_v2_smooth(const GPUPerturbParamsV2 *params,
                                        const double *deltas,
                                        PixelColor *output,
                                        uint32_t *iterations_out,
                                        const MBRenderSettings *settings) {
    if (!perturbInitialized || cpuRefLen == 0 || !params || !deltas || !output) {
        return;
    }

    size_t tilePixels = (size_t)params->tile_size * params->tile_size;
    uint32_t *iters = iterations_out;
    uint32_t *local = NULL;
    if (!iters) {
        local = (uint32_t *)malloc(tilePixels * sizeof(uint32_t));
        if (!local) return;
        iters = local;
    }
    float *z2 = (float *)malloc(tilePixels * sizeof(float));
    if (!z2) {
        free(local);
        return;
    }

    perturb_cpu_tile(cpuRefRe, cpuRefIm, cpuRefLen, deltas,
                     params->tile_size, params->max_iter, iters, z2);

    for (size_t i = 0; i < tilePixels; i++) {
        color_from_iteration_ex(&output[i], iters[i], z2[i],
                                params->max_iter, settings);
    }

    free(z2);
    free(local);
}

bool gpu_perturbation_initialized(void) {
    return perturbInitialized;
}

#ifdef MB_GPU_METAL

#include <cmt/cmt.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// =============================================================================
// Metal Shader Source (generated from mandelbrot.metal)
// =============================================================================

#include "mandelbrot_shader.h"

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

// Perturbation compute state (V1 float kernel, deprecated but kept working)
static MtComputePipelineState *pipelinePerturb = NULL;
static MtBuffer *perturbParamsBuffer = NULL;
static MtBuffer *refOrbitBuffer = NULL;
static MtBuffer *perturbIterBuffer = NULL;

// Smooth coloring state
static MtComputePipelineState *pipelineTileSmooth = NULL;
static MtBuffer *finalZ2Buffer = NULL;
static bool smoothInitialized = false;

// Supersampling state
static MtComputePipelineState *pipelineTileSS4 = NULL;
static MtBuffer *smoothIterBuffer = NULL;
static bool supersampleInitialized = false;

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
        color_from_iteration_classic(&output[x], iterations[x],
                                     (unsigned int)gpuConfig.max_iter);
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
        color_from_iteration_classic(&output[i], iterations[i],
                                     (unsigned int)gpuConfig.max_iter);
    }
    // Command buffer is autoreleased by CMT
}

void gpu_cleanup(void) {
    // Supersampling buffers
    if (smoothIterBuffer) {
        mtRelease(smoothIterBuffer);
        smoothIterBuffer = NULL;
    }
    if (pipelineTileSS4) {
        mtRelease(pipelineTileSS4);
        pipelineTileSS4 = NULL;
    }
    supersampleInitialized = false;

    // Smooth coloring buffers
    if (finalZ2Buffer) {
        mtRelease(finalZ2Buffer);
        finalZ2Buffer = NULL;
    }
    if (pipelineTileSmooth) {
        mtRelease(pipelineTileSmooth);
        pipelineTileSmooth = NULL;
    }
    smoothInitialized = false;

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
    perturb_cpu_state_free();

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
        color_from_iteration_classic(&output[i], iterations[i], params->max_iter);
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
    // CPU-side perturbation state; works with or without a GPU.
    // (The V1 float GPU kernel is no longer wired up: its reference-orbit
    // buffer was typed float2 on the GPU but filled with doubles from the
    // CPU, and the V2 double kernels could never compile. Perturbation now
    // runs on the CPU — see the shared section at the top of this file.)
    if (perturb_cpu_state_init(max_iter) != 0) {
        return -1;
    }

    // Optional GPU float pipelines for smooth/AA tile rendering. Failures
    // here do not disable perturbation itself.
    if (device && library) {
        MtFunction *kernelTileSmooth = mtNewFunctionWithName(library, "mandelbrot_compute_tile_smooth");
        if (kernelTileSmooth) {
            pipelineTileSmooth = mtNewComputePipelineStateWithFunction(device, kernelTileSmooth, NULL);
            mtRelease(kernelTileSmooth);
        }

        // Allocate final_z2 buffer (one float per pixel)
        size_t z2BufferSize = MB_INTERACTIVE_TILE_SIZE * MB_INTERACTIVE_TILE_SIZE * sizeof(float);
        finalZ2Buffer = mtDeviceNewBufferWithLength(device, z2BufferSize, MtResourceStorageModeShared);

        smoothInitialized = (pipelineTileSmooth != NULL && finalZ2Buffer != NULL);

        // Initialize supersampling pipeline
        MtFunction *kernelTileSS4 = mtNewFunctionWithName(library, "mandelbrot_compute_tile_ss4");
        if (kernelTileSS4) {
            pipelineTileSS4 = mtNewComputePipelineStateWithFunction(device, kernelTileSS4, NULL);
            mtRelease(kernelTileSS4);
        }

        // Allocate smooth iteration buffer (one float per pixel for supersampled output)
        size_t smoothIterSize = MB_INTERACTIVE_TILE_SIZE * MB_INTERACTIVE_TILE_SIZE * sizeof(float);
        smoothIterBuffer = mtDeviceNewBufferWithLength(device, smoothIterSize, MtResourceStorageModeShared);

        supersampleInitialized = (pipelineTileSS4 != NULL && smoothIterBuffer != NULL);
    }

    perturbMaxIter = max_iter;
    perturbInitialized = true;

    return 0;
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
#ifdef MB_DEBUG
            // Debug: magenta to make glitches visible
            output[i].r = 255;
            output[i].g = 0;
            output[i].b = 255;
#else
            // Production: black placeholder (will be replaced by CPU fallback)
            output[i].r = 0;
            output[i].g = 0;
            output[i].b = 0;
#endif
        } else {
            color_from_iteration_classic(&output[i], iter, params->max_iter);
        }
    }

    mtRelease(cmdBuffer);
}

void gpu_precompute_deltas(double center_x, double center_y, double scale,
                           double ref_cx, double ref_cy,
                           uint32_t tile_size, int vp_half_w, int vp_half_h,
                           uint32_t tile_px, uint32_t tile_py,
                           double *delta_buffer) {
    for (uint32_t ly = 0; ly < tile_size; ly++) {
        double py = (double)(tile_py + ly);
        double delta_cy = center_y + (py - vp_half_h) * scale - ref_cy;

        for (uint32_t lx = 0; lx < tile_size; lx++) {
            double px = (double)(tile_px + lx);
            double delta_cx = center_x + (px - vp_half_w) * scale - ref_cx;

            size_t idx = (ly * tile_size + lx) * 2;
            delta_buffer[idx + 0] = delta_cx;
            delta_buffer[idx + 1] = delta_cy;
        }
    }
}

void gpu_precompute_deltas_hp(
    const char *center_x_str, const char *center_y_str,
    const char *ref_cx_str, const char *ref_cy_str,
    uint32_t precision,
    double scale,
    uint32_t tile_size, int vp_half_w, int vp_half_h,
    uint32_t tile_px, uint32_t tile_py,
    double *delta_buffer)
{
    mpfr_prec_t prec = (mpfr_prec_t)precision;

    // Initialize HP values for center and reference
    MPReal center_x, center_y, ref_cx, ref_cy;
    mp_real_init_set_str(&center_x, center_x_str, prec);
    mp_real_init_set_str(&center_y, center_y_str, prec);
    mp_real_init_set_str(&ref_cx, ref_cx_str, prec);
    mp_real_init_set_str(&ref_cy, ref_cy_str, prec);

    // Temp variables for pixel coordinate computation
    MPReal pixel_cx, pixel_cy, delta_cx, delta_cy;
    MPReal offset, scaled_offset;
    mp_real_init(&pixel_cx, prec);
    mp_real_init(&pixel_cy, prec);
    mp_real_init(&delta_cx, prec);
    mp_real_init(&delta_cy, prec);
    mp_real_init(&offset, prec);
    mp_real_init(&scaled_offset, prec);

    for (uint32_t ly = 0; ly < tile_size; ly++) {
        // Compute pixel Y offset and scaled offset
        // offset = (tile_py + ly) - vp_half_h
        mp_real_set_d(&offset, (double)(tile_py + ly) - vp_half_h);

        // scaled_offset = offset * scale
        mp_real_mul_d(&scaled_offset, &offset, scale);

        // pixel_cy = center_y + scaled_offset
        mp_real_add(&pixel_cy, &center_y, &scaled_offset);

        // delta_cy = pixel_cy - ref_cy
        mp_real_sub(&delta_cy, &pixel_cy, &ref_cy);

        double delta_cy_d = mp_real_get_d(&delta_cy);

        for (uint32_t lx = 0; lx < tile_size; lx++) {
            // Compute pixel X offset and scaled offset
            // offset = (tile_px + lx) - vp_half_w
            mp_real_set_d(&offset, (double)(tile_px + lx) - vp_half_w);

            // scaled_offset = offset * scale
            mp_real_mul_d(&scaled_offset, &offset, scale);

            // pixel_cx = center_x + scaled_offset
            mp_real_add(&pixel_cx, &center_x, &scaled_offset);

            // delta_cx = pixel_cx - ref_cx
            mp_real_sub(&delta_cx, &pixel_cx, &ref_cx);

            double delta_cx_d = mp_real_get_d(&delta_cx);

            size_t idx = (ly * tile_size + lx) * 2;
            delta_buffer[idx + 0] = delta_cx_d;
            delta_buffer[idx + 1] = delta_cy_d;
        }
    }

    // Cleanup
    mp_real_clear(&center_x);
    mp_real_clear(&center_y);
    mp_real_clear(&ref_cx);
    mp_real_clear(&ref_cy);
    mp_real_clear(&pixel_cx);
    mp_real_clear(&pixel_cy);
    mp_real_clear(&delta_cx);
    mp_real_clear(&delta_cy);
    mp_real_clear(&offset);
    mp_real_clear(&scaled_offset);
}

// =============================================================================
// Smooth Coloring API Implementation
// =============================================================================

void gpu_compute_tile_smooth(const GPUTileParams *params,
                             PixelColor *output,
                             const MBRenderSettings *settings) {
    if (!smoothInitialized || !pipelineTileSmooth) {
        // Fallback to non-smooth
        gpu_compute_tile(params, output);
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
    MtCommandBuffer *cmdBuffer = mtNewCommandBuffer(commandQueue);
    mtRetain(cmdBuffer);
    MtComputeCommandEncoder *encoder = mtNewComputeCommandEncoder(cmdBuffer);

    mtComputeCommandEncoderSetComputePipelineState(encoder, pipelineTileSmooth);
    mtComputeCommandEncoderSetBufferOffsetAtIndex(encoder, tileIterBuffer, 0, 0);
    mtComputeCommandEncoderSetBufferOffsetAtIndex(encoder, tileParamsBuffer, 0, 1);
    mtComputeCommandEncoderSetBufferOffsetAtIndex(encoder, finalZ2Buffer, 0, 2);

    // Dispatch threads for tile (2D)
    MtSize gridSize = {(NsUInteger)params->tile_size, (NsUInteger)params->tile_size, 1};
    MtSize threadgroupSize = {16, 16, 1};
    mtComputeCommandEncoderDispatchThread_threadsPerThreadgroup(encoder, gridSize, threadgroupSize);

    mtComputeCommandEncoderEndEncoding(encoder);
    mtCommandBufferCommit(cmdBuffer);
    mtCommandBufferWaitUntilCompleted(cmdBuffer);

    // Apply color mapping with smooth coloring
    uint32_t *iterations = mtBufferContents(tileIterBuffer);
    float *z2Values = mtBufferContents(finalZ2Buffer);
    size_t tilePixels = (size_t)params->tile_size * params->tile_size;

    for (size_t i = 0; i < tilePixels; i++) {
        color_from_iteration_ex(&output[i], iterations[i], z2Values[i],
                                params->max_iter, settings);
    }

    mtRelease(cmdBuffer);
}

void gpu_compute_tile_supersampled(const GPUTileParams *params,
                                   PixelColor *output,
                                   const MBRenderSettings *settings) {
    if (!supersampleInitialized || !pipelineTileSS4) {
        // Fallback to non-supersampled smooth
        gpu_compute_tile_smooth(params, output, settings);
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
    MtCommandBuffer *cmdBuffer = mtNewCommandBuffer(commandQueue);
    mtRetain(cmdBuffer);
    MtComputeCommandEncoder *encoder = mtNewComputeCommandEncoder(cmdBuffer);

    mtComputeCommandEncoderSetComputePipelineState(encoder, pipelineTileSS4);
    mtComputeCommandEncoderSetBufferOffsetAtIndex(encoder, smoothIterBuffer, 0, 0);
    mtComputeCommandEncoderSetBufferOffsetAtIndex(encoder, tileParamsBuffer, 0, 1);

    // Dispatch threads for tile (2D)
    MtSize gridSize = {(NsUInteger)params->tile_size, (NsUInteger)params->tile_size, 1};
    MtSize threadgroupSize = {16, 16, 1};
    mtComputeCommandEncoderDispatchThread_threadsPerThreadgroup(encoder, gridSize, threadgroupSize);

    mtComputeCommandEncoderEndEncoding(encoder);
    mtCommandBufferCommit(cmdBuffer);
    mtCommandBufferWaitUntilCompleted(cmdBuffer);

    // Apply color mapping from smooth iteration values
    float *smoothIter = mtBufferContents(smoothIterBuffer);
    size_t tilePixels = (size_t)params->tile_size * params->tile_size;

    // Use default cycle scale if not set (for backward compatibility)
    float cycle_scale = settings->color_cycle_scale > 0.0f ? settings->color_cycle_scale : 64.0f;

    for (size_t i = 0; i < tilePixels; i++) {
        color_from_smooth_iteration(&output[i], smoothIter[i],
                                    params->max_iter, settings->palette_id, cycle_scale);
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
    perturb_cpu_state_free();
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
    // CPU perturbation needs no GPU.
    if (perturb_cpu_state_init(max_iter) != 0) {
        return -1;
    }
    perturbMaxIter = max_iter;
    perturbInitialized = true;
    return 0;
}

void gpu_compute_tile_perturb(const GPUPerturbTileParams *params,
                              PixelColor *output, uint32_t *iterations) {
    (void)params;
    (void)output;
    (void)iterations;
}

void gpu_precompute_deltas(double center_x, double center_y, double scale,
                           double ref_cx, double ref_cy,
                           uint32_t tile_size, int vp_half_w, int vp_half_h,
                           uint32_t tile_px, uint32_t tile_py,
                           double *delta_buffer) {
    for (uint32_t ly = 0; ly < tile_size; ly++) {
        double py = (double)(tile_py + ly);
        double delta_cy = center_y + (py - vp_half_h) * scale - ref_cy;

        for (uint32_t lx = 0; lx < tile_size; lx++) {
            double px = (double)(tile_px + lx);
            double delta_cx = center_x + (px - vp_half_w) * scale - ref_cx;

            size_t idx = (ly * tile_size + lx) * 2;
            delta_buffer[idx + 0] = delta_cx;
            delta_buffer[idx + 1] = delta_cy;
        }
    }
}

void gpu_precompute_deltas_hp(
    const char *center_x_str, const char *center_y_str,
    const char *ref_cx_str, const char *ref_cy_str,
    uint32_t precision,
    double scale,
    uint32_t tile_size, int vp_half_w, int vp_half_h,
    uint32_t tile_px, uint32_t tile_py,
    double *delta_buffer) {
    (void)center_x_str; (void)center_y_str;
    (void)ref_cx_str; (void)ref_cy_str;
    (void)precision; (void)scale;
    (void)tile_size; (void)vp_half_w; (void)vp_half_h;
    (void)tile_px; (void)tile_py;
    (void)delta_buffer;
}

void gpu_compute_tile_smooth(const GPUTileParams *params,
                             PixelColor *output,
                             const MBRenderSettings *settings) {
    (void)params;
    (void)output;
    (void)settings;
}

void gpu_compute_tile_supersampled(const GPUTileParams *params,
                                   PixelColor *output,
                                   const MBRenderSettings *settings) {
    (void)params;
    (void)output;
    (void)settings;
}

#endif // MB_GPU_METAL
