use crate::InvokeMalformedTokensTest;
use sdk::sys::{self, SyscallErrorCode};
use sdk::transaction::InvocationFrameStatusCode;
use sdk::*;

pub struct InvokeMalformedTokensTestContract;

/// Calldata large enough that copying it is real host work: a quarter of the
/// 64 KiB guest memory, and well past the 4 KiB the SDK's own call path
/// stages. Lives in .rodata, so it costs no stack.
const LARGE_CALLDATA_LEN: usize = 16 * 1024;
static LARGE_CALLDATA: [u8; LARGE_CALLDATA_LEN] = [0x5a; LARGE_CALLDATA_LEN];

/// One byte: not a whole number of 4-byte token ids. The span from the
/// #2327 report.
const MALFORMED_TOKEN_BYTES: [u8; 1] = [0x7f];

/// No registered method has this id (ids are 8-byte hashes), so a child frame
/// that does start aborts in the dispatcher with LibraryExn::UnknownMethod.
const UNREGISTERED_ENTRYPOINT_ID: u64 = 0;

/// Sentinel `frame_status` seeded into the outputs before an `Invoke`, so a
/// frame-status assertion only passes if the syscall actually wrote the
/// outcome (see syscall_misc_tests for the original).
const UNWRITTEN_FRAME_STATUS: InvocationFrameStatusCode =
    InvocationFrameStatusCode(-1);

fn seeded_outputs() -> sys::CopyableInvokeOutputs {
    sys::CopyableInvokeOutputs {
        return_buffer_len: u32::MAX,
        returned_auth_token_len: u32::MAX,
        frame_status: UNWRITTEN_FRAME_STATUS,
    }
}

fn inputs() -> sys::CopyableInvokeInputs {
    sys::CopyableInvokeInputs {
        entrypoint_id: UNREGISTERED_ENTRYPOINT_ID,
        gas_limit: u64::MAX,
        flags: 0,
        reserved: 0,
    }
}

fn expect_err(res: Result<(), sys::SysErr>, expected: SyscallErrorCode) {
    match res {
        Ok(_) => panic!("expected error {:?} but syscall succeeded", expected),
        Err(e) => assert_eq!(e, expected.into()),
    }
}

/// A rejected Invoke is a no-op: the caller's output struct is left as it was.
fn assert_outputs_untouched(outputs: &sys::CopyableInvokeOutputs) {
    assert!(outputs.return_buffer_len == u32::MAX);
    assert!(outputs.returned_auth_token_len == u32::MAX);
    assert!(outputs.frame_status == UNWRITTEN_FRAME_STATUS);
}

/// Large calldata plus the malformed token span, at `address`: rejected as
/// kInvalidSyscallInput before any child frame, whatever else is wrong with
/// the call.
fn invoke_with_malformed_tokens(address: &ContractAddress) {
    let mut outputs = seeded_outputs();
    let res = sys::raw_Invoke(
        address,
        &LARGE_CALLDATA,
        &MALFORMED_TOKEN_BYTES,
        &inputs(),
        &mut outputs,
    );
    expect_err(res, SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
    assert_outputs_untouched(&outputs);
}

#[contract]
impl InvokeMalformedTokensTest for InvokeMalformedTokensTestContract {
    fn test_malformed_tokens_beat_reentrance() {
        // Self-invoke: admission would refuse this as kReentrance, so seeing
        // kInvalidSyscallInput instead pins the span check ahead of admission.
        let addr = sdk::get_current_address();
        invoke_with_malformed_tokens(&addr);
    }

    fn test_malformed_tokens_reject_before_child_frame(
        target: &ContractAddress,
    ) {
        invoke_with_malformed_tokens(target);
    }

    fn test_well_formed_tokens_run_child(target: &ContractAddress) {
        let mut outputs = seeded_outputs();
        let res = sys::raw_Invoke(
            target,
            &LARGE_CALLDATA,
            &[],
            &inputs(),
            &mut outputs,
        );
        // Ok means a child frame ran; its outcome is the unknown-method abort.
        match res {
            Ok(()) => assert!(
                outputs.frame_status == InvocationFrameStatusCode::Abort
            ),
            Err(e) => {
                panic!("expected a child frame but Invoke failed with {:?}", e)
            }
        }
    }
}
