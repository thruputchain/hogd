#include "hogd/hogexec/preset_contracts/shared.h"

#include <cstring>
#include <string_view>

#include "contract.hh"
#include "hogd/crypto/crypto.h"
#include "types.hh"

namespace hogd {

MethodID sdk_method_id(std::string_view trait_name, std::string_view method_name) {

    ContractMethodNamePreimage preimage { 
        .trait_readable_name = {trait_name.begin(), trait_name.end()},
        .method_readable_name ={method_name.begin(), method_name.end()}
    };

    auto h = sha256(preimage);

    MethodID out;
    std::memcpy(&out, h.data(), sizeof(MethodID));
    return out;
}

}