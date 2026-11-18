#!/usr/bin/env bash
set -euo pipefail

if [[ $# -eq 0 ]]; then
  echo "usage: $0 <binary>..." >&2
  exit 1
fi

status=0

for binary in "$@"; do
  if [[ ! -x "$binary" ]]; then
    echo "error: missing executable: $binary" >&2
    status=1
    continue
  fi

  # Some libraries carry hand-written AVX-512 code that they run only after a
  # runtime CPU check. Those symbols are allowed; generic dependency or
  # application code is not.
  #
  # - BLAKE3: the *_avx512 implementations.
  # - OpenSSL libcrypto, linked statically with HOGD_STATIC_DEPS: the AES-XTS,
  #   AES-GCM, ChaCha20, Poly1305 and RSAZ assembly modules. OpenSSL selects
  #   them from OPENSSL_ia32cap_P. The labels iv_len_12_init_IV and
  #   skip_iv_len_12_init_IV belong to the AES-GCM module.
  if objdump -d --demangle "$binary" | awk -v binary="$binary" '
    function runtime_dispatched(name) {
      return name ~ /^_?blake3_.*_avx512$/ ||
        name ~ /^aesni_xts_(128|256)_(en|de)crypt_avx512$/ ||
        name ~ /^ossl_(aes_gcm_[a-z_]+|gcm_gmult)_avx512$/ ||
        name ~ /^(skip_)?iv_len_12_init_IV$/ ||
        name ~ /^ChaCha20_(16x|8xvl|avx512|avx512vl)$/ ||
        name ~ /^poly1305_blocks_(avx512|vpmadd52(_4x|_8x)?)$/ ||
        name ~ /^ossl_rsaz_amm52x[0-9]+_x[12]_ifma256$/ ||
        name ~ /^ossl_extract_multiplier_2x[0-9]+_win5$/
    }
    {
      delete fields
      has_evex = 0
      has_avx512 = 0
    }
    /^[[:xdigit:]]+ <.*>:$/ {
      symbol = $0
      sub(/^[[:xdigit:]]+ </, "", symbol)
      sub(/>:$/, "", symbol)
      next
    }
    /^[[:space:]]*[[:xdigit:]]+:/ {
      rest = $0
      sub(/^[[:space:]]*[[:xdigit:]]+:[[:space:]]*/, "", rest)
      split(rest, fields, /[[:space:]]+/)

      mnemonic = ""
      for (i = 1; i in fields; i++) {
        if (fields[i] ~ /^[[:xdigit:]][[:xdigit:]]$/) {
          continue
        }
        mnemonic = fields[i]
        break
      }

      # AVX-512 and AVX-512VL instructions are EVEX-encoded. Checking the raw
      # prefix catches instructions that only use xmm/ymm operands and no masks.
      has_evex = tolower(fields[1]) == "62" && mnemonic != "" && mnemonic != "(bad)"

      has_avx512 = has_evex || \
        $0 ~ /%zmm[0-9][0-9]*/ || \
        $0 ~ /(^|[^[:alnum:]_])zmm[0-9][0-9]*([^[:alnum:]_]|$)/ || \
        $0 ~ /%k[1-7]([^[:digit:]]|$)/ || \
        $0 ~ /(^|[^[:alnum:]_])k[1-7]([^[:alnum:]_]|$)/
    }
    has_avx512 {
      if (runtime_dispatched(symbol)) {
        next
      }
      found++
      if (found <= 25) {
        printf("%s: unexpected AVX-512 in %s: %s\n", binary, symbol, $0)
      }
    }
    END {
      if (found > 25) {
        printf("%s: ... %d more unexpected AVX-512 instructions omitted\n",
               binary, found - 25)
      }
      exit found ? 1 : 0
    }
  '; then
    :
  else
    status=1
  fi
done

exit "$status"
