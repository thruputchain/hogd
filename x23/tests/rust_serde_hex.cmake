cmake_minimum_required(VERSION 3.28)
file(MAKE_DIRECTORY "${TEST_DIR}")
file(WRITE "${TEST_DIR}/hex.x" [=[
typedef opaque Digest[32];

struct Packet {
  opaque bytes[64];
  Digest digest;
};

// #[rust(newtype)]
typedef opaque Hash[32];

// #[rust(newtype)]
// #[serde(with = "serde_arrays::hex")]
typedef Digest LegacyHash;

// #[rust(newtype)]
// #[serde(with = "serde_arrays::hex")]
typedef opaque LegacyOpaque[32];
]=])

foreach(mode default enabled disabled)
  set(flags)
  if(mode STREQUAL "enabled")
    set(flags --rust-feature serde)
  elseif(mode STREQUAL "disabled")
    set(flags --rust-no-feature serde)
  endif()
  execute_process(
    COMMAND "${X23C}" --rust ${flags}
      -o "${TEST_DIR}/${mode}.rs" "${TEST_DIR}/hex.x"
    RESULT_VARIABLE result ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "x23c failed for ${mode}: ${error}")
  endif()
  file(READ "${TEST_DIR}/${mode}.rs" rust)
  string(FIND "${rust}" "serde_arrays" legacy_pos)
  if(NOT legacy_pos EQUAL -1)
    message(FATAL_ERROR "Generated Rust still refers to serde_arrays (${mode})")
  endif()
  string(REGEX MATCHALL "serde\\(with = \"xdr_common::serde_hex\"\\)" adapters "${rust}")
  list(LENGTH adapters count)
  if(mode STREQUAL "disabled")
    if(NOT count EQUAL 0)
      message(FATAL_ERROR "Hex adapter was emitted with Serde disabled")
    endif()
  else()
    # Inline opaque, newtype, and legacy-marked direct/alias newtypes.
    # Counting also catches duplicate attributes on the legacy newtype.
    if(NOT count EQUAL 4)
      message(FATAL_ERROR "Expected four hex adapters for ${mode}, got ${count}")
    endif()
    if(mode STREQUAL "default")
      string(FIND "${rust}"
        "cfg_attr(feature = \"serde\", serde(with = \"xdr_common::serde_hex\"))"
        gated_pos)
      if(gated_pos EQUAL -1)
        message(FATAL_ERROR "Default hex adapter is missing its feature gate")
      endif()
    endif()
  endif()
endforeach()
