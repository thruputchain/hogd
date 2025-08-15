namespace hogd {

enum MultiplexMessageType {
  MultiplexMessageInvalid = 0,
  MultiplexMessageSubmittedTxs = 1,
  MultiplexMessageSCP = 2,
};

union MultiplexMessage switch (MultiplexMessageType type)
{
case MultiplexMessageSubmittedTxs:
  SubmittedTransactions submitted;
default:
  void;
};

} // namespace hogd
