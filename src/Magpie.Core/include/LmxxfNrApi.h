// Origin: lmxxf's runtime, Copyright (c) 2026 Kien (MIT; see LICENSE.lmxxf).
// Modifications Copyright (c) 2026 3zwr1 (AMDNR)
#pragma once

/* Versioned C ABI for LmxxfNrRuntime.dll.
 * MSVC host and MinGW runtime must not share a C++ ABI. No STL, exceptions, or
 * CRT-allocated objects cross this boundary. x64 stdcall is the Windows default. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LMXXF_NR_ABI_VERSION 1u

enum LmxxfNrStatus
{
    LMXXF_NR_OK = 0,
    LMXXF_NR_UNSUPPORTED_ABI = 1,
    LMXXF_NR_INVALID_ARGUMENT = 2,
    LMXXF_NR_NOT_IMPLEMENTED = 3,
    LMXXF_NR_UNAVAILABLE = 4,
    LMXXF_NR_FAILED = 5
};

enum LmxxfNrJobState
{
    LMXXF_NR_JOB_NONE = 0,
    LMXXF_NR_JOB_PREPARED = 1,           /* Set by PrepareFrame; ready for RecordInputs */
    LMXXF_NR_JOB_PRODUCER_SUBMITTED = 2, /* Set by RecordInputs; producer recorded/submitted */
    LMXXF_NR_JOB_NR_ENQUEUED = 3,        /* EnqueueHip scheduled on queue */
    LMXXF_NR_JOB_NR_COMPLETE = 4,        /* EnqueueHip executed or completed */
    LMXXF_NR_JOB_CONSUMER_COMPLETE = 5,  /* Set by RecordOutputs; consumer recorded */
    LMXXF_NR_JOB_RETIRED = 6             /* Set by Retire or CancelUnsubmitted */
};

typedef struct LmxxfNrCapabilities
{
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t max_input_width;
    uint32_t max_input_height;
    uint32_t history_supported; /* first product version: 0 */
    uint32_t overlap_supported; /* first product version: 0 */
    uint32_t graph_supported;   /* first product version: 0; EnqueueHip must not graph-wait */
    uint32_t hip_ready;         /* 1 once host+hsaco are loaded */
    uint32_t gfx1201_target;    /* 1 = this binary is for gfx1201 */
} LmxxfNrCapabilities;

typedef struct LmxxfNrCreateInfo
{
    uint32_t struct_size;
    void *device; /* ID3D12Device*; not dereferenced until HIP is wired */
    void *queue;  /* ID3D12CommandQueue*; must match device when HIP is wired */
    const wchar_t *assets_directory;
    uint32_t flags; /* must be 0 in ABI v1 */
} LmxxfNrCreateInfo;

#define LMXXF_NR_FRAME_FLAG_STRENGTH          (1u << 0)
#define LMXXF_NR_FRAME_FLAG_DEBUG_VIEW        (1u << 1)
#define LMXXF_NR_FRAME_FLAG_CODEC_PASSTHROUGH (1u << 2)
/* Fork addition (DLSS-NR for AMD): feed the network its temporal history - the previous
 * answer, reprojected by this frame's motion vectors (upstream's temporal path: feed ->
 * coordinates -> sample). Needs the motion fields below; history_reset drops the history
 * for one frame (camera cut, resize, a frame the model skipped). */
#define LMXXF_NR_FRAME_FLAG_TEMPORAL          (1u << 3)
/* Fork addition: the host is OptiScaler's Vulkan-on-D3D12 bridge. Its queue already holds a
 * Wait on a fence that only the game's next vkQueueSubmit signals, so the runtime warms the
 * network (weights uploaded, one run) before any external wait exists; and all queue work
 * submitted before PrepareFrame is already complete, so no queue drain (Signal + CPU wait) is
 * issued. Sticky for the session once seen. An older runtime rejects it ("unknown flags").
 * ABI note: a SUCCESSFUL PrepareFrame carrying this flag may leave an informational line in
 * GetLastError (the HIP warm-up, a skipped drain, the Neural passes kernel made before the
 * launch); read it right after PrepareFrame, since the next call clears it. Without the flag a
 * successful PrepareFrame still leaves GetLastError empty. Drain on such a session synchronizes
 * HIP only, and only once the last job's input is signalled (never a queue Signal). */
#define LMXXF_NR_FRAME_FLAG_VULKAN_BRIDGE     (1u << 4)
/* Fork addition (AMDNR "Full network"): run all 71 blocks of the network. Without it the runtime
 * skips residual blocks 42, 43 and 46 (lmxxf's production schedule: about 1 ms faster at 1080p,
 * about 41 dB from the full network); with it nothing is skipped. The block schedule is fixed when
 * the session's HIP network is built, so a PrepareFrame whose value differs from the one the
 * current network was built with rebuilds it (network and codec chain, the teardown a colour-size
 * change goes through; the history restarts). The host must treat that PrepareFrame like a size
 * change: the previous job is consumed on an earlier list or abandoned (AbandonJob), never recorded
 * into the list this PrepareFrame's job goes into, because the rebuild frees what that consumer
 * reads. Set it only when wanted: an older runtime rejects it ("unknown flags"). GetStatus reports
 * the schedule in use as skip=42,43,46 or skip=none. */
#define LMXXF_NR_FRAME_FLAG_FULL_NETWORK      (1u << 5)
/* Fork addition (AMDNR 0.3.4, native character mask): the pre-block controls below
 * (control_tone .. control_style). lmxxf's first layer takes five constant features that the shipped
 * contract fixes at 1 (style at 1/128): tone, structure, skin, other and style; skin and other carry
 * the model's own character/scene split (NVIDIA's auto mask). The runtime scales each feature's
 * column of the 32x16 input mix by its control when it loads the weights, so 1 is the shipped
 * picture; typical values: mask on = (1, 1, skin, local structure), mask off = (1, local structure,
 * -1, -1). Every value must be finite and in [-1, 4]. The flag needs struct_size ==
 * sizeof(LmxxfNrFrameInfo) (144); without the flag the controls are the built-in 1s and the fields
 * are not read. Send the flag only when a value is not 1: an older runtime refuses the size ("struct_size
 * mismatch") or the flag ("unknown flags"). Like LMXXF_NR_FRAME_FLAG_FULL_NETWORK, a PrepareFrame whose
 * controls differ from the ones the current network was built with rebuilds it (the host consumes or
 * abandons the previous job first; the history restarts). GetStatus adds ctl=tone,structure,skin,other
 * (and ,style when not 1) before skip= while the controls are not the built-in ones. */
#define LMXXF_NR_FRAME_FLAG_CONTROLS          (1u << 6)

typedef struct LmxxfNrFrameInfo
{
    uint32_t struct_size;
    uint64_t session_id;
    uint64_t frame_id;
    uint64_t list_generation;
    void *command_list; /* ID3D12GraphicsCommandList*; Record* do not Execute */
    uint32_t color_width;
    uint32_t color_height;
    void *color; /* ID3D12Resource*; required for RecordInputs */
    uint32_t color_state; /* D3D12_RESOURCE_STATES at RecordInputs */
    uint32_t flags; /* LMXXF_NR_FRAME_FLAG_* (0 in legacy ABI v1) */
    float transfer_strength; /* Detail strength: 0..1, default 1.0 */
    float color_strength;    /* Colour strength: 0..1, default 1.0 */
    uint32_t debug_view;     /* 0=normal, 1=proxy, 2=neural solo, 3=diff 20x, 4=tint */
    float model_scale;       /* 0.25..1.0, default 1.0 */
    /* Fork addition: temporal history inputs (LMXXF_NR_FRAME_FLAG_TEMPORAL). `motion` is an
     * ID3D12Resource* 2D texture with two float channels, motion_width x motion_height = the
     * colour's size, whose value times motion_scale is the displacement in colour pixels from
     * this frame's pixel to where it was in the previous frame (the DLSS / FSR convention). */
    void *motion;
    uint32_t motion_state;   /* D3D12_RESOURCE_STATES of `motion` at RecordInputs */
    uint32_t motion_width;
    uint32_t motion_height;
    float motion_scale_x;
    float motion_scale_y;
    uint32_t history_reset;  /* 1: the previous answer must not be used as history this frame */
    uint32_t passes;         /* fork: network launches per frame, 1..3 (0 = 1); each extra pass re-runs
                                the network on its own answer, on the HIP stream, at full model cost */
    /* Fork addition: upstream's output-side temporal smoothing (native_output_smooth.hlsl), in
     * place on the network's answer before it becomes history and before decode: where the
     * answer differs from the motion-warped previous answer by less than the threshold it is
     * blended toward it by up to `output_smooth` (0 = off, at most 1; upstream advises <= 0.8).
     * Needs the temporal path (LMXXF_NR_FRAME_FLAG_TEMPORAL, history in use this frame).
     * `output_smooth_threshold` is in working-surface units (0 = 6/255). */
    float output_smooth;
    float output_smooth_threshold;
    /* Fork addition (AMDNR 0.3.4): the pre-block controls, read only with LMXXF_NR_FRAME_FLAG_CONTROLS.
     * Offsets 124/128/132/136/140; the struct grows from 128 to 144 bytes (sizes 64, 88, 120 and 128
     * stay accepted; control_tone sits where a 128-byte struct has its tail padding, which is why the
     * flag needs the full size). control_style scales the style feature (shipped 1/128); 1 keeps it. */
    float control_tone;
    float control_structure;
    float control_skin;
    float control_other;
    float control_style;
} LmxxfNrFrameInfo;

typedef struct LmxxfNrJob
{
    uint32_t struct_size;
    void *handle;
    void *private_output; /* ID3D12Resource* for SR; null until PrepareFrame succeeds */
} LmxxfNrJob;

typedef struct LmxxfNrApi
{
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t (*QueryCapabilities)(LmxxfNrCapabilities *out);
    int32_t (*Create)(const LmxxfNrCreateInfo *info, void **context);
    int32_t (*Destroy)(void *context);
    int32_t (*PrepareSession)(void *context);
    int32_t (*PrepareFrame)(void *context, const LmxxfNrFrameInfo *info, LmxxfNrJob *job);
    int32_t (*RecordInputs)(void *context, void *job, void *command_list);
    int32_t (*EnqueueHip)(void *context, void *job, void *command_queue);
    int32_t (*RecordOutputs)(void *context, void *job, void *command_list);
    int32_t (*ExecuteAfterProducer)(void *context, void *job, void *command_queue);
    int32_t (*CancelUnsubmitted)(void *context, void *job);
    int32_t (*Poll)(void *context, void *job, uint32_t *state);
    int32_t (*Retire)(void *context, void *job);
    int32_t (*ResetHistory)(void *context);
    int32_t (*Drain)(void *context);
    int32_t (*GetStatus)(void *context, char *buf, uint32_t buf_chars);
    int32_t (*GetLastError)(char *buf, uint32_t buf_chars);
    /* Fork addition (DLSS-NR for AMD, in-tree runtime): EnqueueHip without the D3D12 queue wait.
     * The wait on the HIP result is issued by RecordOutputs, so the host may record the consumer
     * one frame after the producer and the queue never blocks behind the network in between. */
    int32_t (*EnqueueHipAsync)(void *context, void *job, void *command_queue);
    /* Fork addition: retire a launched job without recording its consumer (frame size changed while
     * it was in flight). Waits for its HIP work on the CPU; nothing is recorded into any list. */
    int32_t (*AbandonJob)(void *context, void *job);
    /* Fork addition: whether a job launched with EnqueueHipAsync has finished on the GPU (the shared
     * fence reached the job's value), without waiting. A host that records the consumer only when
     * this reports 1 never stalls a frame behind the network; the answer is consumed when it is
     * there. Jobs launched with EnqueueHip (queue wait already issued) and passthrough jobs report 1. */
    int32_t (*OutputReady)(void *context, void *job, uint32_t *ready);
} LmxxfNrApi;

#ifdef _WIN32
#ifdef LMXXF_NR_RUNTIME_EXPORTS
#define LMXXF_NR_EXPORT __declspec(dllexport)
#else
#define LMXXF_NR_EXPORT __declspec(dllimport)
#endif
#else
#define LMXXF_NR_EXPORT
#endif

/* Main export; LmxxfNrGetImportPoolStats (0.3.3.2 rebuild) is optional and found with GetProcAddress.
 * Caller sets out->struct_size = sizeof(LmxxfNrApi) before the call. */
LMXXF_NR_EXPORT int32_t LmxxfNrGetApi(uint32_t abi_version, LmxxfNrApi *out);

/* AMDNR 0.3.3.2 rebuild (leak audit R3): the runtime imports its HIP-shared D3D12 buffers once per process and
 * reuses them across bridge rebuilds (AMD HIP never frees a mapped import). A host finds this export with
 * GetProcAddress: present = the buffers are reused (enabled 0 only under DLSS5_IMPORT_POOL=0), missing = an older
 * runtime that re-imports on every rebuild. buffers / busy: entries made / in use; bytes: their total; imports:
 * entries made; reuses: bridge buffers served from a free entry. Caller sets out->struct_size. LmxxfNrApi,
 * LMXXF_NR_ABI_VERSION and the capabilities are unchanged, so old and new hosts and runtimes load either way. */
typedef struct LmxxfNrImportPoolStats
{
    uint32_t struct_size;
    uint32_t enabled;
    uint32_t buffers;
    uint32_t busy;
    uint64_t bytes;
    uint64_t imports;
    uint64_t reuses;
} LmxxfNrImportPoolStats;
LMXXF_NR_EXPORT int32_t LmxxfNrGetImportPoolStats(LmxxfNrImportPoolStats *out);

/* AMDNR 0.3.4 (small network tiers; optional, found with GetProcAddress like LmxxfNrGetImportPoolStats): the host's
 * network tier policy for one context. flags bit 0 (1u) = small tiers: an input of at most 640x360 runs the 360 tier
 * (640x448 processed, 80 tokens) and one of at most 1024x576 the 576 tier (1024x640, 160 tokens), before the
 * 720 / 900 / 1080 tiers; any other bit is LMXXF_NR_INVALID_ARGUMENT. Default 0 (the tiers of 0.3.3.2). It takes effect
 * at the next geometry resolve (a host calls it once, right after Create; a later change rebuilds the network). The
 * handheld APUs (gfx1103, gfx115x except gfx1151) need bit 0 set before their first frame, else the runtime refuses them.
 * LmxxfNrApi, LMXXF_NR_ABI_VERSION, the structs and the frame flags are unchanged, so old and new hosts and runtimes
 * load either way (missing export = a runtime without the small tiers). */
LMXXF_NR_EXPORT int32_t LmxxfNrSetTierPolicy(void *context, uint32_t flags);

#ifdef __cplusplus
}
#endif
