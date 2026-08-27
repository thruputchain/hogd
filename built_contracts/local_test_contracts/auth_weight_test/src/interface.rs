use sdk::*;

/// Exercises the auth-configuration syscalls from a contract frame:
/// WriteAddressRequiredWeight and GetAuthClaim, neither of which has any
/// other contract-side coverage (syscall_auth_config_test.cc drives them
/// natively, without going through a guest runtime).
#[contract(client = Client)]
pub trait AuthWeightTest {
    /// Happy path: sets the address's required signing weight. The host-side
    /// test verifies the committed AuthData storage item afterwards.
    fn test_write_weight(weight: u32);

    /// GetAuthClaim from a contract frame returns the transaction's claim.
    /// The test transaction carries the default claim: a source-address
    /// claim with an empty bitmap and required weight 0.
    fn test_get_auth_claim();

    /// Internal helper for the read-only test: expects the weight write to
    /// be rejected with kInvalidSyscallInput. Runs at a second deployment of
    /// this contract (the reentrance guard rejects a self-invoke).
    fn internal_write_weight_expect_readonly_reject();

    /// WriteAddressRequiredWeight is a write syscall: in a read-only child
    /// frame it must fail with kInvalidSyscallInput and leave no AuthData
    /// behind.
    fn test_write_weight_read_only(target: &ContractAddress);
}
