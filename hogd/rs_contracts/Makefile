%.wasm : Makefile $(wildcard $</src/*.rs)
	cargo rustc --target wasm32-unknown-unknown --release -p $(basename $@) -- -C link-args=-zstack-size=64000  && \
	cp target/wasm32-unknown-unknown/release/$@ built_wasms/$@

clean:
	cargo clean

# rustup toolchain install nightly
# rustup target install wasm32-unknown-unknown
