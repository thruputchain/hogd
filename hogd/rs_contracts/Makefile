%.wasm : Makefile $(wildcard $</src/*.rs)
	cargo rustc --target wasm32-unknown-unknown $(RELEASE) -p $(basename $@) \
	-- -C link-args=-zstack-size=64000
	#&& \
	#cp target/wasm32-unknown-unknown/release/$@ built_wasms/$@

clean:
	cargo clean

.PHONY: clean rs_contracts

rs_contracts : basic.wasm basic_alloc.wasm

# rustup toolchain install nightly
# rustup target install wasm32-unknown-unknown
