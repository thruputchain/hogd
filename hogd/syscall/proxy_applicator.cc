#include "hogd/syscall/proxy_applicator.h"

#include "hogd/syscall/metered_result.h"
#include "hogd/syscall/key_status.h"

namespace hogd
{

MeteredResult<KeyStatus> 
ProxyApplicator::has_key() const {
	if (is_deleted_) {
		return {KeyStatus::kKeyAlreadyDeleted, 0};
	}
	// exists case unimplemented, pending x23c
	return {KeyStatus::kKeyNExist, 0};
}
	
MeteredResult<void> 
ProxyApplicator::delete_last()
{
	is_deleted_ = true;
	return {{}, 0};
}

}
