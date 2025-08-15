namespace hogd {

struct RejectedTransaction {
  SignedTransaction transaction;
  string reason<>;
};

} // namespace hogd
