//! Types

pub use ::rs_xdr::types::*;

// A wrapper used in generated types above
pub use ::rs_xdr::ByteArray;

#[cfg(feature = "mock")]
pub use ::rs_xdr::contract::*; // export Contract for testing under name sdk::types::Contract
