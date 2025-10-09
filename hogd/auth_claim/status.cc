#include "hogd/auth_claim/status.h"

#include <string>

namespace hogd {

std::string AuthVerifyErrorCategory::message(int code) const {
    switch (static_cast<AuthVerifyErrorCode>(code)) {
        case AuthVerifyErrorCode::kAuthMalformed:
            return "kAuthMalformed";
        case AuthVerifyErrorCode::kSignatureVerificationFailed:
            return "kSignatureVerificationFailed";
        case AuthVerifyErrorCode::kAuthDataMismatch:
            return "kAuthDataMismatch";
    }
    return "Unknown auth verify error";
}

} // namespace hogd