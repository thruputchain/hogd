#pragma once

#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include "contract.hh"

namespace hogd {

// User-provided contract artifact directories searched before defaults.
void set_contract_artifact_directories(
    std::vector<std::filesystem::path> directories);

// Directories searched for contract artifacts, in priority order.
std::vector<std::filesystem::path> contract_search_directories();

// Loads a WASM contract from the contract artifact search path. Panics if the
// file cannot be loaded.
std::shared_ptr<Contract> load_contract(std::string_view filename);

// Loads raw file contents from the contract artifact search path. Panics if the
// file cannot be loaded or read.
ContractBlob load_contract_blob(std::string_view filename);

} // namespace hogd
