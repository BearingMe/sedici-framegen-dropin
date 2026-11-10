//! The five exported FFX functions, ported from `dllmain.cpp`'s exports.
//!
//! The exports are the only place a game pointer is accepted or produced. Each one:
//!
//! 1. installs a panic guard (`catch_unwind`) so no panic can cross the C ABI;
//! 2. converts the raw chain into the safe model in [`crate::application::ffx_model`];
//! 3. calls the installed [`model::FfxService`] — until the composition root installs one, every
//!    call answers with the documented error code, exactly as the C++ router answers a call it
//!    does not recognise;
//! 4. maps a [`model::Rejection`] to its FFX return code and logs the call through
//!    [`super::desc_walk::describe_call`].
//!
//! `DllMain` is not reproduced here: a Rust `cdylib` links the CRT's own entry point, so the port
//! has no `DllMain` (K148); the log writer is stopped by its guard on unload. Each export calls
//! [`crate::entry::ensure_started`] first, which runs the composition root on the first FFX call
//! when this code is loaded as a DLL (never in a test binary that links the rlib).
//!
//! # Return codes
//!
//! The router chooses the code ([`model::Rejection::ffx`]); the ABI edge only supplies the
//! documented defaults for the cases the C++ exports themselves decide:
//!
//! | Call | Code |
//! |---|---|
//! | null handle or a handle Sedici never issued | `FFX_API_RETURN_ERROR_PARAMETER` |
//! | create whose chain is not frame generation or a swapchain form | `FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE` |
use std::panic::{catch_unwind, AssertUnwindSafe};

use crate::application::ffx_model::{self as model, ReturnCode};
use crate::entry;

use super::desc_walk;
use super::types::{ffxAllocationCallbacks, ffxApiHeader, ffxContext, ffxReturnCode_t};

/// The documented answer for a call on a null handle or a handle Sedici never issued (README).
const FOREIGN_HANDLE: ReturnCode = ReturnCode::ErrorParameter;
/// The documented answer for a create chain Sedici does not recognise (README).
const UNKNOWN_DESC: ReturnCode = ReturnCode::ErrorUnknownDesctype;

/// Runs `body` so that a panic becomes `FFX_API_RETURN_ERROR` and never unwinds across the ABI.
fn guarded(body: impl FnOnce() -> ReturnCode) -> ffxReturnCode_t {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(code) => code.as_u32(),
        Err(_) => {
            // The panic hook has already printed its message to stderr; the game gets the
            // documented generic error and keeps running.
            tracing::error!("ffx export panicked; answering FFX_API_RETURN_ERROR");
            ReturnCode::Error.as_u32()
        }
    }
}

fn log_rejection(operation: &str, context: usize, rejection: &model::Rejection) {
    // The log line the README documents: `rejected: <reason> [code=E_...] [op=<call> ctx=<handle>]`.
    tracing::info!(
        "rejected: {} [code={}] [op={} ctx={}]",
        rejection.reason,
        rejection.code.name(),
        operation,
        if context == 0 {
            "null".to_owned()
        } else {
            format!("0x{context:x}")
        }
    );
}

fn context_value(context: *const ffxContext) -> Option<usize> {
    if context.is_null() {
        return None;
    }
    // SAFETY: the FFX ABI passes a live `ffxContext*`; reading the slot once on entry is the
    // same read the C++ `*context` does, and a null handle reads as zero.
    Some(unsafe { std::ptr::read_unaligned(context) } as usize)
}

/// `ffxCreateContext`.
///
/// Until a [`model::FfxService`] is installed, a chain Sedici could own (frame generation or a
/// swapchain form) answers `FFX_API_RETURN_ERROR` (nothing can be created yet) and any other chain
/// answers the documented `FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE`.
///
/// # Safety
/// The C ABI contract: `context` is a writable `ffxContext*`; `desc` is a `pNext` chain of FFX
/// descriptor structs whose pointers stay live until the matching `ffxDestroyContext`. Every
/// read and write through either pointer is validated first by `desc_walk`.
#[no_mangle]
pub unsafe extern "system" fn ffxCreateContext(
    context: *mut ffxContext,
    desc: *mut ffxApiHeader,
    callbacks: *const ffxAllocationCallbacks,
) -> ffxReturnCode_t {
    let _ = callbacks; // Sedici never allocates through the game's callbacks, as the C++ did not.
    guarded(|| {
        entry::ensure_started();
        let context_value = context_value(context as *const ffxContext);
        let desc = desc as *const ffxApiHeader;
        let flattened = desc_walk::convert_create(desc);

        // A null handle or a null chain is a parameter error, as the C++ exports answer.
        let result = match (context_value, desc.is_null()) {
            (None, _) | (_, true) => ReturnCode::ErrorParameter,
            (Some(_), false) => match model::service() {
                None => {
                    // Until the router is installed Sedici recognises no chain.
                    if flattened.names_a_sedici_context() {
                        ReturnCode::Error
                    } else {
                        UNKNOWN_DESC
                    }
                }
                Some(service) if flattened.names_a_sedici_context() => match service
                    .create(&flattened)
                {
                    Ok(created) => {
                        if desc_walk::apply_created(context, desc as *mut ffxApiHeader, &created) {
                            ReturnCode::Ok
                        } else {
                            // The chain named no writable output slot; the router's context is
                            // dropped rather than leaked into a game call that cannot use it.
                            tracing::info!(
                                "rejected: create could not write the game's output slot"
                            );
                            ReturnCode::Error
                        }
                    }
                    Err(rejection) => {
                        log_rejection("create", context_value.unwrap_or(0), &rejection);
                        rejection.ffx
                    }
                },
                Some(_) => UNKNOWN_DESC,
            },
        };

        let handle = context_value.unwrap_or(0);
        desc_walk::describe_call(desc_walk::ExportKind::Create, desc, result, handle);
        result
    })
}

/// `ffxDestroyContext`.
///
/// # Safety
/// The C ABI contract: `context` is a live `ffxContext*` that Sedici issued.
#[no_mangle]
pub unsafe extern "system" fn ffxDestroyContext(
    context: *mut ffxContext,
    callbacks: *const ffxAllocationCallbacks,
) -> ffxReturnCode_t {
    let _ = callbacks;
    guarded(|| {
        entry::ensure_started();
        let handle = context_value(context as *const ffxContext);
        let result = match handle {
            None => FOREIGN_HANDLE,
            Some(0) => FOREIGN_HANDLE,
            Some(token) => match model::service() {
                None => ReturnCode::Error,
                Some(service) => service.destroy(token).map_or_else(
                    |rejection| {
                        log_rejection("destroy", token, &rejection);
                        rejection.ffx
                    },
                    |()| ReturnCode::Ok,
                ),
            },
        };
        desc_walk::describe_call(
            desc_walk::ExportKind::Destroy,
            std::ptr::null(),
            result,
            handle.unwrap_or(0),
        );
        result
    })
}

/// `ffxConfigure`.
///
/// # Safety
/// The C ABI contract: `context` is a live `ffxContext*`; `desc` is a readable configure chain.
#[no_mangle]
pub unsafe extern "system" fn ffxConfigure(
    context: *mut ffxContext,
    desc: *const ffxApiHeader,
) -> ffxReturnCode_t {
    guarded(|| {
        entry::ensure_started();
        let handle = context_value(context as *const ffxContext);
        let flattened = desc_walk::convert_configure(desc);
        let result = match handle {
            None => FOREIGN_HANDLE,
            Some(0) => FOREIGN_HANDLE,
            Some(token) => match model::service() {
                None => ReturnCode::Error,
                Some(service) => service.configure(token, &flattened).map_or_else(
                    |rejection| {
                        log_rejection("configure", token, &rejection);
                        rejection.ffx
                    },
                    |()| ReturnCode::Ok,
                ),
            },
        };
        desc_walk::describe_call(
            desc_walk::ExportKind::Configure,
            desc,
            result,
            handle.unwrap_or(0),
        );
        result
    })
}

/// `ffxQuery`.
///
/// A `GetVersions` query is answered without a context (the game asks before it creates one);
/// every other query needs the handle Sedici issued.
///
/// # Safety
/// The C ABI contract: `context` is a live `ffxContext*` or null; `desc` is a readable query chain
/// whose output slots are writable.
#[no_mangle]
pub unsafe extern "system" fn ffxQuery(
    context: *mut ffxContext,
    desc: *mut ffxApiHeader,
) -> ffxReturnCode_t {
    guarded(|| {
        entry::ensure_started();
        let handle = context_value(context as *const ffxContext);
        let flattened = desc_walk::convert_query(desc);
        let root = desc as *const ffxApiHeader;

        // GetVersions needs no context and a null descriptor is a parameter error otherwise.
        let result = if !desc.is_null() && flattened.get_versions.is_some() {
            match model::service() {
                None => UNKNOWN_DESC,
                Some(service) => match service.query(None, &flattened) {
                    Ok(reply) => {
                        desc_walk::apply_query_reply(desc, &reply);
                        ReturnCode::Ok
                    }
                    Err(rejection) => {
                        log_rejection("query", 0, &rejection);
                        rejection.ffx
                    }
                },
            }
        } else {
            match handle {
                None => FOREIGN_HANDLE,
                Some(0) => FOREIGN_HANDLE,
                Some(token) => match model::service() {
                    None => ReturnCode::Error,
                    Some(service) => match service.query(Some(token), &flattened) {
                        Ok(reply) => {
                            desc_walk::apply_query_reply(desc, &reply);
                            ReturnCode::Ok
                        }
                        Err(rejection) => {
                            log_rejection("query", token, &rejection);
                            rejection.ffx
                        }
                    },
                },
            }
        };

        desc_walk::describe_call(
            desc_walk::ExportKind::Query,
            root,
            result,
            handle.unwrap_or(0),
        );
        result
    })
}

/// `ffxDispatch`.
///
/// # Safety
/// The C ABI contract: `context` is a live `ffxContext*` that Sedici issued; `desc` is a readable
/// dispatch chain.
#[no_mangle]
pub unsafe extern "system" fn ffxDispatch(
    context: *mut ffxContext,
    desc: *const ffxApiHeader,
) -> ffxReturnCode_t {
    guarded(|| {
        entry::ensure_started();
        let handle = context_value(context as *const ffxContext);
        let flattened = desc_walk::convert_dispatch(desc);
        let result = match handle {
            None => FOREIGN_HANDLE,
            Some(0) => FOREIGN_HANDLE,
            Some(token) => match model::service() {
                None => ReturnCode::Error,
                Some(service) => service.dispatch(token, &flattened).map_or_else(
                    |rejection| {
                        log_rejection("dispatch", token, &rejection);
                        rejection.ffx
                    },
                    |_reply| ReturnCode::Ok,
                ),
            },
        };
        desc_walk::describe_call(
            desc_walk::ExportKind::Dispatch,
            desc,
            result,
            handle.unwrap_or(0),
        );
        result
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_panic_becomes_the_documented_error() {
        let code = guarded(|| panic!("boom"));
        assert_eq!(code, ReturnCode::Error.as_u32());
    }

    #[test]
    fn a_null_context_is_a_parameter_error_on_every_export() {
        // SAFETY: every export is called with null pointers only, which each export checks before
        // touching game memory.
        unsafe {
            assert_eq!(
                ffxCreateContext(std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null()),
                6
            );
            assert_eq!(ffxDestroyContext(std::ptr::null_mut(), std::ptr::null()), 6);
            assert_eq!(ffxConfigure(std::ptr::null_mut(), std::ptr::null()), 6);
            assert_eq!(ffxQuery(std::ptr::null_mut(), std::ptr::null_mut()), 6);
            assert_eq!(ffxDispatch(std::ptr::null_mut(), std::ptr::null()), 6);
        }
    }

    #[test]
    fn a_null_chain_with_a_handle_is_a_parameter_error_on_create() {
        let mut handle: ffxContext = std::ptr::null_mut();
        // SAFETY: `handle` is owned and writable; the chain is null, which the export rejects.
        let code = unsafe { ffxCreateContext(&mut handle, std::ptr::null_mut(), std::ptr::null()) };
        assert_eq!(code, ReturnCode::ErrorParameter.as_u32());
        assert!(handle.is_null());
    }
}
