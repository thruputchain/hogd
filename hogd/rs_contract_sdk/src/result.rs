use crate::syscall::SysErr;

use alloc::string::{String, ToString};
use bcs::Error as SerializationError;
use serde::{Deserialize, Serialize};
use thiserror::Error;

// For whose only error is a SysErr (syscalls or syscall-wrappers)
pub type SysResult<T> = core::result::Result<T, SysErr>;

// For other, more general SDK functions
pub type Result<T> = core::result::Result<T, Error>;

// Error type for general SDK functions
#[derive(Error, Debug, Serialize, Deserialize)]
pub enum Error {
    #[error("{context:?}: System call error: {err}")]
    SystemErr {
        err: SysErr,
        context: Option<String>,
    },

    #[error("Assertion failed: {0}")]
    AssertionFailed(String),

    #[error("IO error: {0}")]
    IO(String),

    #[error("Initialization guard failed")]
    InitGuardFailure,

    #[error("Serialization error: {0}")]
    SerializationErr(String),
}

impl From<SysErr> for Error {
    fn from(err: SysErr) -> Self {
        Self::SystemErr { err, context: None }
    }
}

impl From<SerializationError> for Error {
    fn from(err: SerializationError) -> Self {
        Self::SerializationErr(err.to_string())
    }
}

pub fn with_context<S: AsRef<str>>(msg: S) -> impl Fn(Error) -> Error {
    move |source| match source {
        Error::SystemErr { err, context: None } => Error::SystemErr {
            err,
            context: Some(msg.as_ref().to_string()),
        },
        _ => source,
    }
}

pub fn ensure<S: AsRef<str>>(condition: bool, msg: S) -> Result<()> {
    if condition {
        Ok(())
    } else {
        Err(Error::AssertionFailed(msg.as_ref().to_string()))
    }
}

pub fn check_err<F, T>(thunk: F) -> T
where
    F: Fn() -> Result<T>,
{
    match thunk() {
        Ok(x) => x,
        Err(e) => {
            panic!("{}", e);
        }
    }
}
