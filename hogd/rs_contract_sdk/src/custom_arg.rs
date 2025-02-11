pub trait TwoCustomArgs {
    fn to_u64s(&self) -> (u64, u64);
}

// required for custom arg handling in syscall
impl TwoCustomArgs for ::rs_xdr::types::Signature {
    fn to_u64s(&self) -> (u64, u64) {
        use crate::syscall::to_offset;
        use ::rs_xdr::types::CryptographicScheme;
        match self {
            Self::Ed25519 { ed25519: sig } => {
                return (
                    CryptographicScheme::Ed25519 as u64,
                    to_offset(sig) as u64,
                );
            }
            #[allow(unreachable_patterns)]
            _ => {
                crate::rt::trap();
            }
        }
    }
}

impl TwoCustomArgs for ::rs_xdr::types::PublicKey {
    fn to_u64s(&self) -> (u64, u64) {
        use crate::syscall::to_offset;
        use ::rs_xdr::types::CryptographicScheme;

        match self {
            Self::Ed25519 { ed25519: pk } => {
                return (
                    CryptographicScheme::Ed25519 as u64,
                    to_offset(pk) as u64,
                );
            }
            #[allow(unreachable_patterns)]
            _ => {
                crate::rt::trap();
            }
        }
    }
}
