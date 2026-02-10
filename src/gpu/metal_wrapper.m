// Metal wrapper - Minimal C bindings for Metal compute
// This file provides the missing CMT implementations

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

// Buffer functions
void* mtBufferContents(void *buf) {
    return [(__bridge id<MTLBuffer>)buf contents];
}

// Library functions
void* mtNewLibraryWithSource(void *device, char *source, void *opts, void **error) {
    NSError *nsError = nil;
    id<MTLLibrary> lib = [(__bridge id<MTLDevice>)device
                          newLibraryWithSource:[NSString stringWithUTF8String:source]
                          options:(__bridge MTLCompileOptions*)opts
                          error:&nsError];
    if (error && nsError) {
        *error = (__bridge_retained void*)nsError;
    }
    return (__bridge_retained void*)lib;
}

// Function functions
void* mtNewFunctionWithName(void *lib, const char *name) {
    return (__bridge_retained void*)[(__bridge id<MTLLibrary>)lib
                                     newFunctionWithName:[NSString stringWithUTF8String:name]];
}

// Compute Pipeline functions
void* mtNewComputePipelineStateWithFunction(void *device, void *func, void *error) {
    NSError *nsError = nil;
    id<MTLComputePipelineState> pipeline = [(__bridge id<MTLDevice>)device
                                            newComputePipelineStateWithFunction:(__bridge id<MTLFunction>)func
                                            error:&nsError];
    // Note: error parameter is ignored for simplicity
    return (__bridge_retained void*)pipeline;
}
