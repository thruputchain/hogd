# CARGO_TARGET_DIR is set in CMakeLists.txt,
# so to build these contracts, use:
#
#   cmake --build ${BUILD}/hogd/rs_contracts/

rs_contracts : \
	erc20.wasm \
	erc20_geoff.wasm \
	caller.wasm \
	genesis_deployer.wasm \
	rcq_test.wasm \
	simple_wallet.wasm \
	test_bytestring.wasm

%.wasm : Makefile $(wildcard $</src/*.rs)
	cargo +nightly rustc \
		--features "guest,alloc" \
		--target wasm32-unknown-unknown \
		--target-dir $(CARGO_TARGET_DIR) \
		--release \
		-p $(basename $@) \
		-Zbuild-std=alloc,core \
		-Zbuild-std-features=panic_immediate_abort \
		-- \
		-C link-args=-zstack-size=64000

test:
	# Run all contract tests
	cargo +nightly test \
		--features "guest,std" \
		--target-dir $(CARGO_TARGET_DIR)

clean:
	cargo clean

.PHONY: rs_contracts test clean
