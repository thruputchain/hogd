#[cfg(target_arch = "wasm32")]
#[inline(always)]
pub fn trap() -> ! {
    core::arch::wasm32::unreachable()
}

#[cfg(not(target_arch = "wasm32"))]
#[inline(always)]
pub fn trap() -> ! {
    unreachable!();
}

#[cfg(target_arch = "wasm32")]
#[panic_handler]
fn handle_panic(_: &core::panic::PanicInfo) -> ! {
    trap();
}

#[inline(always)]
pub fn unwrap_abort_option<T>(o: Option<T>) -> T {
    match o {
        Some(t) => t,
        None => {
            trap();
        }
    }
}

#[inline(always)]
pub fn unwrap_abort<T, E>(o: Result<T, E>) -> T {
    match o {
        Ok(t) => t,
        _ => {
            trap();
        }
    }
}
