#include "hogd/hogexec/runtime/test_util/port_allocator.h"
#include "hogd/base/types.h"

#include <cassert>
#include <cstdio>
#include <print>
#include <stdexcept>
#include <string>
#include <sys/fcntl.h>
#include <unistd.h>

namespace hogd {


struct LockedFile {
    LockedFile(std::string const& path) { open_file(path); }
    ~LockedFile() { 
        if (fd_ != -1) close_file(); 
    }

    void open_file(std::string const& path) {
        assert(fd_ == -1);
        fd_ = open(path.c_str(), O_CREAT | O_RDWR, 0666);
        if (fd_ == -1) {
            throw std::runtime_error("Failed to open port file: " + path);
        }
        if (flock(fd_, LOCK_EX) == -1) {
            close(fd_);
            fd_ = -1;
            throw std::runtime_error("Failed to lock port file: " + path);
        }
    }

    void close_file() {
        assert(fd_ != -1);
        [[maybe_unused]]
        int r = flock(fd_, LOCK_UN);
        assert(r == 0);
        close(fd_);
        fd_ = -1;
    }

    int fd_ = -1;
};

uint32_t allocate_test_port(std::string const& portfile) {

    LockedFile lock_file(portfile);

    // the lock file is just a sequence of 0s and 1s, representing active ports
    // offset from kMinPort.

    constexpr static size_t kBufferSize = 1024;
    uint8_t buffer[kBufferSize];
    int nread = read(lock_file.fd_, buffer, kBufferSize);
    int idx = 0;
    for (; idx < nread; idx++) {
        if (buffer[idx] == 0) {
            if (lseek(lock_file.fd_, idx, SEEK_SET) == -1) {
                throw std::runtime_error("Failed to lseek port file: " + portfile);
            }
            uint8_t byte = 1;
            if (write(lock_file.fd_, &byte, sizeof(byte)) == -1) {
                throw std::runtime_error("Failed to write port file: " + portfile);
            }
            return kMinTestPort + idx;
        }
        assert(buffer[idx] == 1);
        // port is in use; continue
    }
    assert(idx == nread);
    // no free ports; allocate a new one
    uint8_t byte = 1;
    if (write(lock_file.fd_, &byte, sizeof(byte)) != 1) {
        throw std::runtime_error("Failed to write port file: " + portfile);
    }
    return kMinTestPort + idx;
}

void free_test_port(uint32_t port, std::string const& portfile) {
    LockedFile lock_file(portfile);

    if (int r = lseek(lock_file.fd_, port - kMinTestPort, SEEK_SET); r == -1) {
        throw std::runtime_error("Failed to lseek port file: " + portfile);
    }
    uint8_t byte = 0;
    if (int r = write(lock_file.fd_, &byte, sizeof(byte)); r == -1) {
        throw std::runtime_error("Failed to write port file: " + portfile);
    }
}

} // namespace hogd