%.wasm : Makefile $(wildcard $</src/*.rs)
	cargo +nightly rustc --target wasm32-unknown-unknown --verbose --release -p $(basename $@) \
	-Zbuild-std=alloc,core -Zbuild-std-features=panic_immediate_abort \
	-- -C target-feature=+mutable-globals -C link-args=-zstack-size=64000 -C link_args=-export=__stack_pointer

clean:
	cargo clean

.PHONY: clean rs_contracts

rs_contracts : \
	basic.wasm \
	basic_alloc.wasm \
	test_bytestring.wasm \
	genesis_deployer.wasm \
	erc20.wasm \
	simple_wallet.wasm \
	rcq_test.wasm
