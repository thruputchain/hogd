#pragma once

#include "hogd/hogexec/runtime/serialize_calldata.h"
#include "hogd/hogexec/runtime/contract_result.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>
#include "x23/x23.h"

struct ManualRevertCaller {
    constexpr static std::string_view kTraitName = "ManualRevertCaller";
    constexpr static std::string_view kVersion = "0.0.0";

    // These will be sorted before being passed as indices to method id derivation.
    constexpr static auto kExportNames = std::array<std::string_view, 8>{
            "call_basic_manual_revert",
            "call_error_type_revert",
            "call_nested_manual_revert",
            "call_storage_manual_revert",
            "check_own_state_exists",
            "check_target_storage_exists",
            "initialize",
            "write_own_state",
        };

    // Result<()> in Rust maps to void in C++ when successful
    // The only methods that return bool are check methods
    using export_types = std::tuple<
        hogd::Result<void>(*)(bool),            // call_basic_manual_revert -> Result<()>
        hogd::Result<void>(*)(uint32_t),        // call_error_type_revert -> Result<()>
        hogd::Result<void>(*)(bool),            // call_nested_manual_revert -> Result<()>
        hogd::Result<void>(*)(bool),            // call_storage_manual_revert -> Result<()>
        bool(*)(),                              // check_own_state_exists -> bool
        bool(*)(),                              // check_target_storage_exists -> bool
        void(*)(hogd::ContractAddress),         // initialize -> void
        hogd::Result<void>(*)(x23::Vec<uint8_t>) // write_own_state -> Result<()>
    >;
}; 