# CARGO_TARGET_DIR is set in CMakeLists.txt,
# so to build these contracts, use:
#
#   cmake --build ${BUILD}/hogd/rs_contracts/ -j

rs_contracts : \
	erc20.wasm \
	erc20_geoff.wasm \
	caller.wasm \
	genesis_deployer.wasm \
	rcq_test.wasm \
	simple_wallet.wasm \
	test_bytestring.wasm

.PHONY: rs_contracts

%.wasm : Makefile $(wildcard $</src/*.rs)
	cargo +nightly rustc \
		--no-default-features \
		--features "guest,alloc" \
		--target wasm32-unknown-unknown \
		--target-dir $(CARGO_TARGET_DIR) \
		--release \
		--verbose \
		-p $(basename $@) \
		-Zbuild-std=alloc,core \
		-Zbuild-std-features=panic_immediate_abort \
		-- \
		-C link-args=-zstack-size=64000

expand_erc20:
	cargo +nightly expand \
		--no-default-features \
		--features "alloc,guest,client" \
		--target wasm32-unknown-unknown \
		--target-dir $(CARGO_TARGET_DIR) \
		-p erc20

test_erc20:
	CC=gcc CXX=g++ \
	   cargo +nightly -vv test -p erc20
.PHONY: test_erc20

test_erc20_geoff:
	CC=gcc CXX=g++ \
	   cargo +nightly -vv test -p erc20_geoff
.PHONY: test_erc20_geoff

clean:
	cargo clean
.PHONY: clean
