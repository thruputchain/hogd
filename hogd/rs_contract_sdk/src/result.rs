use crate::syscall::SysErr;
use thiserror::Error;
use bcs::Error as SerializationError;

pub type Result<T> = core::result::Result<T, Error>;

#[derive(Error, Debug)]
pub enum Error {

    #[error("{context:?}: System call error: {code}")]
    SystemErr {
        code: SysErr,
        context: Option<&'static str>
    },

    #[error("Assertion failed: {0}")]
    AssertionFailed(&'static str),

    #[error("IO error: {0}")]
    IO(&'static str),

    #[error("Initialization guard failed")]
    InitGuardFailure,

    #[error("Serialization error: {0}")]
    Serialization(#[from] SerializationError),
}

impl From<SysErr> for Error {
    fn from(e: SysErr) -> Self {
        Self::SystemErr {
            code: e,
            context: None
        }
    }
}

pub fn with_context(msg: &'static str)
    -> impl Fn (Error) -> Error
{
    move |source| {
        match source {
            Error::SystemErr { code, context: None } =>
                Error::SystemErr { code, context: Some(msg) },
            _ =>
                source
        }
    }
}

pub fn assert(condition: bool, msg: &'static str) -> Result<()> {
    if condition {
        Ok(())
    } else {
        Err(Error::AssertionFailed(msg))
    }
}

pub fn check_err<F, T>(thunk: F) -> T
    where F: Fn() -> Result<T>
{
    match thunk() {
        Ok(x) => x,
        Err(e) => {
            panic!("{}", e);
        }
    }
}
