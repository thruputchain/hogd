namespace hogd {

enum ErrorType {
  ErrorTypeNone = 0,

  // Contract already initialized
  AlreadyInitialized = 1,

  // Caller must be contract owner
  MustBeContractOwner = 2,

  // "Internal contract error"
  InternalErr = -1
};

typedef uint128_xdr Amount;

typedef int128_xdr Difference;

union ExpectedAmount switch (ErrorType err_type)
{
case ErrorTypeNone:
  Amount amount;
default:
  void;
};

union ExpectedVoid switch (ErrorType err_type)
{
default:
  void;
};

// To construct a `ContractInterfaceIDPreimage` from an RPC program spec:
//    name: the program identifier ("GroundhogERC20")
//    vers: the version identifier, with "v" removed and underscores
//      replaced with periods (so, "0.1.0")
//
// When constructing a `ContractEntrypointID` for a particular procedure,
// put the 16-bit, big-endian encoding of the procedure constant in
// the last two ("method id") bytes.

program GroundhogERC20
{
  version v0_1_0
  {
    // Initialize contract with address of the administrative
    // owner, who can mint and burn
    ExpectedVoid initialize(
        /* contract_owner */ ContractAddress) = 4;

    // Transfer from caller to `receiver` address
    ExpectedVoid transfer(
        /* receiver */ ContractAddress,
        /* amount */ Amount) = 7;

    // Transfer from `owner` to `receiver` address, using allowance
    ExpectedVoid transfer_from(
        /* owner */ ContractAddress,
        /* receiver */ ContractAddress,
        /* amount */ Amount) = 8;

    // Adjust allowance for `spender` by `amount` (positive or negative)
    ExpectedVoid adjust_allowance(
        /* spender */ ContractAddress,
        /* amount */ Difference) = 0;

    // Get balance for given address
    ExpectedAmount get_balance_of(
        /* addr */ ContractAddress) = 3;

    // Get balance for caller
    ExpectedAmount get_balance(void) = 2;

    // Create tokens into `receiver` address
    ExpectedVoid mint(
        /* receiver */ ContractAddress,
        /* amount */ Amount) = 5;

    // Destroy tokens from `owner` address
    ExpectedVoid burn(
        /* owner */ ContractAddress,
        /* amount */ Amount) = 1;

    // Get total supply of outstanding tokens
    ExpectedAmount total_supply(void) = 6;
  }
  = 0;
}
= 0;

} // namespace hogd
