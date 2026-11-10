//! Binds one FFX frame generation context to one Sedici-wrapped swapchain
//! (ports `runtime/integration.cpp`).
//!
//! The session owns the [`Context`] behind a shared lock and, once configured, a reference to the
//! swapchain wrapper it presents through. Ownership rules from the C++:
//!
//! * A wrapped swapchain is registered when created and unregistered when its wrap handle is
//!   destroyed. Game-held COM references may survive, but the generator gate is closed first.
//! * A frame generation session never frees a swapchain it did not wrap.
//! * Neither object reaches into the other's GPU work; the swapchain drives the recording and
//!   reports the fence it submitted, so retirement stays with the object that owns the list.
//!
//! # The swapchain seam
//!
//! The presentation slice (`application/presentation/swapchain.rs`) is written in parallel, so the
//! session talks to the wrapper through [`Wrapper`] — the exact operations the C++ `Session` used on
//! `Swapchain`: `set_generator`, `clear_generator_if_owner`, `close_generator` and the two
//! interpolation scratch queries. The composition root supplies the concrete implementation.
use std::sync::Arc;

use thiserror::Error;

use crate::application::context::{
    Context, ContextHandle, ContextSink, DeviceBinding, SharedContext,
};
use crate::application::faults::Faults;
use crate::application::ffx_model::{ConfigureDesc, CreateContextDesc, DispatchDesc, OpaqueHandle};
use crate::application::ports::generator::FrameGenerator;
use crate::application::ports::gpu::GpuDevice;
use crate::domain::error::ErrorCode;

/// The lifecycle of a [`Session`]. Accepted work per state matches the C++ enum.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SessionState {
    /// Nothing created yet, or creation failed. Every call is refused.
    Empty,
    /// Created but not yet configured.
    Ready,
    /// Configured with generation off; configure and prepare may be accepted.
    Suspended,
    /// Generation requested but no live wrapper is bound.
    Degraded,
    /// The D3D12 device is removed; new configure/prepare are refused.
    DeviceLost,
    /// Generation on and a swapchain bound, but no motion history yet.
    Warmup,
    /// Generation on, bound, with history: frames are generated.
    Active,
    /// Destroy has begun: new configure/prepare are refused while the last submission retires.
    Draining,
    /// Destroyed. Every call is refused; the object can be freed.
    Stopped,
}

impl SessionState {
    pub const fn name(self) -> &'static str {
        match self {
            Self::Empty => "empty",
            Self::Ready => "ready",
            Self::Suspended => "suspended",
            Self::Degraded => "degraded",
            Self::DeviceLost => "device-lost",
            Self::Warmup => "warmup",
            Self::Active => "active",
            Self::Draining => "draining",
            Self::Stopped => "stopped",
        }
    }
}

/// The wrapper operations a session needs (ports `Swapchain`'s generator half).
pub trait Wrapper: Send + Sync {
    /// Installs a generation sink. False once `close_generator` has run; a destroyed wrapper
    /// context may not be reattached, even if the game still holds a COM reference.
    fn set_generator(&self, sink: Arc<dyn ContextHandle>) -> bool;

    /// Clears only the sink `owner` installed; a stale session must not detach a newer session's
    /// sink.
    fn clear_generator_if_owner(&self, owner: usize) -> bool;

    /// Atomically detach and forbid later attachment; waits for an in-flight Present callback.
    fn close_generator(&self);

    /// The wrapper's scratch interpolation command list (AW2, K131), or `None` when buffers are
    /// not created yet. Its lock is separate from the present lock (K138).
    fn interpolation_list(&self) -> Option<OpaqueHandle>;

    /// The wrapper's scratch interpolation texture, already in the FFX resource form the game
    /// receives, or `None` when buffers are not created yet.
    fn interpolation_texture(&self) -> Option<crate::application::ffx_model::Resource>;
}

/// How the wrapper was made, so a failed binding can roll back correctly.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WrapOrigin {
    /// The game handed its native chain over; a failure releases the wrapper the game never saw.
    Wrapped,
    /// Sedici built the chain from the game's factory and desc (the New and HWND forms).
    Created,
}

/// A refused session call. The reason string is what the router logs and what drives the stable
/// [`ErrorCode`].
#[derive(Debug, Clone, Error)]
#[error("{reason} [code={}]", code.name())]
pub struct SessionError {
    pub code: ErrorCode,
    pub reason: String,
}

impl SessionError {
    fn new(reason: impl Into<String>) -> Self {
        let reason = reason.into();
        Self { code: ErrorCode::classify(&reason), reason }
    }

    /// The reason the C++ session stored for a device-removed refusal.
    pub fn device_removed() -> Self {
        Self::new("D3D12 device removed")
    }
}

/// One frame generation context plus, once configured, the swapchain it presents through.
///
/// The context lives behind a [`SharedContext`] because the wrapper's generation callback runs on
/// the presentation thread while the game drives configure/prepare on its own. Every public method
/// takes that lock; the wrapper never holds it while it presents beyond the call itself (K104).
pub struct Session<D: GpuDevice + Send + 'static, G: FrameGenerator<D> + Send + 'static> {
    context: SharedContext<D, G>,
    /// The identity the wrapper uses to tell this session's sink from another's. The token the
    /// router issued is unique for the process lifetime, so it is a sound key.
    owner: usize,
    swapchain: Option<Arc<dyn Wrapper>>,
    configured: bool,
    draining: bool,
    stopped: bool,
    last_error: String,
    _faults: Arc<Faults>,
}
impl<D: GpuDevice + Send + 'static, G: FrameGenerator<D> + Send + 'static> Session<D, G> {
    /// `owner` is the key the wrapper uses for `clear_generator_if_owner`.
    pub fn new(context: Context<D, G>, faults: Arc<Faults>, owner: usize) -> Self {
        Self {
            context: Arc::new(std::sync::Mutex::new(context)),
            owner,
            swapchain: None,
            configured: false,
            draining: false,
            stopped: false,
            last_error: String::new(),
            _faults: faults,
        }
    }

    /// The shared handle the router installs its sink around.
    pub fn shared_context(&self) -> SharedContext<D, G> {
        Arc::clone(&self.context)
    }

    /// A sink bound to this session's context.
    pub fn sink(&self) -> Arc<dyn ContextHandle> {
        Arc::new(ContextSink::new(self.shared_context()))
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, Context<D, G>> {
        self.context.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    pub fn state(&self) -> SessionState {
        if self.stopped {
            return SessionState::Stopped;
        }
        if self.draining {
            return SessionState::Draining;
        }
        let context = self.lock();
        if context.device_lost() {
            return SessionState::DeviceLost;
        }
        if !context.ready() {
            return SessionState::Empty;
        }
        if !self.configured {
            return SessionState::Ready;
        }
        if !context.enabled() {
            return SessionState::Suspended;
        }
        if self.swapchain.is_none() {
            return SessionState::Degraded;
        }
        if context.have_history() {
            SessionState::Active
        } else {
            SessionState::Warmup
        }
    }

    pub fn ready(&self) -> bool {
        if self.draining || self.stopped {
            return false;
        }
        let context = self.lock();
        context.ready() && !context.device_lost()
    }

    pub fn last_error(&self) -> &str {
        let lost = self.lock().device_lost();
        if lost {
            "D3D12 device removed"
        } else {
            &self.last_error
        }
    }

    /// True when no submitted GPU work can still reference this session.
    pub fn retired(&self) -> bool {
        self.stopped || self.lock().retired()
    }

    /// Binds the session to a wrapped swapchain. Called once, before publication.
    pub fn bind_wrapper(&mut self, wrapper: Arc<dyn Wrapper>) {
        self.swapchain = Some(wrapper);
    }

    pub fn create(&mut self, desc: &CreateContextDesc, device: DeviceBinding<D>) -> bool {
        if self.draining || self.stopped {
            return false;
        }
        let failure = {
            let mut context = self.lock();
            if context.ready() {
                return false;
            }
            match context.create(desc, device) {
                Ok(()) => None,
                Err(error) => Some(error.reason().to_owned()),
            }
        };
        match failure {
            None => true,
            Some(reason) => {
                self.last_error = reason;
                false
            }
        }
    }

    /// Applies a configure. `sink` is installed on the bound wrapper when generation turns on, or
    /// cleared (only when this session owns it) when it turns off.
    pub fn configure(&mut self, desc: &ConfigureDesc, sink: Arc<dyn ContextHandle>) -> bool {
        if self.draining || self.stopped {
            return false;
        }
        {
            let mut context = self.lock();
            if !context.ready() || context.device_lost() {
                return false;
            }
            if let Err(error) = context.configure(desc) {
                let reason = error.reason().to_owned();
                drop(context);
                self.last_error = reason;
                return false;
            }
        }
        self.configured = true;
        let enabled = self.lock().enabled();
        if let Some(wrapper) = self.swapchain.clone() {
            if enabled {
                if !wrapper.set_generator(sink) {
                    // Destruction may have closed this wrapper after lookup.
                    self.swapchain = None;
                }
            } else {
                // Another session may have replaced our sink; disabling this one must not detach
                // the other owner.
                let _ = wrapper.clear_generator_if_owner(self.owner);
            }
        }
        true
    }

    pub fn prepare(&mut self, desc: &DispatchDesc) -> bool {
        if self.draining || self.stopped {
            return false;
        }
        let failure = {
            let mut context = self.lock();
            if !context.ready() || context.device_lost() {
                return false;
            }
            match context.prepare(desc) {
                Ok(()) => None,
                Err(error) => Some(error.reason().to_owned()),
            }
        };
        match failure {
            None => true,
            Some(reason) => {
                self.last_error = reason;
                false
            }
        }
    }

    pub fn destroy(&mut self) -> bool {
        if self.stopped {
            return true;
        }
        if !self.lock().ready() {
            self.stopped = true;
            return true;
        }
        self.draining = true;
        // Do not hold the context lock while waiting for the wrapper: an in-flight callback holds
        // the wrapper lock and may need the context lock (K104). New work is already refused while
        // draining, so the state can advance only through a registration or the fence completing.
        self.detach();
        // 1000 ms, not 200: at game exit by Alt+F4 a 200 ms window was too short (K127).
        const RETIRE_WAIT_MS: u32 = 1000;
        let deadline = std::time::Instant::now() + std::time::Duration::from_millis(u64::from(RETIRE_WAIT_MS));
        loop {
            // Poll with the lock released between looks (K104): the wrapper's untracked-sink
            // resolution takes this lock under the wrapper lock, and holding it for the whole
            // window could stall a Present.
            let retired = self.lock().retired();
            if retired || std::time::Instant::now() >= deadline {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(1));
        }
        let ok = {
            let mut context = self.lock();
            context.wait_for_retirement(RETIRE_WAIT_MS) && context.destroy().is_ok()
        };
        // A refused destroy (work still in flight) leaves the session Draining: it keeps refusing
        // new work and the owner must retain it until it retires.
        if ok {
            self.draining = false;
            self.stopped = true;
        } else {
            self.last_error = "destroy before GPU retirement".to_owned();
        }
        ok
    }

    /// Releases the wrapper reference without touching the context or GPU work.
    pub fn detach(&mut self) {
        if let Some(wrapper) = self.swapchain.take() {
            let _ = wrapper.clear_generator_if_owner(self.owner);
        }
    }
}
