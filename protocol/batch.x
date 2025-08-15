// clang-format off
%#include "types.hh"
%#include "transaction.hh"

namespace hogd {
  // clang-format on

  // A "batch" contains a set of proposed transactions.

  struct SubmittedTransactions {
    SignedTransaction transactions<>;
  };

} // namespace hogd
