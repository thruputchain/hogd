use proc_macro::TokenStream;

#[proc_macro]
pub fn static_key(item: TokenStream) -> TokenStream {
    let input = syn::parse_macro_input!(item as syn::LitStr);
    let string = input.token().to_string();
    let res = make_static_key(&string);

    TokenStream::from(quote::quote! {
        sdk::types::ByteArray::new([#(#res),*])
    })
}

// This version is for sdk-internal use; the difference
// is just the path to the `ByteArray` type
#[proc_macro]
pub fn _static_key(item: TokenStream) -> TokenStream {
    let input = syn::parse_macro_input!(item as syn::LitStr);
    let string = input.token().to_string();
    let res = make_static_key(&string);

    TokenStream::from(quote::quote! {
        ::groundhog_xdr::ByteArray::new([#(#res),*])
    })
}

fn make_static_key(arg: &str) -> [u8; 32] {
    use sha2::Digest;
    use sha2::Sha256;

    let mut hasher = Sha256::new();
    hasher.update(arg.as_bytes());
    hasher.finalize().into()
}
