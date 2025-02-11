// suppress spurious warnings from bindgen
#![allow(non_camel_case_types)]
#![allow(non_upper_case_globals)]
#![allow(non_snake_case)]

pub mod deployer;
pub mod host_fn_error;
pub mod invoke_error;
pub mod types;
pub use host_fn_error::perform_mocked_syscall;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_my_function() {
        let _ = types::TestEnvironment::new();
    }
}
