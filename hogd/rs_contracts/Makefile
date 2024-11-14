%.wasm : Makefile $(wildcard $</src/*.rs)
	cargo +nightly rustc --target wasm32-unknown-unknown --release -p $(basename $@) \
	-Zbuild-std=alloc,core -Zbuild-std-features=panic_immediate_abort \
	-- -C link-args=-zstack-size=64000

clean:
	cargo clean

.PHONY: clean rs_contracts

rs_contracts : \
	basic.wasm \
	basic_alloc.wasm \
	test_calldata.wasm \
	test_invoke_1.wasm \
	test_invoke_2.wasm \
	echo.wasm \
	test_echo.wasm \
	test_bytestring.wasm \
	test_nnint64.wasm \
	genesis_deployer.wasm \
	erc20.wasm \
	simple_wallet.wasm \
	test_deploy_revert_1.wasm \
	test_deploy_revert_2.wasm \
	test_reentrant_revert.wasm \
	test_optional_argument.wasm

# rustup target install wasm32-unknown-unknown
