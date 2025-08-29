#pragma once

#include <memory>
#include <string_view>

#include "contract.hh"

namespace hogd {

// Loads a contract from the built contracts directory. Takes a filename for the WASM file
// and returns a shared pointer to the loaded Contract. Throws std::runtime_error if the 
// file cannot be loaded.
std::shared_ptr<Contract> load_contract(std::string_view filename);

// Loads raw file contents from the built contracts directory.
ContractBlob load_contract_blob(std::string_view filename);

} // namespace hogd
