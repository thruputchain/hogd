#[cfg(target_arch = "wasm32")]
#[inline(always)]
pub fn trap() -> ! {
    core::arch::wasm32::unreachable()
}

#[cfg(all(not(target_arch = "wasm32"), not(feature = "mock")))]
#[inline(always)]
pub fn trap() -> ! {
    panic!("unreachable");
}

#[cfg(not(feature = "mock"))]
#[panic_handler]
fn handle_panic(_: &core::panic::PanicInfo) -> ! {
    trap();
}

#[cfg(feature = "mock")]
mod test_rt {
    use std::boxed::Box;
    use std::panic::panic_any;

    use test_ffi::invoke_error::deterministic_error;

    pub fn trap() -> ! {
        panic_any(Box::new(deterministic_error()))
    }
}
#[cfg(feature = "mock")]
pub use test_rt::trap;
