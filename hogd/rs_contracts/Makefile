rs_contracts : \
	erc20.wasm \
	caller.wasm \
	genesis_deployer.wasm

# To be upgraded to new sdk:
#	test_bytestring.wasm \
#	simple_wallet.wasm \
#	rcq_test.wasm

%.wasm : Makefile $(wildcard $</src/*.rs)
	cargo +nightly rustc \
		--features "guest,alloc" \
		--target wasm32-unknown-unknown \
		--target-dir "../../build/rs_contracts" \
		--release \
		-p $(basename $@) \
		-Zbuild-std=alloc,core \
		-Zbuild-std-features=panic_immediate_abort \
		-- \
		-C target-feature=+mutable-globals \
		-C link-args=-zstack-size=64000 \
		-C link_args=-export=__stack_pointer

test:
	# Run all contract tests
	cargo +nightly test \
		--features "guest,std" \
		--target-dir "../../build/rs_contracts" \

clean:
	cargo clean

.PHONY: rs_contracts test clean
