All syscalls can return error kFault if a user-supplied argument points to an
invalid contract memory address.

All syscalls that return a bytespan of non-static size (a [u8], as written
here) take as one of their arguments a contract-allocated bytespan, in which
the output argument should be written, AND return a RuntimeMemoryLength
variable, which describes how much of the contract-supplied bytespan was
overwritten. We do not change any of the remaining elements of the bytespan.

# SHA256 

 Signature: SHA256(input_buffer : [u8]) -> SHA256Hash

Returns SHA256 of the input.

Does not return additional errors.

# GetMsgSender 

Signature: GetMsgSender() -> ContractAddress

Returns the contract address of the contract that invoked the current call
frame.

Can return the error kIsRootTransaction if called from a root call frame.

# GetCalldataLen 

Signature: GetCalldataLen() -> RuntimeMemoryLength

Returns the length of the calldata supplied to the invocation associated with
the current call frame.

Does not return additional errors.

# GetCalldata 

Signature: () -> [u8]

Returns the calldata supplied to the invocation associated with the current
call frame. Truncates the calldata to the size of the bytestring allocated by
the contract to accept the calldata.

Does not return additional errors.

# Log 

Signature: (message : [u8]) -> ()

Creates a log entry associated with the current transaction using the supplied
message, associated with the address of the currently running contract.

Returns kInvalidSyscallInput if the supplied message is too long.

# Invoke 

Signature: (call_address: ContractAddress, method: MethodID,
  gas_limit: GasAmount, calldata: [u8]) -> RuntimeMemoryLength

Invokes the specified method of the contract at the specified address, with
the supplied parameters. Creates a new callframe, which terminates upon
conclusion of the Invoke() syscall.

Returns the length of the buffer returned by the called contract (which is not accessible, if the frame has an error).

# Return 

Signature: (output : [u8]) -> !

Immediately terminates the currently running contract, and sets the parent
callframe's return buffer to be the supplied `output` value.  Sets the exit
code for the current call frame to 0.

If the supplied bytestring is too long, the currently running contract
terminates with a negative exit code (to revert the call frame).

# GetReturnedBuffer

 Signature: () -> [u8]

Loads the bytestring returned by the most recent child callframe.

Returns error kNoReturnBufferAvailable if the most recent child callframe does
not exist or exited in an error state.

# Exit

Signature: Exit(code : i32) -> !

Immediately exits the current contract, with the supplied exit code.  Does not
set the parent call frame's  return buffer. Reverts the contract if the exit
codes is negative.


# GetDeployAddress 

Signature: (
  source_address: Option<ContractAddress>, 
  nonce: SHA256Hash, 
  contract_hash: SHA256Hash) -> ContractAddress

Returns SHA256(source_address || nonce || contract_hash).

If source_address is not present, source_address is replaced with the address
of the currently-running contract.

This returned hash is the address at which the DeployContract syscall, if
called from `source_address`, will deploy a contract with the supplied
arguments `nonce` and `contract_hash`.

Does not return additional errors.

# DeployContract

Signature: DeployContract(nonce: SHA256Hash, contract_hash: SHA256Hash) ->
ContractAddress

Deploys the contract specified by the input hash at the address defined by
GetDeployAddress(nonce, contract_hash).

Returns the address of the newly deployed contract.

Returns kContractLookupFailed if no contract with the specified hash exists,
or kContractAlreadyDeployed if there is already a contract deployed at the
specified address.

The deployment is visible only within the current transaction's context
(until the next block). If the call frame reverts, the deployment is reverted
(like all of the other state changes made within the reverting call frame).

# GetSourceTransactionHash

Signature: GetSourceTransactionHash() -> SHA256Hash

Returns the sha256 hash of the originating transaction.

No other errors.

# VerifySignature

Signature: VerifySignature(signature: Signature, publickey: Publickey, message : [u8]) -> ()

Check the error code to determine verification status.

# BytestringGetLen

Signature: BytestringGetLen(key : StorageKey) -> RuntimeMemoryLength

Gets the length of the bytestring at key if it exists.
Returns an error otherwise.

# BytestringGet

Signature: BytestringGet(key: StorageKey) -> [u8], RuntimeMemoryLength

Returns the bytestring at the input key, if it exists.

# BytestringSet

Signature: BytestringSet(key: StorageKey, string : [u8]) -> ()

Creates a bytestring at the specified key.


