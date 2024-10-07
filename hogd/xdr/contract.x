namespace hogd {

typedef opaque ContractBlob<>;

struct Contract {
  ContractBlob contract_data;
  // metadata for a future PR
};

} // namespace hogd
