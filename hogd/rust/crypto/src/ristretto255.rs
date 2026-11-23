//! Bounded Ristretto255 arithmetic. All operands are public to the executor.
use curve25519_dalek::{
    ristretto::{CompressedRistretto, RistrettoPoint},
    scalar::Scalar,
    traits::VartimeMultiscalarMul,
};

pub const RISTRETTO255_MSM_TERM_BYTES: usize = 64;
pub const RISTRETTO255_MSM_MAX_TERMS: usize = 256;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InvalidInput {
    Length,
    TooManyTerms,
    Scalar,
    Point,
}

/// Each term is a canonical little-endian scalar followed by a canonical
/// compressed Ristretto point. Returns the compressed sum of scalar * point.
/// Zero scalars, identity points, duplicates and an empty sum are valid.
/// Even a zero-scalar term must have a valid point encoding.
pub fn msm(input: &[u8]) -> Result<[u8; 32], InvalidInput> {
    if input.len() % RISTRETTO255_MSM_TERM_BYTES != 0 {
        return Err(InvalidInput::Length);
    }
    if input.len() / RISTRETTO255_MSM_TERM_BYTES > RISTRETTO255_MSM_MAX_TERMS {
        return Err(InvalidInput::TooManyTerms);
    }
    let mut terms =
        Vec::with_capacity(input.len() / RISTRETTO255_MSM_TERM_BYTES);
    for term in input.chunks_exact(RISTRETTO255_MSM_TERM_BYTES) {
        let scalar = Option::<Scalar>::from(Scalar::from_canonical_bytes(
            term[..32].try_into().unwrap(),
        ))
        .ok_or(InvalidInput::Scalar)?;
        let point = CompressedRistretto(term[32..].try_into().unwrap())
            .decompress()
            .ok_or(InvalidInput::Point)?;
        terms.push((scalar, point));
    }
    Ok(RistrettoPoint::vartime_multiscalar_mul(
        terms.iter().map(|(scalar, _)| scalar),
        terms.iter().map(|(_, point)| point),
    )
    .compress()
    .to_bytes())
}

/// Compute a canonical compressed Ristretto sum. Invalid input leaves output untouched.
///
/// # Safety
/// For nonempty input, `input.message_ptr` must reference `input.message_len`
/// readable bytes. `output` must reference 32 writable bytes. Null pointers
/// are rejected except for empty input. Callers reserve work before this call.
#[no_mangle]
pub unsafe extern "C" fn ristretto255_msm(
    input: crate::MessageSpan,
    output: *mut [u8; 32],
) -> bool {
    if output.is_null()
        || input.message_len
            > RISTRETTO255_MSM_MAX_TERMS * RISTRETTO255_MSM_TERM_BYTES
        || input.message_len % RISTRETTO255_MSM_TERM_BYTES != 0
        || (input.message_len != 0 && input.message_ptr.is_null())
    {
        return false;
    }
    let result = msm(unsafe { input.to_slice() });
    match result {
        Ok(point) => {
            unsafe { output.write(point) };
            true
        }
        Err(_) => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use curve25519_dalek::{
        constants::RISTRETTO_BASEPOINT_POINT as B, traits::Identity,
    };

    fn term(scalar: Scalar, point: RistrettoPoint) -> Vec<u8> {
        [scalar.to_bytes(), point.compress().to_bytes()].concat()
    }

    fn decode_hex(s: &str) -> [u8; 32] {
        assert_eq!(s.len(), 64);
        core::array::from_fn(|i| {
            u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap()
        })
    }

    #[test]
    fn agrees_with_rfc9496_two_times_generator() {
        let expected = decode_hex(
            "6a493210f7499cd17fecb510ae0cea23a110e8d5b901f8acadd3095c73a3b919",
        );
        assert_eq!(msm(&term(Scalar::from(2u64), B)), Ok(expected));
        assert_eq!(
            msm(&[term(Scalar::ONE, B), term(Scalar::ONE, B)].concat()),
            Ok(expected)
        );
    }

    #[test]
    fn preserves_generic_group_semantics() {
        assert_eq!(msm(&[]), Ok([0; 32]));
        assert_eq!(msm(&term(Scalar::ZERO, B)), Ok([0; 32]));
        assert_eq!(
            msm(&term(Scalar::ONE, RistrettoPoint::identity())),
            Ok([0; 32])
        );
        assert_eq!(
            msm(&[term(Scalar::ONE, B), term(-Scalar::ONE, B)].concat()),
            Ok([0; 32])
        );
    }

    #[test]
    fn rejects_noncanonical_encodings_even_when_term_is_zero() {
        let mut input = term(Scalar::ZERO, B);
        input[32..].fill(255);
        assert_eq!(msm(&input), Err(InvalidInput::Point));
        let mut input = term(Scalar::ONE, B);
        input[..32].copy_from_slice(&decode_hex(
            "edd3f55c1a631258d69cf7a2def9de1400000000000000000000000000000010",
        ));
        assert_eq!(msm(&input), Err(InvalidInput::Scalar));
        input[0] -= 1;
        assert_eq!(msm(&input), Ok((-B).compress().to_bytes()));
        input[..32].fill(255);
        assert_eq!(msm(&input), Err(InvalidInput::Scalar));
    }

    #[test]
    fn rejects_each_rfc9496_invalid_encoding_class() {
        // One vector from each class in RFC 9496 Appendix A.2:
        // noncanonical s, negative s, nonsquare x^2, negative xy, y=0.
        for encoded in [
            "00ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
            "0100000000000000000000000000000000000000000000000000000000000000",
            "26948d35ca62e643e26a83177332e6b6afeb9d08e4268b650f1f5bbd8d81d371",
            "3eb858e78f5a7254d8c9731174a94f76755fd3941c0ac93735c07ba14579630e",
            "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        ] {
            for scalar in [Scalar::ZERO, Scalar::ONE] {
                let input = [scalar.to_bytes(), decode_hex(encoded)].concat();
                assert_eq!(msm(&input), Err(InvalidInput::Point));
            }
        }
    }

    #[test]
    fn validates_bounds_and_the_last_term() {
        assert_eq!(msm(&[0; 63]), Err(InvalidInput::Length));
        assert_eq!(
            msm(&vec![
                0;
                (RISTRETTO255_MSM_MAX_TERMS + 1)
                    * RISTRETTO255_MSM_TERM_BYTES
            ]),
            Err(InvalidInput::TooManyTerms)
        );
        let mut input = term(Scalar::ONE, B).repeat(RISTRETTO255_MSM_MAX_TERMS);
        assert_eq!(
            msm(&input),
            Ok((Scalar::from(RISTRETTO255_MSM_MAX_TERMS as u64) * B)
                .compress()
                .to_bytes())
        );
        input[(RISTRETTO255_MSM_MAX_TERMS - 1) * RISTRETTO255_MSM_TERM_BYTES
            + 32..]
            .fill(255);
        assert_eq!(msm(&input), Err(InvalidInput::Point));
    }

    #[test]
    fn agrees_with_individual_multiplications_for_dense_operands() {
        use sha2::{Digest, Sha512};
        for n in [1, 2, 3, 4, 8, 32, 128, RISTRETTO255_MSM_MAX_TERMS] {
            let mut input = Vec::new();
            let mut expected = RistrettoPoint::identity();
            for i in 0..n {
                let scalar = Scalar::from_bytes_mod_order_wide(
                    &Sha512::digest(format!("scalar/{i}")).into(),
                );
                let point = RistrettoPoint::from_uniform_bytes(
                    &Sha512::digest(format!("point/{i}")).into(),
                );
                input.extend(term(scalar, point));
                expected += scalar * point;
            }
            assert_eq!(msm(&input), Ok(expected.compress().to_bytes()));
        }
    }

    #[test]
    fn ffi_writes_only_successful_results() {
        let mut output = [0x5a; 32];
        let mut input = term(Scalar::ONE, B);
        input[32..].fill(255);
        let span = crate::MessageSpan {
            message_ptr: input.as_ptr(),
            message_len: input.len(),
        };
        assert!(!unsafe { ristretto255_msm(span, &mut output) });
        assert_eq!(output, [0x5a; 32]);
        let null_nonempty = crate::MessageSpan {
            message_ptr: core::ptr::null(),
            message_len: 64,
        };
        assert!(!unsafe { ristretto255_msm(null_nonempty, &mut output) });
        assert_eq!(output, [0x5a; 32]);
        let empty = crate::MessageSpan {
            message_ptr: core::ptr::null(),
            message_len: 0,
        };
        assert!(!unsafe { ristretto255_msm(empty, core::ptr::null_mut()) });
        assert!(unsafe { ristretto255_msm(empty, &mut output) });
        assert_eq!(output, [0; 32]);
    }
}
