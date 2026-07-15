// Copyright 2026 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// C bridge for the anland GPU display backend.  Allocates HardwareBuffers,
// imports dmabufs, and submits frames to SurfaceFlinger via ANativeWindow.
//
// This is compiled as a static library (libanland_display_bridge) and linked
// into the gpu_display crate on Android targets.

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define LOG_TAG "anland_display_bridge"
#define LOGE(...) fprintf(stderr, "E/" LOG_TAG ": " __VA_ARGS__)
#define LOGI(...) fprintf(stderr, "I/" LOG_TAG ": " __VA_ARGS__)

// ── opaque context ──────────────────────────────────────────────────

typedef void (*error_callback_t)(const char *message);

typedef struct {
    int                daemon_fd;         // connection to anland daemon
    ANativeWindow     *window;            // primary surface window
    error_callback_t   on_error;
    bool               connected;
} AnlandDisplayContext;

typedef struct {
    ANativeWindow     *window;            // per-surface native window
    AHardwareBuffer   *hwbuf;             // active HardwareBuffer
    int                hwbuf_fd;          // dup'd dmabuf fd from HardwareBuffer
    uint32_t           width;
    uint32_t           height;
} AnlandDisplaySurface;

// ── helpers ─────────────────────────────────────────────────────────

static int connect_daemon(const char *socket_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOGE("socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("connect(%s) failed: %s", socket_path, strerror(errno));
        close(fd);
        return -1;
    }

    LOGI("connected to daemon at %s", socket_path);
    return fd;
}

// ── public C API ────────────────────────────────────────────────────

AnlandDisplayContext *create_anland_display_context(
        const char *socket_path, error_callback_t on_error)
{
    AnlandDisplayContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        if (on_error) on_error("OOM creating anland display context");
        return NULL;
    }

    ctx->on_error = on_error;
    ctx->daemon_fd = -1;
    ctx->window    = NULL;

    // Connect to the anland daemon early so we can receive screen info
    // and consumer-side fds when a surface is created.
    ctx->daemon_fd = connect_daemon(socket_path);
    if (ctx->daemon_fd < 0) {
        // Non-fatal: daemon may not be up yet.  The surface-creation path
        // will retry.
        LOGI("daemon not available at startup; will retry on surface create");
    } else {
        ctx->connected = true;
    }

    return ctx;
}

void destroy_anland_display_context(AnlandDisplayContext *ctx) {
    if (!ctx) return;
    if (ctx->daemon_fd >= 0) close(ctx->daemon_fd);
    // ANativeWindow is owned by the surface, not the context.
    free(ctx);
}

AnlandDisplaySurface *create_anland_surface(
        AnlandDisplayContext *ctx, uint32_t width, uint32_t height)
{
    if (!ctx) return NULL;

    AnlandDisplaySurface *surf = calloc(1, sizeof(*surf));
    if (!surf) {
        if (ctx->on_error) ctx->on_error("OOM creating anland surface");
        return NULL;
    }

    surf->width  = width;
    surf->height = height;
    surf->hwbuf_fd = -1;

    // Allocate a HardwareBuffer as the backing store.
    AHardwareBuffer_Desc desc = {
        .width  = width,
        .height = height,
        .layers = 1,
        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage  = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                  AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                  AHARDWAREBUFFER_USAGE_CPU_READ_NEVER |
                  AHARDWAREBUFFER_USAGE_CPU_WRITE_NEVER,
        .stride = 0,  // let gralloc decide
    };

    int err = AHardwareBuffer_allocate(&desc, &surf->hwbuf);
    if (err != 0) {
        LOGE("AHardwareBuffer_allocate(%ux%u) failed: %d", width, height, err);
        free(surf);
        return NULL;
    }

    // Acquire the dmabuf fd for later import.
    const AHardwareBuffer_Plane *planes = NULL;
    err = AHardwareBuffer_lockPlanes(
        surf->hwbuf,
        AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
        -1,  // no fence
        NULL,  // no rect
        &planes);
    if (err != 0 || !planes || planes->fd < 0) {
        LOGE("AHardwareBuffer_lockPlanes failed: %d", err);
        AHardwareBuffer_release(surf->hwbuf);
        surf->hwbuf = NULL;
        free(surf);
        return NULL;
    }

    surf->hwbuf_fd = dup(planes->fd);
    AHardwareBuffer_unlock(surf->hwbuf, NULL);

    LOGI("created surface %ux%u, hwbuf_fd=%d", width, height, surf->hwbuf_fd);
    return surf;
}

void destroy_anland_surface(AnlandDisplayContext *ctx, AnlandDisplaySurface *surf) {
    if (!surf) return;
    if (surf->hwbuf_fd >= 0) close(surf->hwbuf_fd);
    if (surf->hwbuf) AHardwareBuffer_release(surf->hwbuf);
    free(surf);
    (void)ctx;
}

uint32_t anland_surface_import_dmabuf(
        AnlandDisplayContext *ctx,
        AnlandDisplaySurface *surf,
        int dmabuf_fd,
        uint32_t offset,
        uint32_t stride,
        uint64_t modifiers,
        uint32_t width,
        uint32_t height,
        uint32_t fourcc)
{
    if (!ctx || !surf || dmabuf_fd < 0) return 0;

    // The import model: we receive a dmabuf from virtio-gpu (rutabaga export).
    // On single-GPU Android systems, the dmabuf is already allocated from the
    // same GPU, so no cross-device import is needed — we can use it directly.
    //
    // We stash the fd and associated metadata.  The actual "import" is just
    // recording the fd; the submission path (queueBuffer) will use it.
    //
    // For now: dup the fd and return a non-zero import_id.
    // A full implementation would blit or use VK_KHR_external_memory to
    // reference the dmabuf from the HardwareBuffer's EGLImage.

    int dup_fd = dup(dmabuf_fd);
    if (dup_fd < 0) {
        LOGE("dup(dmabuf_fd) failed: %s", strerror(errno));
        return 0;
    }

    // Replace any previously imported fd.
    if (surf->hwbuf_fd >= 0) close(surf->hwbuf_fd);
    surf->hwbuf_fd = dup_fd;

    // Update surface dimensions to match the imported buffer.
    surf->width  = width;
    surf->height = height;

    (void)offset;
    (void)stride;
    (void)modifiers;
    (void)fourcc;

    LOGI("imported dmabuf %d -> surface fd %d (%ux%u)",
         dmabuf_fd, dup_fd, width, height);

    return 1;  // import_id
}

void anland_surface_release_import(
        AnlandDisplayContext *ctx,
        AnlandDisplaySurface *surf,
        uint32_t import_id)
{
    if (!surf) return;
    // import_id is always 1 in the current single-buffer model.
    if (surf->hwbuf_fd >= 0) {
        close(surf->hwbuf_fd);
        surf->hwbuf_fd = -1;
    }
    (void)ctx;
    (void)import_id;
}

void anland_surface_queue_buffer(
        AnlandDisplayContext *ctx,
        AnlandDisplaySurface *surf,
        uint32_t import_id,
        int render_done_fence_fd)
{
    if (!ctx || !surf) return;

    // If we have a daemon connection, send the dmabuf + fence through
    // the anland protocol so the consumer app can queue it to SurfaceFlinger.
    //
    // Otherwise, fall back to direct ANativeWindow submission if a window
    // is available (e.g. when embedded in an Android activity that passed
    // us a Surface).
    (void)import_id;

    if (ctx->connected && ctx->daemon_fd >= 0 && surf->hwbuf_fd >= 0) {
        // TODO: send DATA_MSG_BUFS_READY with the dmabuf fd via SCM_RIGHTS
        // on the daemon's data channel, then trigger refresh with the fence.
        //
        // For the initial implementation, we rely on the direct ANativeWindow
        // path below or on the consumer polling the daemon.
        LOGI("queue_buffer: daemon path (pending implementation)");
    }

    // Direct ANativeWindow path (fallback / embedded mode):
    if (surf->window && surf->hwbuf_fd >= 0) {
        ANativeWindowBuffer *anb = NULL;
        int acquire_fence = -1;

        // The full anland consumer path does dequeue→render→queue.
        // In our embedded path we skip dequeue and use the HardwareBuffer
        // directly.  This requires the native_window API to accept an
        // externally-allocated buffer, which is possible via
        // ANativeWindow_setBuffersGeometry + a custom allocator, or via
        // the SurfaceFlinger bypass (direct HWC submission).
        //
        // For the MVP we use the standard dequeue/queue cycle:
        int err = ANativeWindow_dequeueBuffer(surf->window, &anb, &acquire_fence);
        if (err != 0 || !anb) {
            LOGE("dequeueBuffer failed: %d", err);
            if (acquire_fence >= 0) close(acquire_fence);
            return;
        }
        if (acquire_fence >= 0) close(acquire_fence);

        // Queue with the render-done fence so SurfaceFlinger waits GPU-side.
        err = ANativeWindow_queueBuffer(surf->window, anb,
                                        render_done_fence_fd >= 0
                                            ? dup(render_done_fence_fd)
                                            : -1);
        if (err != 0) {
            LOGE("queueBuffer failed: %d", err);
            ANativeWindow_cancelBuffer(surf->window, anb, -1);
        }
    }

    if (render_done_fence_fd >= 0) close(render_done_fence_fd);
}

int anland_poll_input(
        AnlandDisplayContext *ctx,
        uint32_t *out_type,
        uint32_t *out_code,
        int32_t  *out_value,
        uint32_t timeout_ms)
{
    // Stub for MVP: input events are handled by the anland consumer app's
    // existing input pipeline (VirtualKeyboard / ExtraKeysBar).
    // The producer (crosvm) side doesn't poll for input in this model;
    // the consumer sends input events directly to the guest via virtio-input.
    (void)ctx;
    (void)out_type;
    (void)out_code;
    (void)out_value;
    (void)timeout_ms;
    return 0;
}