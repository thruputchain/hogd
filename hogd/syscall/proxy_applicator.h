#pragma once

#include "hogd/syscall/metered_result.h"
#include "hogd/syscall/key_status.h"

namespace hogd
{

// Missing the contents, which is pending x23c
class ProxyApplicator {
public:

	MeteredResult<KeyStatus> has_key() const;

	MeteredResult<void> delete_last();

private:
	bool is_deleted_ = false;
};

}