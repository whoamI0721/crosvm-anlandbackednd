// Copyright 2026 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// C bridge for the anland GPU display backend.
// Uses AHardwareBuffer for dmabuf-backed graphics buffers and
// ANativeWindow for submitting frames to SurfaceFlinger.
//
// Compiled as a static library and linked into the gpu_display crate.

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

#define LOG_TAG "anland_bridge"
#define LOGE(...) fprintf(stderr, "E/" LOG_TAG ": " __VA_ARGS__)
#define LOGI(...) fprintf(stderr, "I/" LOG_TAG ": " __VA_ARGS__)

// ── opaque context ──────────────────────────────────────────────────

typedef void (*error_callback_t)(const char *message);

typedef struct {
    int              daemon_fd;
    ANativeWindow   *window;
    error_callback_t on_error;
    bool             connected;
} AnlandDisplayContext;

typedef struct {
    ANativeWindow   *window;
    AHardwareBuffer *hwbuf;
    int              hwbuf_fd;
    uint32_t         width;
    uint32_t         height;
} AnlandDisplaySurface;

// ── daemon connection ───────────────────────────────────────────────

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

// ── context lifecycle ───────────────────────────────────────────────

AnlandDisplayContext *create_anland_display_context(
        const char *socket_path, error_callback_t on_error)
{
    AnlandDisplayContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        if (on_error) on_error("OOM creating anland display context");
        return NULL;
    }

    ctx->on_error  = on_error;
    ctx->daemon_fd = -1;
    ctx->window    = NULL;

    ctx->daemon_fd = connect_daemon(socket_path);
    if (ctx->daemon_fd < 0) {
        LOGI("daemon not available at startup; will retry on surface create");
    } else {
        ctx->connected = true;
    }

    return ctx;
}

void destroy_anland_display_context(AnlandDisplayContext *ctx) {
    if (!ctx) return;
    if (ctx->daemon_fd >= 0) close(ctx->daemon_fd);
    free(ctx);
}

// ── surface lifecycle ───────────────────────────────────────────────

AnlandDisplaySurface *create_anland_surface(
        AnlandDisplayContext *ctx, uint32_t width, uint32_t height)
{
    if (!ctx) return NULL;

    AnlandDisplaySurface *surf = calloc(1, sizeof(*surf));
    if (!surf) {
        if (ctx->on_error) ctx->on_error("OOM creating anland surface");
        return NULL;
    }

    surf->width    = width;
    surf->height   = height;
    surf->hwbuf_fd = -1;

    // Allocate a HardwareBuffer.
    AHardwareBuffer_Desc desc = {
        .width  = width,
        .height = height,
        .layers = 1,
        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage  = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                  AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                  AHARDWAREBUFFER_USAGE_CPU_READ_NEVER |
                  AHARDWAREBUFFER_USAGE_CPU_WRITE_NEVER,
        .stride = 0,
    };

    int err = AHardwareBuffer_allocate(&desc, &surf->hwbuf);
    if (err != 0) {
        LOGE("AHardwareBuffer_allocate(%ux%u) failed: %d", width, height, err);
        free(surf);
        return NULL;
    }

    LOGI("allocated surface %ux%u", width, height);
    return surf;
}

void destroy_anland_surface(AnlandDisplayContext *ctx,
                            AnlandDisplaySurface *surf) {
    if (!surf) return;
    if (surf->hwbuf_fd >= 0) close(surf->hwbuf_fd);
    if (surf->hwbuf) AHardwareBuffer_release(surf->hwbuf);
    free(surf);
    (void)ctx;
}

// ── dmabuf import ───────────────────────────────────────────────────

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

    // The dmabuf comes from virtio-gpu (rutabaga export).
    // On single-GPU Android systems the buffer is already allocated
    // from the same GPU allocator — no cross-device import needed.
    //
    // We stash the fd. The submission path uses it directly.
    // A full implementation would use VK_KHR_external_memory or
    // EGL_ANDROID_image_native_buffer to bind the dmabuf to the
    // HardwareBuffer's EGLImage.

    int dup_fd = dup(dmabuf_fd);
    if (dup_fd < 0) {
        LOGE("dup(dmabuf_fd) failed: %s", strerror(errno));
        return 0;
    }

    if (surf->hwbuf_fd >= 0) close(surf->hwbuf_fd);
    surf->hwbuf_fd = dup_fd;
    surf->width    = width;
    surf->height   = height;

    (void)offset;
    (void)stride;
    (void)modifiers;
    (void)fourcc;

    LOGI("imported dmabuf %d -> surface fd %d (%ux%u)",
         dmabuf_fd, dup_fd, width, height);

    return 1;
}

void anland_surface_release_import(
        AnlandDisplayContext *ctx,
        AnlandDisplaySurface *surf,
        uint32_t import_id)
{
    if (!surf) return;
    if (surf->hwbuf_fd >= 0) {
        close(surf->hwbuf_fd);
        surf->hwbuf_fd = -1;
    }
    (void)ctx;
    (void)import_id;
}

// ── frame submission ────────────────────────────────────────────────

void anland_surface_queue_buffer(
        AnlandDisplayContext *ctx,
        AnlandDisplaySurface *surf,
        uint32_t import_id,
        int render_done_fence_fd)
{
    if (!ctx || !surf) {
        if (render_done_fence_fd >= 0) close(render_done_fence_fd);
        return;
    }

    (void)import_id;

    // Daemon path: send dmabuf fd + fence via SCM_RIGHTS.
    // TODO: implement DATA_MSG_BUFS_READY protocol.
    if (ctx->connected && ctx->daemon_fd >= 0 && surf->hwbuf_fd >= 0) {
        LOGI("queue_buffer: daemon path (pending)");
    }

    // Direct ANativeWindow path (fallback / embedded mode).
    // Uses the standard dequeue → queue cycle.
    if (surf->window && surf->hwbuf_fd >= 0) {
        ANativeWindow_Buffer *anb = NULL;
        int acquire_fence = -1;

        int err = ANativeWindow_dequeueBuffer(
                surf->window, &anb, &acquire_fence);
        if (err != 0 || !anb) {
            LOGE("dequeueBuffer failed: %d", err);
            if (acquire_fence >= 0) close(acquire_fence);
            if (render_done_fence_fd >= 0) close(render_done_fence_fd);
            return;
        }
        if (acquire_fence >= 0) close(acquire_fence);

        err = ANativeWindow_queueBuffer(
                surf->window, anb,
                render_done_fence_fd >= 0 ? dup(render_done_fence_fd) : -1);
        if (err != 0) {
            LOGE("queueBuffer failed: %d", err);
            ANativeWindow_cancelBuffer(surf->window, anb, -1);
        }
    }

    if (render_done_fence_fd >= 0) close(render_done_fence_fd);
}

// ── input poll (stub) ───────────────────────────────────────────────

int anland_poll_input(
        AnlandDisplayContext *ctx,
        uint32_t *out_type,
        uint32_t *out_code,
        int32_t  *out_value,
        uint32_t timeout_ms)
{
    // Input events are handled by the anland consumer app's
    // existing input pipeline (VirtualKeyboard / ExtraKeysBar).
    (void)ctx;
    (void)out_type;
    (void)out_code;
    (void)out_value;
    (void)timeout_ms;
    return 0;
}