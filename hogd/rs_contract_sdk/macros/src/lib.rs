/*
#![allow(unused_imports)]
#![allow(unused_variables)]
#![allow(dead_code)]
*/

use proc_macro::TokenStream;

mod static_key;
use static_key::*;

#[proc_macro]
pub fn static_key(item: TokenStream) -> TokenStream {
    let input = syn::parse_macro_input!(item as syn::LitStr);
    let internal = false;
    static_key_output(input, internal)
}

// This version is for sdk-internal use; the difference
// is just the path to the `ByteArray` type
#[proc_macro]
pub fn _static_key(item: TokenStream) -> TokenStream {
    let input = syn::parse_macro_input!(item as syn::LitStr);
    let internal = true;
    static_key_output(input, internal)
}

mod contract;
use contract::*;

/// Generate contract-related code
#[proc_macro_attribute]
pub fn contract(
    attr_args: TokenStream,
    item_tokens: TokenStream,
) -> TokenStream {
    contract_output(attr_args, item_tokens)
}
