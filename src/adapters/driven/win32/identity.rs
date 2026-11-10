//! Adapter and window identity: the same-adapter rule and the overlay's window (K141).
//!
//! Two facts the wrapper needs about the game's objects, read without exposing a COM pointer:
//!
//! * the LUID of the adapter a D3D12 device and a swapchain report, so [`same_adapter`] can apply
//!   K130 (Alan Wake 2 reports a different `ID3D12Device` object for the chain than for its queue;
//!   the same adapter is enough, a different one is refused);
//! * the `HWND` behind a chain, an `OutputWindow` or a create desc's `hwnd`, which the overlay takes
//!   for its hotkey and cursor handling (`capture_wrapped_window` in `dllmain.cpp`).
use windows::core::Interface;
use windows::Win32::Foundation::{HWND, LUID};
use windows::Win32::Graphics::Direct3D12::{ID3D12CommandQueue, ID3D12Device};
use windows::Win32::Graphics::Dxgi::IDXGISwapChain4;

/// The LUID of the adapter a D3D12 device reports, or `None` when the query fails.
pub fn adapter_luid_device(device: &ID3D12Device) -> Option<LUID> {
    // SAFETY: a pure query on a live device; the LUID is returned by value.
    Some(unsafe { device.GetAdapterLuid() })
}

/// The LUID of the adapter a queue was created on.
pub fn adapter_luid_queue(queue: &ID3D12CommandQueue) -> Option<LUID> {
    // SAFETY: a device query on a live queue; no state changes.
    let mut device: Option<ID3D12Device> = None;
    unsafe { queue.GetDevice(&mut device) }.ok()?;
    device.as_ref().and_then(adapter_luid_device)
}

/// The LUID of the adapter a chain was created on.
pub fn adapter_luid_chain(chain: &IDXGISwapChain4) -> Option<LUID> {
    // SAFETY: a device query on a live chain; no state changes.
    let device: Result<ID3D12Device, windows::core::Error> = unsafe { chain.GetDevice() };
    device.ok().as_ref().and_then(adapter_luid_device)
}

/// The same-adapter rule (K130): a different device object on the same adapter is accepted, a
/// different adapter is not. `None` on either side cannot be compared, which the C++ treated as
/// the same object (the comparison only ran when the pointers differed).
pub fn same_adapter(chain: Option<LUID>, queue: Option<LUID>) -> bool {
    match (chain, queue) {
        (Some(chain), Some(queue)) => {
            chain.LowPart == queue.LowPart && chain.HighPart == queue.HighPart
        }
        _ => true,
    }
}

/// The window a chain presents into (`IDXGISwapChain1::GetHwnd`), or `None` when it has none.
///
/// A chain created with `CreateSwapChain` (the New form's legacy desc) refuses `GetHwnd`; the
/// overlay then stays on whatever window an earlier create captured, as the C++ did.
pub fn chain_window(chain: &IDXGISwapChain4) -> Option<HWND> {
    // SAFETY: a pure query on a live chain; a chain without a window answers with an error.
    unsafe { chain.GetHwnd() }
        .ok()
        .filter(|window| !window.0.is_null())
}

/// The address of a live COM object, as an opaque value (never dereferenced).
pub fn object_address<T: Interface>(object: &T) -> usize {
    Interface::as_raw(object) as usize
}

#[cfg(test)]
mod tests {
    use super::*;

    fn luid(low: u32, high: i32) -> LUID {
        LUID {
            LowPart: low,
            HighPart: high,
        }
    }

    #[test]
    fn the_same_adapter_rule_is_luid_equality() {
        assert!(same_adapter(Some(luid(1, 2)), Some(luid(1, 2))));
        assert!(!same_adapter(Some(luid(1, 2)), Some(luid(1, 3))));
        assert!(!same_adapter(Some(luid(2, 2)), Some(luid(1, 2))));
        // An unreadable side cannot be compared; the C++ only refused on a readable mismatch.
        assert!(same_adapter(None, Some(luid(1, 2))));
        assert!(same_adapter(Some(luid(1, 2)), None));
    }
}
