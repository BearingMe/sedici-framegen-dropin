//! Handle tokens, ownership and call dispatch (ports `abi/route.cpp`).
//!
//! Sedici is the only backend: the router creates the frame generation context and the swapchain
//! wrapper, and there is no second implementation to hand a call to. Handles outside the reserved
//! token namespace are never ours (the owner approved that assumption, K29); an issued token stays
//! locally identifiable after destroy and is rejected, never reused.
//!
//! # Owner-approved exceptions
//!
//! The token namespace is reserved before backend creation and a create that cannot reserve it is
//! rejected before consuming game resources. Routing-owner and wrapper-registry allocation may fail
//! with a local create error only before any native chain is consumed. Every other bookkeeping
//! failure is non-fatal: a game call never fails because of Sedici's own accounting.
//!
//! # Rejection codes
//!
//! A rejection returns the FFX code the C++ chose, which is not a function of the stable
//! [`ErrorCode`] alone:
//!
//! | Case | Code |
//! |---|---|
//! | null handle, or a handle Sedici never issued | `ERROR_PARAMETER` |
//! | a token that was destroyed | `ERROR` |
//! | null descriptor on a Sedici-owned context | `ERROR` |
//! | a create naming two context forms | `ERROR` |
//! | an unknown create chain | `ERROR_UNKNOWN_DESCTYPE` (answered by the export) |
//! | GetVersions for an unknown effect | `ERROR_UNKNOWN_DESCTYPE` |
//! | GetVersions without an output count | `ERROR_PARAMETER` |
use std::collections::BTreeMap;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};

use crate::application::context::{Context, DeviceBinding, FrameObjects};
use crate::application::faults::{BookkeepingFault, Faults};
use crate::application::ffx_model::{
    self as model, ConfigureDesc, ContextToken, CreateContextDesc, DispatchDesc, DispatchReply,
    GetVersions, OpaqueHandle, QueryDesc, QueryReply, Rejection, ReturnCode,
};
use crate::application::ports::generator::FrameGenerator;
use crate::application::ports::gpu::GpuDevice;
use crate::application::session::{Session, Wrapper};
use crate::domain::error::ErrorCode;
use crate::domain::token::TokenNamespace;

// ---------------------------------------------------------------------------------------------
// Descriptor type ids the router routes on
// ---------------------------------------------------------------------------------------------

/// The create types a version query can name.
pub const CREATE_FRAMEGENERATION: u64 = 0x0002_0001;
pub const CREATE_SWAPCHAIN_WRAP_DX12: u64 = 0x0003_0001;
pub const CREATE_SWAPCHAIN_NEW_DX12: u64 = 0x0003_0005;
pub const CREATE_SWAPCHAIN_FOR_HWND_DX12: u64 = 0x0003_0006;

/// The dispatch types the router routes on.
pub const DISPATCH_FRAMEGENERATION: u64 = 0x0002_0003;
pub const DISPATCH_FRAMEGENERATION_PREPARE: u64 = 0x0002_0004;
pub const DISPATCH_FRAMEGENERATION_PREPARE_V2: u64 = 0x0002_000c;

/// One provider version: the create desc type it answers, its numeric id and its NUL-terminated
/// name (the game receives that pointer directly).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ProviderVersion {
    pub create_desc_type: u64,
    pub id: u64,
    pub name: &'static std::ffi::CStr,
}

/// The provider versions the original AMD binary reported for the same create types
/// (`docs/refactor/amd-binary.md`). Sedici reports exactly one per recognised type so a game that
/// enumerates versions sees the same shape it saw before. The New and HWND forms share the
/// swapchain provider (Alan Wake 2 sends `0xc01007` with the New form, finding 014).
pub const PROVIDER_VERSIONS: [ProviderVersion; 4] = [
    ProviderVersion {
        create_desc_type: CREATE_FRAMEGENERATION,
        id: 0xf600_0000_00c0_1006,
        name: c"3.1.6",
    },
    ProviderVersion {
        create_desc_type: CREATE_SWAPCHAIN_WRAP_DX12,
        id: 0xf65c_dd12_00c0_1007,
        name: c"3.1.7",
    },
    ProviderVersion {
        create_desc_type: CREATE_SWAPCHAIN_NEW_DX12,
        id: 0xf65c_dd12_00c0_1007,
        name: c"3.1.7",
    },
    ProviderVersion {
        create_desc_type: CREATE_SWAPCHAIN_FOR_HWND_DX12,
        id: 0xf65c_dd12_00c0_1007,
        name: c"3.1.7",
    },
];

/// The provider for a create type, if Sedici recognises it.
pub fn provider_for(create_desc_type: u64) -> Option<&'static ProviderVersion> {
    PROVIDER_VERSIONS
        .iter()
        .find(|version| version.create_desc_type == create_desc_type)
}

/// The token namespace geometry: 256 MiB reserved, 16 bytes apart, never committed (K29).
pub const TOKEN_BYTES: usize = 256 * 1024 * 1024;
pub const TOKEN_STRIDE: usize = 16;

// ---------------------------------------------------------------------------------------------
// Injected seams
// ---------------------------------------------------------------------------------------------

/// Issues and recognises the process-lifetime handle tokens. Split out so the offline tests can
/// use a small in-memory namespace instead of reserving 256 MiB of address space.
pub trait TokenSource: Send + Sync {
    fn issue(&self) -> Option<OpaqueHandle>;
    fn contains_issued(&self, handle: OpaqueHandle) -> bool;
}

/// The process token source: one address reservation behind one `TokenNamespace`.
pub struct ProcessTokens {
    faults: Arc<Faults>,
    reservation: Arc<dyn AddressReservation>,
    namespace: Mutex<Option<TokenNamespace>>,
    reserved: OnceLock<bool>,
}

impl ProcessTokens {
    pub fn new(faults: Arc<Faults>, reservation: Arc<dyn AddressReservation>) -> Self {
        Self {
            faults,
            reservation,
            namespace: Mutex::new(None),
            reserved: OnceLock::new(),
        }
    }

    /// Reserves the namespace once. False when it cannot be reserved; a create is then refused
    /// before consuming game resources (K29).
    fn reserve(&self) -> bool {
        *self.reserved.get_or_init(|| {
            let Some(base) = self.reservation.reserve(TOKEN_BYTES) else { return false };
            *self.namespace.lock().unwrap_or_else(|poisoned| poisoned.into_inner()) =
                Some(TokenNamespace::new(base, TOKEN_BYTES, TOKEN_STRIDE));
            tracing::info!(
                "fg: module pinned in memory; a FreeLibrary is now a no-op (handle namespace is process-lifetime)"
            );
            true
        })
    }
}

impl TokenSource for ProcessTokens {
    fn issue(&self) -> Option<OpaqueHandle> {
        if !self.reserve() {
            return None;
        }
        if self.faults.consume(BookkeepingFault::TokenReservation) {
            return None;
        }
        self.namespace
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .as_mut()
            .and_then(TokenNamespace::issue)
    }

    fn contains_issued(&self, handle: OpaqueHandle) -> bool {
        self.namespace
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .as_ref()
            .is_some_and(|namespace| namespace.contains_issued(handle))
    }
}

/// An in-memory token source for the offline tests: no address reservation, the same refusal and
/// never-reuse rules.
pub struct MemoryTokens {
    faults: Arc<Faults>,
    state: Mutex<MemoryTokenState>,
}

struct MemoryTokenState {
    namespace: TokenNamespace,
}

impl MemoryTokens {
    /// `base`/`bytes`/`stride` define the reserved range; reserve it eagerly so the first issue
    /// succeeds.
    pub fn new(faults: Arc<Faults>, base: usize, bytes: usize, stride: usize) -> Self {
        Self {
            faults,
            state: Mutex::new(MemoryTokenState {
                namespace: TokenNamespace::new(base, bytes, stride),
            }),
        }
    }
}

impl TokenSource for MemoryTokens {
    fn issue(&self) -> Option<OpaqueHandle> {
        if self.faults.consume(BookkeepingFault::TokenReservation) {
            return None;
        }
        self.state
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .namespace
            .issue()
    }

    fn contains_issued(&self, handle: OpaqueHandle) -> bool {
        self.state
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .namespace
            .contains_issued(handle)
    }
}

/// What the router needs from the presentation slice for a swapchain create form: build or adopt a
/// wrapper and hand it back as the game's chain handle.
pub trait SwapchainFactory: Send + Sync {
    /// Wraps the chain the game handed over or builds one from the game's factory and desc.
    ///
    /// `owner` is the create token the router issued; it is unique for the process lifetime, so the
    /// wrapper can tell one session's generator sink from another's. Returns the game-facing chain
    /// handle and the wrapper the session binds to.
    fn create_wrapper(
        &self,
        desc: &CreateContextDesc,
        origin: crate::application::session::WrapOrigin,
        owner: OpaqueHandle,
    ) -> Result<(OpaqueHandle, Arc<dyn Wrapper>), Rejection>;
}

/// Resolves the game's FFX backend device handle into a port device for a session create.
pub trait DeviceResolver<D: GpuDevice>: Send + Sync {
    fn resolve(&self, handle: OpaqueHandle) -> Option<DeviceBinding<D>>;
}

/// Hook that runs once per application Present on every wrapped swapchain.
pub type PresentHook = Arc<dyn Fn(u64) + Send + Sync>;

// ---------------------------------------------------------------------------------------------
// Call identity (P2.10)
// ---------------------------------------------------------------------------------------------

thread_local! {
    static CALL_OP: std::cell::RefCell<(&'static str, OpaqueHandle)> =
        const { std::cell::RefCell::new(("?", 0)) };
}

/// Marks the router entry running on this thread, restoring the previous one on drop, and logs the
/// call when it took 50 ms or more (K121/K122).
pub struct CallScope {
    previous: (&'static str, OpaqueHandle),
    op: &'static str,
    start: std::time::Instant,
}

impl CallScope {
    pub fn enter(op: &'static str, context: OpaqueHandle) -> Self {
        let previous = CALL_OP.with(|cell| *cell.borrow());
        CALL_OP.with(|cell| *cell.borrow_mut() = (op, context));
        Self {
            previous,
            op,
            start: std::time::Instant::now(),
        }
    }
}

impl Drop for CallScope {
    fn drop(&mut self) {
        // Say how long the game was inside Sedici, so a stall in the game's own prepare interval
        // can be attributed to Sedici or not (K121/K122).
        let ms = self.start.elapsed().as_secs_f64() * 1000.0;
        if ms >= 50.0 {
            let (op, context) = CALL_OP.with(|cell| *cell.borrow());
            let _ = self.op;
            tracing::info!("fg: slow call op={op} ctx=0x{context:x} took {ms:.1} ms");
        }
        CALL_OP.with(|cell| *cell.borrow_mut() = self.previous);
    }
}

/// The current call's ` [op=... ctx=...]` identity, or empty when no router entry is running.
pub fn call_identity() -> String {
    CALL_OP.with(|cell| {
        let (op, context) = *cell.borrow();
        if op == "?" && context == 0 {
            String::new()
        } else {
            format!(" [op={op} ctx=0x{context:x}]")
        }
    })
}

// ---------------------------------------------------------------------------------------------
// Owners
// ---------------------------------------------------------------------------------------------

/// The kinds of owners the registry holds.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OwnerKind {
    /// A frame generation context plus its session.
    Session,
    /// A wrapped swapchain.
    Wrap,
}

/// The mutable body of one owner. It is a [`Mutex`] so calls on one owner serialize (sessions and
/// wrapped chains are not reentrant) while different owners run concurrently.
pub struct OwnerBody<D: GpuDevice + Send + 'static, G: FrameGenerator<D> + Send + 'static> {
    pub session: Option<Session<D, G>>,
    pub wrap: Option<Arc<dyn Wrapper>>,
    /// The chain handle the game holds for a wrap owner.
    pub swapchain: OpaqueHandle,
    pub dead: bool,
}

/// One registered owner. A call that has looked a handle up holds a shared lease (`Arc`), so the
/// owner stays alive while the call runs, and the registry lock is never held across a backend
/// call. `body` serialises calls on this owner only; `dead` is set under it by destroy, so a lease
/// obtained just before destroy cannot use a torn-down owner.
pub struct Owned<D: GpuDevice + Send + 'static, G: FrameGenerator<D> + Send + 'static> {
    pub kind: OwnerKind,
    pub body: Mutex<OwnerBody<D, G>>,
    /// What the game last asked for, so the limiter and the menu can say whether frame generation
    /// is on without reaching into a session another thread may be using.
    pub generation_on: AtomicBool,
    /// Retained because destroy was refused; reaped on a later router call once work retires.
    pub parked: AtomicBool,
}

impl<D: GpuDevice + Send + 'static, G: FrameGenerator<D> + Send + 'static> Owned<D, G> {
    fn new(kind: OwnerKind) -> Self {
        Self {
            kind,
            body: Mutex::new(OwnerBody {
                session: None,
                wrap: None,
                swapchain: 0,
                dead: false,
            }),
            generation_on: AtomicBool::new(false),
            parked: AtomicBool::new(false),
        }
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, OwnerBody<D, G>> {
        self.body
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }
}

/// The router for one process.
///
/// Generic over the GPU device and generator, because a process has exactly one backend device and
/// one generator implementation. Every owner it holds is of that pair.
pub struct Router<D: GpuDevice + Send + 'static, G: FrameGenerator<D> + Send + 'static> {
    faults: Arc<Faults>,
    objects: Arc<dyn FrameObjects<D, G::List>>,
    devices: Arc<dyn DeviceResolver<D>>,
    swapchains: Arc<dyn SwapchainFactory>,
    tokens: Arc<dyn TokenSource>,

    owned: Mutex<BTreeMap<OpaqueHandle, Arc<Owned<D, G>>>>,
    /// Owners whose destroy was refused because GPU work had not retired (K41).
    parked: Mutex<Vec<Arc<Owned<D, G>>>>,
    parked_count: AtomicU64,

    present_hook: Mutex<Option<PresentHook>>,
}

impl<D: GpuDevice + Send, G: FrameGenerator<D> + Send> Router<D, G> {
    /// Builds the router with the ports the composition root supplies. No token is issued yet.
    pub fn new(
        objects: Arc<dyn FrameObjects<D, G::List>>,
        devices: Arc<dyn DeviceResolver<D>>,
        swapchains: Arc<dyn SwapchainFactory>,
        tokens: Arc<dyn TokenSource>,
        faults: Arc<Faults>,
    ) -> Self {
        Self {
            faults,
            objects,
            devices,
            swapchains,
            tokens,
            owned: Mutex::new(BTreeMap::new()),
            parked: Mutex::new(Vec::new()),
            parked_count: AtomicU64::new(0),
            present_hook: Mutex::new(None),
        }
    }

    /// The fault injector, for the offline host.
    pub fn faults(&self) -> &Arc<Faults> {
        &self.faults
    }

    /// Owners kept alive because their GPU work had not retired when the game destroyed them.
    pub fn parked_count(&self) -> u64 {
        self.parked_count.load(Ordering::Acquire)
    }

    /// True when at least one live session was last configured with generation enabled.
    pub fn any_generation_enabled(&self) -> bool {
        let registry = self
            .owned
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        registry.values().any(|owner| {
            owner.kind == OwnerKind::Session && owner.generation_on.load(Ordering::Relaxed)
        })
    }

    /// Installs a hook that runs once per application Present on every wrapped swapchain.
    pub fn set_present_hook(&self, hook: Option<PresentHook>) {
        *self
            .present_hook
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = hook;
    }

    /// The installed Present hook, for a wrapper that initialized after it was set (K60).
    pub fn present_hook(&self) -> Option<PresentHook> {
        self.present_hook
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .clone()
    }

    fn failure(&self, reason: impl Into<String>, ffx: ReturnCode) -> Rejection {
        Rejection::new(ffx, reason)
    }

    fn allocation(&self, reason: &str) -> Rejection {
        Rejection::with_code(ReturnCode::Error, ErrorCode::Allocation, reason)
    }

    fn find_owned(&self, handle: OpaqueHandle) -> Option<Arc<Owned<D, G>>> {
        if handle == 0 {
            return None;
        }
        let registry = self
            .owned
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        registry.get(&handle).cloned()
    }

    fn publish(&self, token: OpaqueHandle, owner: Arc<Owned<D, G>>) {
        let mut registry = self
            .owned
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        registry.insert(token, owner);
    }

    fn rollback(&self, token: OpaqueHandle) {
        let mut registry = self
            .owned
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        registry.remove(&token);
    }

    // -- Create --------------------------------------------------------------------------------

    pub fn create(&self, desc: &CreateContextDesc) -> Result<model::CreatedContext, Rejection> {
        let _scope = CallScope::enter("create", 0);
        self.reap_parked();

        let forms = [
            desc.frame_generation.is_some(),
            desc.swapchain_wrap.is_some(),
            desc.swapchain_new.is_some(),
            desc.swapchain_for_hwnd.is_some(),
        ];
        let named = forms.iter().filter(|named| **named).count();
        if named == 0 {
            return Err(Rejection::unknown_desctype(
                "create chain names no Sedici context",
            ));
        }
        if named > 1 {
            // One create makes one context. Answering OK as a wrapper alone would drop the
            // generation context the caller asked for without telling it (K102).
            return Err(self.failure(
                "create chain names both a swapchain wrap and frame generation",
                ReturnCode::Error,
            ));
        }

        // Validate the form-specific inputs before reserving anything.
        if let Some(wrap) = desc.swapchain_wrap {
            if wrap.swapchain == 0 || wrap.swapchain_slot == 0 || wrap.game_queue == 0 {
                return Err(self.failure(
                    "swapchain wrap without a swapchain or queue",
                    ReturnCode::Error,
                ));
            }
        }
        if let Some(created) = desc.swapchain_new {
            if created.swapchain_slot == 0
                || created.desc.is_none()
                || created.dxgi_factory == 0
                || created.game_queue == 0
            {
                return Err(self.failure(
                    "swapchain new without a factory, desc, output or queue",
                    ReturnCode::Error,
                ));
            }
        }
        if let Some(for_hwnd) = desc.swapchain_for_hwnd {
            if for_hwnd.swapchain_slot == 0
                || for_hwnd.desc.is_none()
                || for_hwnd.hwnd == 0
                || for_hwnd.dxgi_factory == 0
                || for_hwnd.game_queue == 0
            {
                return Err(self.failure(
                    "swapchain for hwnd without a factory, desc, window, output or queue",
                    ReturnCode::Error,
                ));
            }
        }
        let swapchain_form = !forms[0];

        // Resolve the backend device before reserving the token, so a missing device is refused
        // without consuming the namespace.
        let device = if let Some(frame_generation) = desc.frame_generation {
            let _ = frame_generation;
            let Some(backend) = desc.backend_dx12 else {
                return Err(self.failure("create without a DX12 device", ReturnCode::Error));
            };
            match self.devices.resolve(backend.device) {
                Some(binding) => Some(binding),
                None => return Err(self.failure("create without a DX12 device", ReturnCode::Error)),
            }
        } else {
            None
        };

        // Mint the identity before any backend call that can consume game resources.
        let Some(token) = self.tokens.issue() else {
            return Err(self.failure(
                "Sedici handle namespace unavailable or exhausted",
                ReturnCode::Error,
            ));
        };

        // Allocate the owner before a backend call can consume game resources.
        if self.faults.consume(BookkeepingFault::OwnerAllocation) {
            return Err(self.allocation("owner allocation failed"));
        }
        let kind = if swapchain_form {
            OwnerKind::Wrap
        } else {
            OwnerKind::Session
        };
        let owner = Arc::new(Owned::new(kind));

        if self
            .faults
            .consume(BookkeepingFault::RouteRegistryAllocation)
        {
            return Err(self.allocation("route registry allocation failed"));
        }
        // Publish a placeholder: a concurrent destroy of this token sees the node but no session,
        // so it refuses rather than racing an uninitialized owner.
        self.publish(token, Arc::clone(&owner));

        if swapchain_form {
            self.create_swapchain(token, desc, owner)
        } else {
            self.create_session(
                token,
                desc,
                owner,
                device.expect("generation create resolved a device"),
            )
        }
    }

    fn create_swapchain(
        &self,
        token: OpaqueHandle,
        desc: &CreateContextDesc,
        owner: Arc<Owned<D, G>>,
    ) -> Result<model::CreatedContext, Rejection> {
        let origin = if desc.swapchain_wrap.is_some() {
            crate::application::session::WrapOrigin::Wrapped
        } else {
            crate::application::session::WrapOrigin::Created
        };
        // Injected backend allocation failure: rejected before the native chain is touched.
        if self.faults.consume(BookkeepingFault::BackendCreation) {
            self.rollback(token);
            return Err(self.failure(
                "swapchain wrap backend allocation failed",
                ReturnCode::Error,
            ));
        }
        let (swapchain, wrapper) = match self.swapchains.create_wrapper(desc, origin, token) {
            Ok(created) => created,
            Err(rejection) => {
                self.rollback(token);
                return Err(rejection);
            }
        };
        {
            let mut body = owner.lock();
            body.swapchain = swapchain;
            body.wrap = Some(wrapper);
        }
        self.faults.run_creation_hook();
        tracing::info!(
            "fg: created swapchain {}",
            if desc.swapchain_wrap.is_some() {
                "wrapper"
            } else {
                "(new)"
            }
        );
        Ok(model::CreatedContext {
            context: token,
            swapchain: Some(swapchain),
        })
    }

    fn create_session(
        &self,
        token: OpaqueHandle,
        desc: &CreateContextDesc,
        owner: Arc<Owned<D, G>>,
        device: DeviceBinding<D>,
    ) -> Result<model::CreatedContext, Rejection> {
        // Injected backend allocation failure before creation (K63).
        if self.faults.consume(BookkeepingFault::BackendCreation) {
            self.rollback(token);
            return Err(self.failure(
                "frame generation backend allocation failed",
                ReturnCode::Error,
            ));
        }
        let mut session = Session::new(
            Context::new(Arc::clone(&self.objects), Arc::clone(&self.faults)),
            Arc::clone(&self.faults),
            token,
        );
        if !session.create(desc, device) {
            let reason = session.last_error().to_owned();
            self.rollback(token);
            return Err(self.failure(reason, ReturnCode::Error));
        }
        owner.lock().session = Some(session);
        tracing::info!("fg: created frame generation context");
        Ok(model::CreatedContext {
            context: token,
            swapchain: None,
        })
    }

    // -- Configure -----------------------------------------------------------------------------

    pub fn configure(&self, token: ContextToken, desc: &ConfigureDesc) -> Result<(), Rejection> {
        let _scope = CallScope::enter("configure", token);
        self.reap_parked();
        let Some(owner) = self.find_owned(token) else {
            return Err(if self.tokens.contains_issued(token) {
                self.failure("configure on a destroyed context", ReturnCode::Error)
            } else {
                self.failure("configure on a foreign handle", ReturnCode::ErrorParameter)
            });
        };
        if desc.null_descriptor {
            return Err(Rejection::with_code(
                ReturnCode::Error,
                ErrorCode::NullDescriptor,
                "null descriptor on a Sedici-owned context",
            ));
        }
        let mut body = owner.lock();
        if body.dead || (body.session.is_none() && owner.kind == OwnerKind::Session) {
            return Err(self.failure("configure on a destroyed context", ReturnCode::Error));
        }
        if owner.kind == OwnerKind::Wrap {
            // Sedici owns output pacing, so a request to tune AMD's swapchain pacing has no meaning
            // here. Accept it so the game's own call sequence is unchanged, and log it once.
            tracing::debug!("fg: swapchain configure accepted (Sedici owns pacing)");
            return Ok(());
        }
        // Resolve the swapchain this configure names, by identity, so a recycled pointer cannot
        // bind a context to a dead wrapper (K68). A session that already owns a different wrapper
        // rebinds to the named one.
        let named = desc
            .frame_generation
            .as_ref()
            .map(|d| d.swapchain)
            .or_else(|| desc.legacy_frame_generation.as_ref().map(|d| d.swapchain))
            .unwrap_or(0);
        if named != 0 {
            if let Some((_, wrapper)) = self.find_wrapper(named) {
                if let Some(session) = body.session.as_mut() {
                    session.bind_wrapper(wrapper);
                }
            }
        }
        let Some(session) = body.session.as_mut() else {
            return Err(self.failure("configure on a destroyed context", ReturnCode::Error));
        };
        let sink = session.sink();
        if !session.configure(desc, sink) {
            return Err(self.failure(session.last_error().to_owned(), ReturnCode::Error));
        }
        let enabled = desc
            .frame_generation
            .as_ref()
            .map(|d| d.frame_generation_enabled)
            .or_else(|| {
                desc.legacy_frame_generation
                    .as_ref()
                    .map(|d| d.frame_generation_enabled)
            })
            .unwrap_or(false);
        owner.generation_on.store(enabled, Ordering::Relaxed);
        Ok(())
    }

    /// Finds a live wrap owner by the chain handle the game holds. Lookup is by identity, not by
    /// the game's pointer value alone: the handle is the wrapper object, which stays stable while
    /// the owner lives.
    fn find_wrapper(&self, swapchain: OpaqueHandle) -> Option<(OpaqueHandle, Arc<dyn Wrapper>)> {
        if swapchain == 0 {
            return None;
        }
        let registry = self
            .owned
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        for (token, owner) in registry.iter() {
            if owner.kind != OwnerKind::Wrap {
                continue;
            }
            let body = owner.lock();
            if body.dead {
                continue;
            }
            if body.swapchain == swapchain {
                if let Some(wrapper) = body.wrap.clone() {
                    return Some((*token, wrapper));
                }
            }
        }
        None
    }

    // -- Dispatch ------------------------------------------------------------------------------

    pub fn dispatch(
        &self,
        token: ContextToken,
        desc: &DispatchDesc,
    ) -> Result<DispatchReply, Rejection> {
        let _scope = CallScope::enter("dispatch", token);
        self.reap_parked();
        let Some(owner) = self.find_owned(token) else {
            return Err(if self.tokens.contains_issued(token) {
                self.failure("dispatch on a destroyed context", ReturnCode::Error)
            } else {
                self.failure("dispatch on a foreign handle", ReturnCode::ErrorParameter)
            });
        };
        if desc.null_descriptor {
            return Err(Rejection::with_code(
                ReturnCode::Error,
                ErrorCode::NullDescriptor,
                "null descriptor on a Sedici-owned context",
            ));
        }
        let mut body = owner.lock();
        if body.dead || body.session.is_none() && owner.kind == OwnerKind::Session {
            return Err(self.failure("dispatch on a destroyed context", ReturnCode::Error));
        }
        if owner.kind == OwnerKind::Wrap {
            return Err(self.failure("dispatch on the swapchain wrapper", ReturnCode::Error));
        }
        if desc.root_type == Some(DISPATCH_FRAMEGENERATION) {
            // The game's own generation dispatch into the interpolation list it got from the
            // swapchain query (K131). Sedici generates the frame itself; acknowledge it.
            tracing::debug!(
                "fg: game frame generation dispatch acknowledged (Sedici generates its own)"
            );
            return Ok(DispatchReply::FrameGenerationAcknowledged);
        }
        if desc.root_type != Some(DISPATCH_FRAMEGENERATION_PREPARE)
            && desc.root_type != Some(DISPATCH_FRAMEGENERATION_PREPARE_V2)
        {
            return Err(self.failure(
                "unsupported frame generation dispatch type",
                ReturnCode::Error,
            ));
        }
        let Some(session) = body.session.as_mut() else {
            return Err(self.failure("dispatch on a destroyed context", ReturnCode::Error));
        };
        if !session.prepare(desc) {
            return Err(self.failure(session.last_error().to_owned(), ReturnCode::Error));
        }
        Ok(DispatchReply::Prepared)
    }

    // -- Query ---------------------------------------------------------------------------------

    pub fn query(
        &self,
        token: Option<ContextToken>,
        desc: &QueryDesc,
    ) -> Result<QueryReply, Rejection> {
        let _scope = CallScope::enter("query", token.unwrap_or(0));
        self.reap_parked();
        if let Some(get_versions) = desc.get_versions {
            return self.answer_versions(&get_versions);
        }
        let Some(token) = token.filter(|token| *token != 0) else {
            return Err(self.failure("query on a foreign handle", ReturnCode::ErrorParameter));
        };
        let Some(owner) = self.find_owned(token) else {
            return Err(if self.tokens.contains_issued(token) {
                self.failure("query on a destroyed context", ReturnCode::Error)
            } else {
                self.failure("query on a foreign handle", ReturnCode::ErrorParameter)
            });
        };
        if desc.null_descriptor {
            return Err(Rejection::with_code(
                ReturnCode::Error,
                ErrorCode::NullDescriptor,
                "null descriptor on a Sedici-owned context",
            ));
        }
        let body = owner.lock();
        if body.dead {
            return Err(self.failure("query on a destroyed context", ReturnCode::Error));
        }
        // Alan Wake 2 asks the swapchain for an interpolation command list and texture (K131).
        // Sedici generates the frame itself, so these are the wrapper's scratch objects and
        // whatever the game records into them is discarded.
        if owner.kind == OwnerKind::Wrap {
            if let Some(query) = desc.interpolation_command_list {
                let Some(wrapper) = body.wrap.as_ref() else {
                    return Err(
                        self.failure("interpolation command list unavailable", ReturnCode::Error)
                    );
                };
                return match wrapper.interpolation_list() {
                    Some(list) if query.output_slot != 0 => {
                        tracing::debug!(
                            "fg: interpolation command list query answered with a scratch list"
                        );
                        Ok(QueryReply::CommandList(list))
                    }
                    _ => {
                        Err(self
                            .failure("interpolation command list unavailable", ReturnCode::Error))
                    }
                };
            }
            if let Some(query) = desc.interpolation_texture {
                let Some(wrapper) = body.wrap.as_ref() else {
                    return Err(
                        self.failure("interpolation texture unavailable", ReturnCode::Error)
                    );
                };
                return match wrapper.interpolation_texture() {
                    Some(texture) if query.output_slot != 0 => {
                        tracing::debug!(
                            "fg: interpolation texture query answered with a scratch texture"
                        );
                        Ok(QueryReply::Texture(texture))
                    }
                    _ => Err(self.failure("interpolation texture unavailable", ReturnCode::Error)),
                };
            }
        }
        // Other queries stay refused and logged so a game session can name them.
        Err(self.failure(
            "unimplemented query on a Sedici-owned context",
            ReturnCode::Error,
        ))
    }

    fn answer_versions(&self, query: &GetVersions) -> Result<QueryReply, Rejection> {
        let Some(version) = provider_for(query.create_desc_type) else {
            return Err(Rejection::with_code(
                ReturnCode::ErrorUnknownDesctype,
                ErrorCode::Rejected,
                "GetVersions for an unknown effect",
            ));
        };
        if query.output_count_slot == 0 {
            return Err(Rejection::with_code(
                ReturnCode::ErrorParameter,
                ErrorCode::Rejected,
                "GetVersions without an output count",
            ));
        }
        // The name is a NUL-terminated literal so the adapter can hand the game a stable pointer.
        Ok(QueryReply::Versions {
            count: 1,
            id: Some(version.id),
            name: Some(version.name),
        })
    }

    // -- Destroy -------------------------------------------------------------------------------

    pub fn destroy(&self, token: ContextToken) -> Result<(), Rejection> {
        let _scope = CallScope::enter("destroy", token);
        // Unregister first so no new call can find the owner, then wait for calls already running
        // on it, then tear it down. The registry lock is not held through retirement waits.
        let owner = {
            let mut registry = self
                .owned
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            match registry.remove(&token) {
                Some(owner) => owner,
                None => {
                    return Err(if self.tokens.contains_issued(token) {
                        self.failure("destroy on a destroyed context", ReturnCode::Error)
                    } else {
                        self.failure("destroy on a foreign handle", ReturnCode::ErrorParameter)
                    });
                }
            }
        };

        let mut body = owner.lock();
        body.dead = true;
        let ok = if owner.kind == OwnerKind::Session {
            body.session
                .as_mut()
                .is_none_or(|session| session.destroy())
        } else {
            if let Some(wrapper) = body.wrap.as_ref() {
                wrapper.close_generator();
            }
            true
        };
        drop(body);

        if !ok && owner.kind == OwnerKind::Session {
            // Keep the owner alive so a later router call can release it once its GPU work retires
            // (K41). The token was already unregistered, so the game's handle is stale at once.
            owner.parked.store(true, Ordering::Release);
            self.parked
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner())
                .push(Arc::clone(&owner));
            self.parked_count.fetch_add(1, Ordering::Release);
            tracing::info!("fg: retaining a frame generation context until GPU work retires");
        }
        drop(owner);
        self.reap_parked();
        if ok {
            Ok(())
        } else {
            // The caller's handle is left exactly as it was: a game that inspects its own context
            // afterwards must see the same value. The issued token stays recognisable as stale.
            Err(self.failure("destroy before GPU retirement", ReturnCode::Error))
        }
    }

    /// Releases retained owners whose GPU work has retired. The common case is one atomic load.
    pub fn reap_parked(&self) {
        if self.parked_count.load(Ordering::Acquire) == 0 {
            return;
        }
        loop {
            let mut parked = self
                .parked
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            let index = parked.iter().position(|owner| {
                let mut body = owner.lock();
                let retired = match body.session.as_ref() {
                    Some(session) => session.retired(),
                    None => true,
                };
                if retired {
                    if let Some(session) = body.session.as_mut() {
                        let _ = session.destroy();
                    }
                    body.dead = true;
                }
                retired
            });
            let Some(index) = index else { return };
            parked.swap_remove(index);
            drop(parked);
            self.parked_count.fetch_sub(1, Ordering::Release);
            tracing::info!("fg: released a retained frame generation context");
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Address reservation
// ---------------------------------------------------------------------------------------------

/// Reserves a process-lifetime opaque address range.
///
/// The C++ used `VirtualAlloc(MEM_RESERVE, PAGE_NOACCESS)`, a call the adapter layer owns. The
/// composition root installs [`AddressReservation`] here; the application never names a `windows`
/// type. When no reserver is installed (or it fails), the router issues no token, which is the
/// owner-approved refusal (K29).
pub trait AddressReservation: Send + Sync {
    /// Reserve `bytes` of address space and return the base; `None` on failure. The range is never
    /// committed or dereferenced, and is released only at process exit.
    fn reserve(&self, bytes: usize) -> Option<usize>;
}

/// A reserver that never succeeds; the default until the composition root installs the adapter.
pub struct NoAddressReservation;

impl AddressReservation for NoAddressReservation {
    fn reserve(&self, _bytes: usize) -> Option<usize> {
        None
    }
}

// ---------------------------------------------------------------------------------------------
// The FfxService implementation
// ---------------------------------------------------------------------------------------------

impl<D, G> model::FfxService for Router<D, G>
where
    D: GpuDevice + Send + 'static,
    G: FrameGenerator<D> + Send + 'static,
{
    fn create(&self, desc: &CreateContextDesc) -> Result<model::CreatedContext, Rejection> {
        Router::create(self, desc)
    }

    fn destroy(&self, context: ContextToken) -> Result<(), Rejection> {
        Router::destroy(self, context)
    }

    fn configure(&self, context: ContextToken, desc: &ConfigureDesc) -> Result<(), Rejection> {
        Router::configure(self, context, desc)
    }

    fn query(
        &self,
        context: Option<ContextToken>,
        desc: &QueryDesc,
    ) -> Result<QueryReply, Rejection> {
        Router::query(self, context, desc)
    }

    fn dispatch(
        &self,
        context: ContextToken,
        desc: &DispatchDesc,
    ) -> Result<DispatchReply, Rejection> {
        Router::dispatch(self, context, desc)
    }

    fn any_generation_enabled(&self) -> bool {
        Router::any_generation_enabled(self)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_provider_table_matches_the_amd_binary() {
        let expected = [
            (CREATE_FRAMEGENERATION, 0xf600_0000_00c0_1006, "3.1.6"),
            (CREATE_SWAPCHAIN_WRAP_DX12, 0xf65c_dd12_00c0_1007, "3.1.7"),
            (CREATE_SWAPCHAIN_NEW_DX12, 0xf65c_dd12_00c0_1007, "3.1.7"),
            (
                CREATE_SWAPCHAIN_FOR_HWND_DX12,
                0xf65c_dd12_00c0_1007,
                "3.1.7",
            ),
        ];
        assert_eq!(PROVIDER_VERSIONS.len(), expected.len());
        for (version, (create_desc_type, id, name)) in PROVIDER_VERSIONS.iter().zip(expected) {
            assert_eq!(version.create_desc_type, create_desc_type);
            assert_eq!(version.id, id);
            assert_eq!(version.name.to_str().unwrap(), name);
            assert_eq!(provider_for(create_desc_type), Some(version));
        }
        assert_eq!(provider_for(0x7fff_ffff), None);
    }

    #[test]
    fn the_token_namespace_never_reuses_a_value() {
        let faults = Arc::new(Faults::new());
        let tokens = MemoryTokens::new(Arc::clone(&faults), 0x1000, 0x100, 0x10);
        let first = tokens.issue().expect("first token");
        assert!(tokens.contains_issued(first));
        let second = tokens.issue().expect("second token");
        assert_ne!(first, second);
        assert!(!tokens.contains_issued(first + 1));
    }

    #[test]
    fn an_armed_reservation_fault_issues_no_token_then_a_retry_succeeds() {
        let faults = Arc::new(Faults::new());
        let tokens = MemoryTokens::new(Arc::clone(&faults), 0x1000, 0x100, 0x10);
        faults.arm(BookkeepingFault::TokenReservation);
        assert_eq!(tokens.issue(), None);
        let token = tokens.issue().expect("retry succeeds");
        assert!(tokens.contains_issued(token));
    }
}
