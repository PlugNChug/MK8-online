// Diagnostic interface of the MK8-online MLFI provider (not part of the FFX API).
//
// The five FFX entry points are the ABI; this header adds one optional export so the headless test
// can assert what the provider actually recorded (pipeline count, dispatch counts, last return
// codes) without reading the command buffer back through the validation layer.
//
// Everything here is measured by the provider itself: counts are incremented at the vkCmdDispatch
// call site, not derived from the captured tables.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MK8_MLFI_STATS_VERSION 1u

typedef struct Mk8MlfiStats
{
    uint32_t struct_size;  ///< Must be set to sizeof(Mk8MlfiStats) by the caller.
    uint32_t version;      ///< Output: MK8_MLFI_STATS_VERSION.

    uint32_t graph_pipelines;  ///< Model + preprocess pipelines created (195).
    uint32_t copy_pipelines;   ///< Input-copy pipelines (3: colour, motion vectors, depth).
    uint32_t compose_pipelines;///< Output composition pipelines (1).

    uint32_t prepare_dispatches;  ///< Graph dispatches recorded by the last PrepareV2 (5).
    uint32_t copy_dispatches;     ///< Input-copy dispatches recorded by the last PrepareV2 (2).
    uint32_t model_dispatches;    ///< Dispatches recorded by the last FrameGeneration (191).
    uint32_t compose_dispatches;  ///< Composition passes recorded by the last FrameGeneration (1).

    uint64_t frames;   ///< FrameGeneration calls that recorded the graph.
    uint32_t resets;   ///< Reset sequences recorded by PrepareV2.
    uint32_t first_frame;

    uint32_t last_create_rc;   ///< FFX return code of the last context creation.
    uint32_t last_prepare_rc;  ///< FFX return code of the last PrepareV2.
    uint32_t last_frame_rc;    ///< FFX return code of the last FrameGeneration.

    uint32_t display_width;
    uint32_t display_height;
    uint32_t render_width;
    uint32_t render_height;
    uint32_t backbuffer_format;  ///< FFX_API_SURFACE_FORMAT_* the context was created with.
} Mk8MlfiStats;

/// Fills `stats` for `context`.  Returns 0 (FFX_API_RETURN_OK) on success, 6
/// (FFX_API_RETURN_ERROR_PARAMETER) for a null/foreign context or a bad struct size.
__attribute__((visibility("default"))) uint32_t mk8MlfiGetStats(void* context, Mk8MlfiStats* stats);

#ifdef __cplusplus
}
#endif
