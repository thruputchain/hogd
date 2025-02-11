#![cfg_attr(not(feature = "activate"), no_std)]
// suppress spurious warnings from bindgen
#![allow(non_camel_case_types)]
#![allow(non_upper_case_globals)]
#![allow(non_snake_case)]

#[cfg(feature = "activate")]
pub mod deployer;
#[cfg(feature = "activate")]
pub mod host_fn_error;
#[cfg(feature = "activate")]
pub mod invoke_error;
#[cfg(feature = "activate")]
pub mod types;
#[cfg(feature = "activate")]
pub use host_fn_error::perform_mocked_syscall;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_my_function() {
        let _ = types::Environment::new();
    }
}
