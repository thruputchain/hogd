use crate::AuthWeightTest;
use sdk::sys::{self, SyscallErrorCode};
use sdk::transaction::InvocationFrameStatusCode;
use sdk::*;

pub struct AuthWeightTestContract;

/// Sentinel `frame_status` seeded into the outputs before an `Invoke`, so a
/// frame-status assertion only passes if the syscall actually wrote the
/// outcome (see syscall_misc_tests for the original).
const UNWRITTEN_FRAME_STATUS: InvocationFrameStatusCode =
    InvocationFrameStatusCode(-1);

#[contract]
impl AuthWeightTest for AuthWeightTestContract {
    fn test_write_weight(weight: u32) {
        sdk::set_address_required_weight(weight).unwrap();
    }

    fn test_get_auth_claim() {
        let claim = sdk::get_auth_claim().unwrap();
        match claim {
            AuthClaim::kSourceAddressAuthData {
                source_address_claim,
            } => {
                assert!(source_address_claim.allowed_keys == 0);
                assert!(source_address_claim.required_weight == 0);
            }
            other => panic!("unexpected auth claim: {:?}", other),
        }
    }

    fn internal_write_weight_expect_readonly_reject() {
        match sdk::set_address_required_weight(7) {
            Ok(()) => panic!("weight write succeeded in read-only frame"),
            Err(e) => assert!(
                e == SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput
                    .into()
            ),
        }
    }

    fn test_write_weight_read_only(target: &ContractAddress) {
        let entrypoint =
            crate::AuthWeightTestEntrypoints::internal_write_weight_expect_readonly_reject
                .into_u64();

        // Invoke with the ReadOnly flag (0x2). The helper asserts the
        // rejection; the frame status has to be Return (not Fault), or the
        // helper's own assertion failed.
        let mut outputs = sys::CopyableInvokeOutputs {
            return_buffer_len: 0,
            returned_auth_token_len: 0,
            frame_status: UNWRITTEN_FRAME_STATUS,
        };
        let res = sys::raw_Invoke(
            target,
            &[],
            &[],
            &sys::CopyableInvokeInputs {
                entrypoint_id: entrypoint,
                gas_limit: u64::MAX,
                flags: 0x2,
            },
            &mut outputs,
        );
        match res {
            Ok(()) => {
                assert!(outputs.frame_status == InvocationFrameStatusCode::Return)
            }
            Err(e) => panic!("read-only invoke failed: {:?}", e),
        }
    }
}
