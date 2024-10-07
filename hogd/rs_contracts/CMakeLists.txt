# Corrosion doesn't support (at least, not per their doc/usage.md)
# having per-target target-triple selections, which makes it
# untenable to use here and also for the wasmi lib.
# Cargo seems to work well, so we're using that here.

#Always builds contracts in release mode

ExternalProject_Add(
	rs_test_contracts
	SOURCE_DIR ${CMAKE_SOURCE_DIR}/hogd/rs_contracts/
	CONFIGURE_COMMAND ""
	BUILD_COMMAND CARGO_TARGET_DIR=${CMAKE_CURRENT_BINARY_DIR}/built_wasms/ make rs_contracts
	BUILD_IN_SOURCE true
	BUILD_ALWAYS true
	INSTALL_COMMAND ""
)

ExternalProject_Get_Property(rs_test_contracts SOURCE_DIR)
target_sources(rs_test_contracts PRIVATE ${SOURCE_DIR}/Makefile)

add_dependencies(rs_test_contracts protocol_rs generate_syscall_rs)
