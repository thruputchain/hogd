namespace hogd {

typedef opaque JWTSecretKey<>;

struct WalletUser {
  string login<>;
  SHA256Hash password_hash;
};

struct WalletSubmit {
  SignedTransaction signed_transactions<>;
};

enum WalletTransactionAckType {
  WalletTransactionNotSubmitted = 0,
  WalletTransactionSubmitted = 1,
};

typedef opaque WalletSubmittedTransactionID[32];

union WalletTransactionAck switch (WalletTransactionAckType ack_type)
{
case WalletTransactionNotSubmitted:
  void;
case WalletTransactionSubmitted:
  WalletSubmittedTransactionID submitted_transaction_id;
default:
  void;
};

struct WalletAck {
  WalletTransactionAck transaction_acks<>;
};

// Wallet is an API for wallets submitting transactions to chuckd.
program Wallet
{
  version Wallet1
  {

    void wallet_null(void) = 0;

    WalletAck wallet_submit(WalletSubmit) = 10;
  }
  = 1;
}
= 0x8aa87aa7;

} // namespace hogd
