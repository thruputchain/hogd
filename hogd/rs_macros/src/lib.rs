#[macro_use]
extern crate syn;
#[macro_use]
extern crate quote;

use proc_macro::TokenStream;
use proc_macro2::TokenStream as TokenStream2;
use sha2::Digest;
use sha2::Sha256;
use syn::parse::{Parse, ParseStream};
use syn::FnArg::Typed;
use syn::ReturnType;
use syn::Type::{Reference, Tuple};
use syn::{ItemFn, Lit, LitInt, LitStr, Type, TypeTuple};

use syn::Token;

fn get_method_name(arg: &str) -> String {
    let mut hasher = Sha256::new();
    hasher.update(arg.as_bytes());
    let result = hasher.finalize();

    let prefix = &result[0..4];

    let hex = hex::encode_upper(&prefix);

    let m_write = format!("{}", hex);

    m_write
}

fn make_static_key(arg: &str) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(arg.as_bytes());
    hasher.finalize().into()
}

#[proc_macro]
pub fn static_key(item: TokenStream) -> TokenStream {
    let input = parse_macro_input!(item as LitStr);

    let string = input.token().to_string();

    let res = make_static_key(&string);

    TokenStream::from(quote! {
        [#(#res),*]
    })
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
    let crate_name =
        format_ident!("{}", std::env::var("CARGO_PKG_NAME").unwrap());

    TokenStream::from(quote! {
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

// TODO(geoff) Ref to Box on tuple types
fn iface_return_type_inner(t: &Type) -> Type {
    match t {
        Reference(type_reference) => {
            let elem = *type_reference.elem.clone();
            return Type::Verbatim(quote! {Box<#elem>});
        }
        Tuple(tuple_type) => {
            let old_elems = tuple_type.elems.clone();

            return Type::Tuple(TypeTuple {
                paren_token: tuple_type.paren_token.clone(),
                elems: old_elems
                    .iter()
                    .map(|t| iface_return_type_inner(&t))
                    .collect(),
            });
        }
        _ => return t.clone(),
    }
}

fn iface_return_type(ret: &ReturnType) -> Type {
    let t = match ret {
        ReturnType::Type(_, t) => t.clone(),
        _ => {
            return Type::Verbatim(TokenStream::from(quote! {()}).into());
        }
    };

    return iface_return_type_inner(&t);
}

struct AttributeArgs {
    static_calldata_length: Option<LitInt>,
    static_return_length: Option<LitInt>,
}

impl AttributeArgs {
    pub fn new(scl: &Option<LitInt>, srl: &Option<LitInt>) -> Self {
        Self {
            static_calldata_length: scl.clone(),
            static_return_length: srl.clone(),
        }
    }
}

impl Parse for AttributeArgs {
    fn parse(input: ParseStream) -> syn::Result<Self> {
        let mut scl = None;
        let mut srl = None;

        while !input.is_empty() {
            let lookahead = input.lookahead1();
            if lookahead.peek(syn::Ident) {
                let key: syn::Ident = input.parse()?;
                input.parse::<Token![=]>()?;

                if key == "static_calldata_length" {
                    let lit: Lit = input.parse()?;
                    if let Lit::Int(lit_int) = lit {
                        scl = Some(lit_int);
                    }
                } else if key == "static_return_length" {
                    let lit: Lit = input.parse()?;
                    if let Lit::Int(lit_int) = lit {
                        srl = Some(lit_int);
                    }
                } else {
                    return Err(syn::Error::new(
                        key.span(),
                        "Unknown attribute",
                    ));
                }
            }

            // Parse comma between attributes
            if !input.is_empty() {
                input.parse::<Token![,]>()?;
            }
        }

        Ok(AttributeArgs::new(&scl, &srl))
    }
}

#[proc_macro_attribute]
pub fn public_method(attr: TokenStream, item: TokenStream) -> TokenStream {
    let tokens: AttributeArgs = parse_macro_input!(attr as AttributeArgs);

    let calldata_len: Option<LitInt> = tokens.static_calldata_length;
    let return_len: Option<LitInt> = tokens.static_return_length;

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

    let serialize_to_calldata_name =
        format_ident!("serialize_to_calldata_{}", n_args);

    let compute_serialize_len_name =
        format_ident!("compute_serialize_len_{}", n_args);

    let mut types: Vec<TokenStream2> = vec![];

    let mut arg_names: Vec<TokenStream2> = vec![];

    for input in &function_signature.inputs {
        match input {
            Typed(t) => {
                let type_arg = *t.ty.clone();
                types.push(quote! {#type_arg});
                types.push(quote! {, });
                let pat = *t.pat.clone();
                arg_names.push(quote! {#pat});
                arg_names.push(quote! {, });
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

    let args: TokenStream2 = types
        .iter()
        .map(|t| -> TokenStream2 { t.clone() })
        .collect();

    let arg_names_join: TokenStream2 = arg_names
        .iter()
        .map(|t| -> TokenStream2 { t.clone() })
        .collect();

    let crate_name =
        format_ident!("{}", std::env::var("CARGO_PKG_NAME").unwrap());

    let methodname: u32 = get_method_name_int(&function_name.to_string());

    let interface_calldata_decl = match calldata_len.clone() {
        None => quote! {
            let buffer_size: usize = ::rs_contract_sdk::parse_trait::#compute_serialize_len_name (
                        #arg_names_join);

            let mut vec = Box::<[u8]>::new_uninit_slice(buffer_size);

            ::rs_contract_sdk::parse_trait::#serialize_to_calldata_name (
                vec.as_mut(),
                #arg_names_join
            )?;

            let calldata = unsafe { vec.assume_init() };
        },
        Some(x) => quote! {
            let buffer_size : usize = #x;

            let mut calldata_mu= [::core::mem::MaybeUninit::<u8>::uninit(); #x];

            ::rs_contract_sdk::parse_trait::#serialize_to_calldata_name (
                &mut calldata_mu,
                #arg_names_join
            )?;

            let calldata : [u8; #x] = unsafe { core::mem::transmute(calldata_mu)};
        },
    };

    let calldata_decl = match calldata_len.clone() {
        None => {
            quote! {
                let calldata = match ::rs_contract_sdk::GetCalldata() {
                    Ok(x) => x,
                    Err(err) => {return err}
                };
            }
        }
        Some(x) => {
            quote! {
                let mut calldata_mu= [::core::mem::MaybeUninit::<u8>::uninit(); #x];
                let mut calldata_len : ::rs_contract_sdk::RuntimeMemoryLength = 0;
                match ::rs_contract_sdk::syscall_bindings::uninit_GetCalldata(&mut calldata_mu, &mut calldata_len) {
                    Ok(()) => {},
                    Err(err) => {return err;},
                };

                if calldata_len != #x {
                    return ::rs_contract_sdk::SyscallErrorCode::kCalldataParseError as i32;
                }

                let calldata : [u8; #x] = unsafe {core::mem::transmute(calldata_mu)};
            }
        }
    };

    let get_return_buf_decl = match return_len.clone() {
        None => {
            quote! {
               let returned_buf = ::rs_contract_sdk::rt::unwrap_abort(::rs_contract_sdk::GetReturnedBuffer(return_len));
            }
        }
        Some(x) => {
            quote! {
                if #x != return_len {
                    ::rs_contract_sdk::rt::trap();
                }
                let mut return_mu= [::core::mem::MaybeUninit::<u8>::uninit(); #x];
                let mut return_written_len : ::rs_contract_sdk::RuntimeMemoryLength = 0;

                ::rs_contract_sdk::rt::unwrap_abort(
                    ::rs_contract_sdk::syscall_bindings::uninit_GetReturnedBuffer(&mut return_mu, &mut return_written_len));

                // should be guaranteed by runtime.  if this fails, something weird has happened
                if #x != return_written_len {
                    ::rs_contract_sdk::rt::trap();
                }

                let returned_buf : [u8; #x] = unsafe {core::mem::transmute(return_mu)};
            }
        }
    };

    TokenStream::from(quote! {
        #[cfg_attr(target_family = "wasm", export_name = #export_name)]
        #[no_mangle]
        #[cfg(not(feature="interface"))]
        pub extern "C" fn #wrap_fn_name () -> i32 {
            #calldata_decl

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

                #interface_calldata_decl

                let mut return_len: sdk::RuntimeMemoryLength = 0;

                let method: sdk::MethodID = #methodname;

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

                //let returned_buf = ::rs_contract_sdk::rt::unwrap_abort(sdk::GetReturnedBuffer(return_len));
                #get_return_buf_decl

                Ok(sdk::parse_trait::parse_returns_from_calldata::<#iface_return_type>(&returned_buf)?)
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
