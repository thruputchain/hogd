use sdk::*;

/// Pins how the Invoke syscall treats a malformed owned-auth-token byte span
/// (a length that is not a whole number of 4-byte ids) that arrives together
/// with contract-chosen calldata (#2327).
///
/// A rejected Invoke bills nothing beyond the flat syscall crossing, so any
/// host work done ahead of the rejection is unpriced. The span-length check
/// is the cheapest check Invoke runs and has to come first: ahead of
/// admission (reentrance, token ownership, the depth bound), which in turn
/// runs ahead of the host's copy of the calldata (#2973). A guest cannot
/// watch that copy -- it has no side effect and costs no gas -- so the pin is
/// on the error precedence the ordering implies, with a control showing that
/// the same calldata does reach a child frame once the span is well-formed.
///
/// These need an rv32 caller: hogd's native syscall entry takes a typed span
/// of uint32_t token ids, whose length is a count of ids, so it cannot express
/// a misaligned byte span at all.
#[contract(client = Client)]
pub trait InvokeMalformedTokensTest {
    /// Self-invoke with large calldata and a one-byte token span. Reports
    /// kInvalidSyscallInput, not kReentrance: the span check precedes
    /// admission, and admission precedes the calldata copy.
    fn test_malformed_tokens_beat_reentrance();

    /// The same input against a separately deployed `target`. Rejected as
    /// kInvalidSyscallInput with no child frame: every output keeps the
    /// sentinel it was seeded with.
    fn test_malformed_tokens_reject_before_child_frame(
        target: &ContractAddress,
    );

    /// Control: the same calldata with an empty (well-formed) token span does
    /// start a child frame at `target`, so the rejections above are the span
    /// check and not anything about the calldata. The child is asked for an
    /// entrypoint id no method registers, so it aborts (UnknownMethod) before
    /// touching its calldata; the host-side trace shows the calldata arrived.
    fn test_well_formed_tokens_run_child(target: &ContractAddress);

    /// Forward a single well-formed, unowned token id to `target`. Admission
    /// refuses it as kInvalidSyscallInput because the frame owns no tokens,
    /// after the host has parsed the one-id span (#4174).
    fn test_one_unowned_token(target: &ContractAddress);

    /// The same, with a many-id span. The span is well-formed and every id is
    /// unowned, so admission refuses it too -- but only after the host has
    /// copied and hashed the whole span, which is why its parse must be billed
    /// in proportion to the span length rather than at the flat crossing
    /// (#4174).
    fn test_many_unowned_tokens(target: &ContractAddress);
}
