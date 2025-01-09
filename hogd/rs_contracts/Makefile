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
	test_bytestring.wasm

# rustup target install wasm32-unknown-unknown
