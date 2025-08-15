#pragma once

#include "hogd/hogexec/runtime/serialize_calldata.h"
#include "hogd/hogexec/runtime/contract_result.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>

struct ManualRevertTest {
    constexpr static std::string_view kTraitName = "ManualRevertTest";
    constexpr static std::string_view kVersion = "0.0.0";

    // These will be sorted before being passed as indices to method id derivation.
    constexpr static auto kExportNames = std::array<std::string_view, 6>{
            "check_storage_exists",
            "internal_revert_method", 
            "test_basic_manual_revert",
            "test_error_type_revert",
            "test_nested_manual_revert",
            "test_storage_manual_revert",
        };

    // Result<()> in Rust maps to void in C++ when successful
    // The only method that returns bool is check_storage_exists
    using export_types = std::tuple<
        bool(*)(),                              // check_storage_exists -> bool
        hogd::Result<void>(*)(bool),            // internal_revert_method -> Result<()>  
        hogd::Result<void>(*)(bool),            // test_basic_manual_revert -> Result<()>
        hogd::Result<void>(*)(uint32_t),        // test_error_type_revert -> Result<()>
        hogd::Result<void>(*)(bool),            // test_nested_manual_revert -> Result<()>
        hogd::Result<void>(*)(bool)             // test_storage_manual_revert -> Result<()>
    >;
}; 