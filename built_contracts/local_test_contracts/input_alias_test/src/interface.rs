use thruput_sdk::*;

/// Which memory arguments of a syscall may overlap, as a guest sees it: inputs
/// may share bytes with each other, and an output may share bytes with
/// nothing. Two ranges overlap when they share at least one byte; a
/// zero-length range overlaps a range it lies strictly inside and not one it
/// only touches at an edge.
///
/// Every method makes its syscalls with raw argument words, so it can place
/// each argument exactly, and asserts on each result itself. A test only has
/// to see the method return.
#[contract(client = Client)]
pub trait InputAliasTest {
    /// Two inputs that are the same bytes read the same as two copies.
    fn test_inputs_at_one_address();

    /// So do three, one of them an optional input.
    fn test_every_input_at_one_address();

    /// Two inputs that share half their bytes.
    fn test_inputs_overlapping_in_part();

    /// An input span that is, or lies inside, another input.
    fn test_input_span_inside_input();

    /// A zero-length input span strictly inside another input, which the
    /// overlap predicate counts as overlapping it.
    fn test_zero_length_input_span_inside_input();

    /// An output on an input faults, by one byte or entirely, and writes
    /// nothing.
    fn test_output_overlapping_input();

    /// An output on another output faults and writes nothing.
    fn test_output_overlapping_output();

    /// A zero-length input span faults strictly inside an output and not at
    /// either edge of it.
    fn test_zero_length_input_span_against_output();

    /// A zero-length output span faults strictly inside an input and not at
    /// either edge of it.
    fn test_zero_length_output_span_against_input();

    /// Arguments laid back to back share no byte.
    fn test_touching_ranges();

    /// The argument checks come before the read-only check: in a read-only
    /// frame a write whose output overlaps an input faults, where the same
    /// write without the overlap is refused as a write. Runs the `_inner`
    /// method read-only at `callee_addr`, a second deployment of this
    /// contract (the reentrance guard rejects a self-invoke).
    fn read_only_aliasing_before_read_only_check(callee_addr: &ContractAddress);

    /// The body of `read_only_aliasing_before_read_only_check`, which must
    /// run in a read-only frame.
    fn read_only_aliasing_before_read_only_check_inner();
}
