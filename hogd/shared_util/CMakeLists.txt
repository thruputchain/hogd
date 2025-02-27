add_library(
  shared_util
  load_contracts.cc
  connect_to_service.cc
)

target_link_libraries(
  shared_util
  crypto
  protocol
  libsodium_shared
  base
)
