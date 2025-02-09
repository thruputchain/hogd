#[cfg(target_arch = "wasm32")]
#[inline(always)]
pub fn trap() -> ! {
    core::arch::wasm32::unreachable()
}

#[cfg(all(target_arch = "wasm32", not(feature = "mock")))]
#[panic_handler]
fn handle_panic(_: &core::panic::PanicInfo) -> ! {
    trap();
}

#[cfg(all(not(target_arch = "wasm32"), not(feature = "mock")))]
#[panic_handler]
fn handle_panic(_: &core::panic::PanicInfo) -> ! {
    // empty panic handler that halts the program.
    // This should probably never be used -- either this library will be built in std
    // for testing or built targeting wasm32

    loop {}
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
