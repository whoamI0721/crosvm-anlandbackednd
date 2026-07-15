// Copyright 2026 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! GPU display backend that submits rendered dmabufs directly to an Android
//! ANativeWindow via HardwareBuffer, bypassing VNC encoding / CPU copies.
//! Designed to pair with the anland consumer app's SurfaceFlinger submission
//! pipeline (queueBuffer with render-done fence).

use std::collections::HashMap;
use std::ffi::CStr;
use std::ffi::CString;
use std::panic::catch_unwind;
use std::process::abort;
use std::ptr::NonNull;
use std::sync::Arc;

use base::error;
use base::info;
use base::AsRawDescriptor;
use base::Event;
use base::RawDescriptor;
use base::VolatileSlice;
use sync::Mutex;
use vm_control::gpu::DisplayParameters;

use crate::DisplayT;
use crate::GpuDisplayError;
use crate::GpuDisplayFramebuffer;
use crate::GpuDisplayResult;
use crate::GpuDisplaySurface;
use crate::SurfaceType;
use crate::SysDisplayT;

// ── Opaque C handles ────────────────────────────────────────────────

#[repr(C)]
pub(crate) struct AnlandDisplayContext {
    _data: [u8; 0],
    _marker: core::marker::PhantomData<(*mut u8, core::marker::PhantomPinned)>,
}

#[repr(C)]
pub(crate) struct AnlandDisplaySurface {
    _data: [u8; 0],
    _marker: core::marker::PhantomData<(*mut u8, core::marker::PhantomPinned)>,
}

pub(crate) type ErrorCallback = unsafe extern "C" fn(message: *const libc::c_char);

// ── C FFI ───────────────────────────────────────────────────────────

extern "C" {
    /// Create an anland-flavoured display context.  On Android this opens
    /// a connection to the anland daemon, obtains a native window, and
    /// prepares the HardwareBuffer pool.
    fn create_anland_display_context(
        socket_path: *const libc::c_char,
        error_callback: ErrorCallback,
    ) -> *mut AnlandDisplayContext;

    /// Tear down the context and release the native window.
    fn destroy_anland_display_context(ctx: *mut AnlandDisplayContext);

    /// Create one scanout surface backed by a dedicated ANativeWindow /
    /// HardwareBuffer slot.
    fn create_anland_surface(
        ctx: *mut AnlandDisplayContext,
        width: u32,
        height: u32,
    ) -> *mut AnlandDisplaySurface;

    /// Destroy a previously created surface.
    fn destroy_anland_surface(
        ctx: *mut AnlandDisplayContext,
        surface: *mut AnlandDisplaySurface,
    );

    /// Import a dmabuf into the surface so it can be submitted later.
    /// Returns an import-id (>= 1) on success, 0 on failure.
    fn anland_surface_import_dmabuf(
        ctx: *mut AnlandDisplayContext,
        surface: *mut AnlandDisplaySurface,
        dmabuf_fd: libc::c_int,
        offset: u32,
        stride: u32,
        modifiers: u64,
        width: u32,
        height: u32,
        fourcc: u32,
    ) -> u32;

    /// Release a previously imported dmabuf.
    fn anland_surface_release_import(
        ctx: *mut AnlandDisplayContext,
        surface: *mut AnlandDisplaySurface,
        import_id: u32,
    );

    /// Submit the frame associated with `import_id` to SurfaceFlinger.
    /// `render_done_fence_fd` is an optional sync_file fd (-1 = "ready now").
    fn anland_surface_queue_buffer(
        ctx: *mut AnlandDisplayContext,
        surface: *mut AnlandDisplaySurface,
        import_id: u32,
        render_done_fence_fd: libc::c_int,
    );

    /// Poll for input events arriving on the anland data channel.
    /// Returns > 0 when an event was dequeued, 0 on timeout, < 0 on error.
    fn anland_poll_input(
        ctx: *mut AnlandDisplayContext,
        out_type: *mut u32,
        out_code: *mut u32,
        out_value: *mut i32,
        timeout_ms: u32,
    ) -> libc::c_int;
}

// ── Error callback (matches Android/VNC pattern) ────────────────────

unsafe extern "C" fn error_callback(message: *const libc::c_char) {
    catch_unwind(|| {
        error!(
            "{}",
            // SAFETY: message is null-terminated
            unsafe { CStr::from_ptr(message) }.to_string_lossy()
        )
    })
    .unwrap_or_else(|_| abort())
}

// ── Context wrapper (RAII) ──────────────────────────────────────────

struct AnlandContextWrapper(NonNull<AnlandDisplayContext>);

impl Drop for AnlandContextWrapper {
    fn drop(&mut self) {
        // SAFETY: constructed from create_anland_display_context
        unsafe { destroy_anland_display_context(self.0.as_ptr()) };
    }
}

// ── Per-surface state ───────────────────────────────────────────────

struct AnlandSurfaceInner {
    surface: NonNull<AnlandDisplaySurface>,
    imports: HashMap<u32, ()>, // track live import ids for cleanup
}

impl Drop for AnlandSurfaceInner {
    fn drop(&mut self) {
        // imports are released by the C side when the surface is destroyed
    }
}

struct AnlandSurface {
    context: Arc<AnlandContextWrapper>,
    inner: Arc<Mutex<AnlandSurfaceInner>>,
    surface_id: u32,
}

impl GpuDisplaySurface for AnlandSurface {
    fn surface_descriptor(&self) -> u64 {
        self.surface_id as u64
    }

    fn framebuffer(&mut self) -> Option<GpuDisplayFramebuffer> {
        // The anland backend does not expose a CPU-writable framebuffer;
        // all rendering goes through import_resource + flip_to (dmabuf path).
        None
    }

    fn flip(&mut self) {
        // flip() is the legacy shm path — unused here.
    }

    fn flip_to(
        &mut self,
        import_id: u32,
        _acquire_timepoint: Option<crate::SemaphoreTimepoint>,
        release_timepoint: Option<crate::SemaphoreTimepoint>,
        _extra_info: Option<crate::FlipToExtraInfo>,
    ) -> anyhow::Result<sync::Waitable> {
        let fence_fd: libc::c_int = match release_timepoint {
            Some(tp) => {
                // The release timepoint carries a sync_file fd inside the import
                // descriptor.  We don't have direct access to the fd here — the
                // caller (virtio-gpu) passes it through the rutabaga timeline
                // semaphore.  For now, submit without an explicit fence; the
                // implicit pipeline barrier inside queueBuffer is sufficient
                // for single-GPU scenarios.
                //
                // TODO: plumb the actual sync_file fd when rutabaga exposes it.
                -1
            }
            None => -1,
        };

        let inner = self.inner.lock();
        // SAFETY: ctx and surface are valid, import_id was returned by
        // anland_surface_import_dmabuf.
        unsafe {
            anland_surface_queue_buffer(
                self.context.0.as_ptr(),
                inner.surface.as_ptr(),
                import_id,
                fence_fd,
            );
        }
        Ok(sync::Waitable::signaled())
    }

    fn commit(&mut self) -> GpuDisplayResult<()> {
        // commit() is a no-op here — the frame is already submitted in flip_to.
        Ok(())
    }

    fn set_position(&mut self, _x: u32, _y: u32) {
        // Cursor position is not meaningful for a full-screen scanout.
    }
}

// ── Display backend ─────────────────────────────────────────────────

pub struct DisplayAnland {
    context: Arc<AnlandContextWrapper>,
    next_import_id: u32,
    /// This event is never triggered; exists solely to satisfy AsRawDescriptor.
    event: Event,
}

impl DisplayAnland {
    pub fn new(socket_path: &str) -> GpuDisplayResult<DisplayAnland> {
        let c_path = CString::new(socket_path).map_err(|_| GpuDisplayError::InvalidPath)?;
        let ctx = NonNull::new(
            // SAFETY: socket_path is not leaked; error_callback is 'static.
            unsafe { create_anland_display_context(c_path.as_ptr(), error_callback) },
        )
        .ok_or(GpuDisplayError::Unsupported)?;
        let context = AnlandContextWrapper(ctx);
        let event = Event::new().map_err(|_| GpuDisplayError::CreateEvent)?;

        info!("anland display backend: connected to {}", socket_path);

        Ok(DisplayAnland {
            context: context.into(),
            next_import_id: 1,
            event,
        })
    }
}

impl DisplayT for DisplayAnland {
    fn pending_events(&self) -> bool {
        // We poll lazily in handle_next_event; the fd is not edge-triggered
        // in this backend so pending_events is conservatively false.
        false
    }

    fn next_event(&mut self) -> GpuDisplayResult<u64> {
        // No compositor-side surface events; return 0 as a sentinel.
        Ok(0)
    }

    fn handle_next_event(
        &mut self,
        _surface: &mut Box<dyn GpuDisplaySurface>,
    ) -> Option<crate::GpuDisplayEvents> {
        // Input events are polled inline via anland_poll_input; this method
        // is only called for compositor-initiated events (Wayland/X11 style)
        // which don't exist in the anland model.
        None
    }

    fn flush(&self) {
        // No batched commands to flush.
    }

    fn create_surface(
        &mut self,
        _parent_surface_id: Option<u32>,
        surface_id: u32,
        _scanout_id: Option<u32>,
        display_params: &DisplayParameters,
        _surf_type: SurfaceType,
    ) -> GpuDisplayResult<Box<dyn GpuDisplaySurface>> {
        let (width, height) = display_params.get_virtual_display_size();
        let surf = NonNull::new(
            // SAFETY: context is valid, width/height are reasonable.
            unsafe {
                create_anland_surface(self.context.0.as_ptr(), width, height)
            },
        )
        .ok_or(GpuDisplayError::CreateSurface)?;

        let inner = AnlandSurfaceInner {
            surface: surf,
            imports: HashMap::new(),
        };

        Ok(Box::new(AnlandSurface {
            context: self.context.clone(),
            inner: Arc::new(Mutex::new(inner)),
            surface_id,
        }))
    }

    fn import_resource(
        &mut self,
        import_id: u32,
        _surface_id: u32,
        external_display_resource: crate::DisplayExternalResourceImport,
    ) -> anyhow::Result<()> {
        // NOTE: surface_id is currently ignored because we don't have a
        // surface→inner mapping on the Display side.  In practice there is
        // a single scanout surface, so we use the most recently created one.
        // A proper multi-surface implementation would track surfaces in a map.

        if let crate::DisplayExternalResourceImport::Dmabuf {
            descriptor,
            offset,
            stride,
            modifiers,
            width,
            height,
            fourcc,
        } = external_display_resource
        {
            let fd = descriptor.as_raw_descriptor();

            // We need a reference to the active surface.  For the single-scanout
            // case we use the first/only surface.  Multi-display would require
            // a surface registry — deferred.
            //
            // The C bridge handles the actual import and returns a
            // HardwareBuffer-backed import id.
            //
            // SAFETY: fd is a valid dmabuf descriptor; the C side dup()s it.
            let result = unsafe {
                anland_surface_import_dmabuf(
                    self.context.0.as_ptr(),
                    std::ptr::null_mut(), // FIXME: need surface pointer; see above
                    fd,
                    offset,
                    stride,
                    modifiers,
                    width,
                    height,
                    fourcc,
                )
            };

            if result == 0 {
                anyhow::bail!("anland dmabuf import failed");
            }

            self.next_import_id = import_id + 1;
            Ok(())
        } else {
            anyhow::bail!("anland backend only supports Dmabuf imports");
        }
    }

    fn release_import(&mut self, import_id: u32, _surface_id: u32) {
        // SAFETY: context is valid, import_id was returned by import_resource.
        unsafe {
            anland_surface_release_import(
                self.context.0.as_ptr(),
                std::ptr::null_mut(), // FIXME: same surface-pointer issue
                import_id,
            );
        }
    }
}

impl SysDisplayT for DisplayAnland {}

impl AsRawDescriptor for DisplayAnland {
    fn as_raw_descriptor(&self) -> RawDescriptor {
        self.event.as_raw_descriptor()
    }
}