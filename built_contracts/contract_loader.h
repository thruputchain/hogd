#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "contract.hh"

namespace hogd {

// User-provided contract artifact directories searched before defaults.
void set_contract_artifact_directories(
    std::vector<std::filesystem::path> directories);

// Registers an in-memory copy of a contract artifact, served before any
// directory is searched. For artifacts that shipped libraries (test_ffi)
// must load without a hogd build tree or an installed contracts directory
// on disk. `bytes` must outlive the process (embedded arrays qualify);
// registering a filename again replaces the previous bytes.
void register_embedded_contract_artifact(std::string_view filename,
                                         std::span<const unsigned char> bytes);

// Directories searched for contract artifacts, in priority order.
std::vector<std::filesystem::path> contract_search_directories();

// Loads a compiled RV32 artifact (.rv32a) from the contract artifact search
// path as a kRv32 contract. Panics if the file cannot be loaded.
std::shared_ptr<Contract> load_rv32_contract(std::string_view filename);

// Loads raw file contents from the contract artifact search path. Panics if the
// file cannot be loaded or read.
ContractBlob load_contract_blob(std::string_view filename);

} // namespace hogd
