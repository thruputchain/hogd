#[macro_use]
extern crate syn;
#[macro_use]
extern crate quote;

use proc_macro::TokenStream;
use proc_macro2::TokenStream as TokenStream2;
use quote::ToTokens;
use sha2::Digest;
use sha2::Sha256;
use syn::FnArg::Typed;
use syn::ReturnType;
use syn::{Ident, ItemFn, LitStr, Type};
use syn::Type::Reference;

fn get_method_name(arg: &str) -> String {
    let mut hasher = Sha256::new();
    hasher.update(arg.as_bytes());
    let result = hasher.finalize();

    let prefix = &result[0..4];

    let hex = hex::encode_upper(&prefix);

    let m_write = format!("{}", hex);

    m_write
}

fn get_method_name_int(arg: &str) -> u32 {
    let mut hasher = Sha256::new();
    hasher.update(arg.as_bytes());
    let result = hasher.finalize();

    let prefix = &result[0..4];

    u32::from_be_bytes(prefix.try_into().unwrap())
}

#[proc_macro]
pub fn public_interface(_item: TokenStream) -> TokenStream {
    let crate_name = format_ident!("{}", std::env::var("CARGO_PKG_NAME").unwrap());

    TokenStream::from(quote!{
        #[cfg(feature="interface")]
        pub struct #crate_name <'a> {
            call_address: &'a ::rs_contract_sdk::ContractAddress,
        }

        #[cfg(feature="interface")]
        impl <'a> #crate_name <'a> {
            pub fn new(addr: &'a ::rs_contract_sdk::ContractAddress) -> Self {
                Self {call_address : addr}
            }
        }
    })
}



fn iface_return_type(ret: &ReturnType) -> Type {
    let t = match ret {
        ReturnType::Type(_, t) => {t.clone()},
        _ => {return Type::Verbatim(TokenStream::from(quote!{()}).into());}
    };

    let ref_type = match *t {
        Reference(type_reference) => type_reference,
        _ => {return *t.clone();}
    };

    let elem = *ref_type.elem.clone();

    return Type::Verbatim(quote!{Box<#elem>});
}

#[proc_macro_attribute]
pub fn public_method(_attr: TokenStream, item: TokenStream) -> TokenStream {
    let func: ItemFn = parse_macro_input!(item as ItemFn);

    let function_signature = &func.sig;

    let function_name = &function_signature.ident;

    let n_args = function_signature.inputs.len();

    let inputs = &function_signature.inputs;

    let wrap_fn_name = format_ident!("wrapper_{}", function_name.to_string());

    let export_name =
        format!("{}", get_method_name(&function_name.to_string()));

    let parse_from_calldata_name =
        format_ident!("parse_from_calldata_{}", n_args);

    let mut types: Vec<TokenStream2> = vec![];

    for input in &function_signature.inputs {
        match input {
            Typed(t) => {
                let type_arg = *t.ty.clone();
                types.push(quote! {#type_arg});
                types.push(quote! {, });
            }
            _ => {
                panic!("invalid arg type");
            }
        }
    }

    let return_type = match &function_signature.output {
        ReturnType::Default => quote! {()},
        ReturnType::Type(_, t) => {
            let type_arg = *t.clone();
            quote! {#type_arg}
        }
    };

    let iface_return_type = iface_return_type(&function_signature.output);
    println!("{:?}", iface_return_type);
    //.let iface_return = parse_macro_input!(iface_return_tokens as Type);

    let args: TokenStream2 = types
        .iter()
        .map(|t| -> TokenStream2 { t.clone() })
        .collect();

    let crate_name = format_ident!("{}", std::env::var("CARGO_PKG_NAME").unwrap());

    TokenStream::from(quote! {
        #[cfg_attr(target_family = "wasm", export_name = #export_name)]
        #[no_mangle]
        #[cfg(not(feature="interface"))]
        pub extern "C" fn #wrap_fn_name () -> i32 {
            let calldata = match ::rs_contract_sdk::GetCalldata() {
                Ok(x) => x,
                Err(err) => {return err}
            };

            use ::rs_contract_sdk::parse_trait::OutputReturnable;

            let result = ::rs_contract_sdk::parse_trait::#parse_from_calldata_name::<
                #args
                #return_type
            >(&calldata, #function_name);

            result.return_self();
        }

        #[cfg(not(feature = "interface"))]
        #func

        #[cfg(feature = "interface")]
        impl<'a> #crate_name <'a> {
            pub fn #function_name (
                &self,
                #inputs
            ) -> Result< #iface_return_type, i32> {

                let buffer_size: usize = 4 + 4 + calldata.len();

                let mut vec = Box::<[u8]>::new_uninit_slice(buffer_size as usize);

                sdk::parse_trait::serialize_to_calldata_3(
                    vec.as_mut(),
                    slice_start,
                    slice_end,
                    calldata,
                )?;

                let calldata = unsafe { vec.assume_init() };

                let mut return_len: sdk::RuntimeMemoryLength = 0;

                let method: sdk::MethodID = 0;

                sdk::syscall_bindings::raw_Invoke(
                    &self.call_address,
                    method,
                    u64::MAX,
                    &calldata,
                    &mut return_len,
                )?;

                // Design choice: If we make a call across to another contract,
                // we cannot unwind that inside this code snipped.  Thus,
                // any subsequent part of code here should trap(), instead of returning an error code
                // (which would leave the caller uncertain of whether the error happened inside
                // the other contract, or inside the parse code on this side)

                // if raw_Invoke returns success, then it cannot be the case that 
                // GetReturnedBuffer fails (unless e.g. an allocation error)

                let returned_buf = ::rs_contract_sdk::rt::unwrap_abort(sdk::GetReturnedBuffer(return_len));

                Ok(sdk::parse_trait::parse_returns_from_calldata_1::<#iface_return_type>(&returned_buf))
            }
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn check_methods_eq() {
        let strmethod: String = get_method_name("abc").to_string();

        let intmethod: String = format!("{:2X}", get_method_name_int("abc"));

        assert_eq!(strmethod, intmethod);
    }
}
