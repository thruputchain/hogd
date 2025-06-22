#pragma once

#include <string>

#include "hogd/base/types.h"

namespace hogd {

constexpr static uint32_t kMinTestPort = 10000;

uint32_t allocate_test_port(std::string const& portfile);
void free_test_port(uint32_t port, std::string const& portfile);

struct TemporaryPort {
    TemporaryPort(std::string const& portfile = std::string(CMAKE_ROOT_BINARY_DIR) + "/active_test_ports.txt")
        : port_(allocate_test_port(portfile)), portfile_(portfile) {}

    ~TemporaryPort() { free_test_port(port_, portfile_); }

    const uint32_t port_;
    const std::string portfile_;
};

} // namespace hogd