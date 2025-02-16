// WIP notes
// - method is identified by ordered index
// - #[no_mangle] is unsafe
//
#![allow(unused_imports)]
#![allow(unused_variables)]
#![allow(dead_code)]

use darling::ast::NestedMeta;
use darling::FromMeta;
use proc_macro::TokenStream;
use proc_macro2::TokenStream as TokenStream2;
use syn::parse::{Error, Result};
use syn::spanned::Spanned;
use syn::Item;

/// Generate contract-related code
#[proc_macro_attribute]
pub fn contract(
    attr_args: TokenStream,
    item_tokens: TokenStream,
) -> TokenStream {
    // Parse the attributes
    let config = match NestedMeta::parse_meta_list(attr_args.into()) {
        Ok(meta_list) => match sig::Config::from_list(&meta_list) {
            Ok(config) => config,
            Err(e) => {
                return TokenStream::from(
                    darling::Error::from(e).write_errors(),
                )
            }
        },
        Err(e) => {
            return TokenStream::from(darling::Error::from(e).write_errors());
        }
    };

    // Parse the item tokens into syntax defined in `syn`
    let item = {
        let item_tokens = item_tokens.clone();

        syn::parse_macro_input!(item_tokens as syn::Item)
    };

    // Generate the helper code
    let helper_tokens = match item_to_contract_helpers(&item, &config) {
        Ok(x) => x,
        Err(err) => return Error::into_compile_error(err).into(),
    };

    // Construct the output
    let mut output = TokenStream::new();

    // First output the original item
    output.extend(item_tokens);

    // Then add the helper items
    let helper_tokens: TokenStream = helper_tokens.into();
    output.extend(helper_tokens);

    output
}

fn item_to_contract_helpers(
    item: &syn::Item,
    config: &sig::Config,
) -> Result<TokenStream2> {
    match item {
        syn::Item::Trait(item) => {
            let sig = parse::contract_signature_from_trait(item)?;
            let client = gen::client_from_contract_signature(&sig, config)?;

            Ok(client)
        }
        syn::Item::Impl(item) => {
            let sig = parse::contract_signature_from_impl(item)?;

            let mut out = TokenStream2::new();
            let exports = gen::exports_from_contract_signature(&sig, config)?;
            out.extend(exports.clone());
            let test_registration =
                gen::test_registration_from_method_signature(&sig)?;
            out.extend(test_registration.clone());

            Ok(out)
        }
        _ => err(
            item,
            format!("Can't generate contract helpers for item {:?}", item),
        ),
    }
}

// Produce an error referencing a particular code span
fn err<T, S, M>(spanned: &S, msg: M) -> Result<T>
where
    S: Spanned,
    M: AsRef<str>,
{
    Err(Error::new(spanned.span(), msg.as_ref()))
}

mod sig {
    //! Represent contract signatures

    use darling::FromMeta;

    #[derive(Debug, FromMeta)]
    pub struct Config {
        pub mod_name: Option<String>,
    }

    impl Config {
        pub fn mod_ident_or(&self, default_name: &str) -> syn::Ident {
            let span = proc_macro2::Span::call_site();
            match &self.mod_name {
                Some(name_string) => {
                    syn::Ident::new(name_string.as_ref(), span)
                }
                None => syn::Ident::new(default_name, span),
            }
        }
    }

    #[derive(Debug)]
    pub struct Contract {
        pub span: proc_macro2::Span,
        pub ident: syn::Ident,
        pub generics: syn::Generics,
        pub methods: Vec<syn::Signature>,
    }

    #[derive(Debug)]
    pub struct ContractImpl {
        pub span: proc_macro2::Span,
        pub trait_: Option<syn::Path>,
        pub generics: syn::Generics,
        pub self_ty: Box<syn::Type>,
        pub methods: Vec<Method>,
    }

    #[derive(Debug)]
    pub struct Method {
        pub span: proc_macro2::Span,
        pub ident: syn::Ident,
        pub args: Vec<Arg>,
    }

    #[derive(Debug)]
    pub struct Arg {
        pub ident: syn::Ident,
        pub ty: syn::Type,
    }
}

mod parse {
    //! Parse syntax to contract signature

    use crate::err;
    use crate::sig;
    use syn::parse::{Error, Result};
    use syn::spanned::Spanned;
    use syn::{
        Attribute, Expr, ExprLit, ExprPath, Field, Item, ItemConst, ItemImpl,
        ItemMod, ItemStruct, ItemTrait, Lit, Type,
    };

    pub fn contract_signature_from_trait(
        item: &ItemTrait,
    ) -> Result<sig::Contract> {
        // Pull out the method signatures
        let methods = (&item.items)
            .iter()
            .filter_map(|item| {
                if let syn::TraitItem::Fn(trait_fn) = item {
                    Some(trait_fn.sig.clone())
                } else {
                    None
                }
            })
            .collect();

        Ok(sig::Contract {
            span: item.span(),
            ident: item.ident.clone(),
            generics: item.generics.clone(),
            methods,
        })
    }

    pub fn contract_signature_from_impl(
        item: &ItemImpl,
    ) -> Result<sig::ContractImpl> {
        // Get the contract trait being implemented, if any
        let trait_ = item.trait_.clone().map(|(_, path, _)| path);

        // Pull out the function items
        let methods = (&item.items)
            .into_iter()
            .filter_map(|item| {
                if let syn::ImplItem::Fn(ref impl_fn) = item {
                    let is_pub = is_visible(&impl_fn.vis);

                    // Only include public functions if this is not a trait impl
                    if trait_.is_some() || is_pub {
                        method_signature_from_sig(&impl_fn.sig)
                    } else {
                        None
                    }
                } else {
                    None
                }
            })
            .collect();
        Ok(sig::ContractImpl {
            span: item.span(),
            trait_,
            generics: item.generics.clone(),
            self_ty: item.self_ty.clone(),
            methods,
        })
    }

    fn is_visible(vis: &syn::Visibility) -> bool {
        if let syn::Visibility::Public(_) = vis {
            true
        } else {
            false
        }
    }

    fn method_signature_from_sig(sig: &syn::Signature) -> Option<sig::Method> {
        let span = sig.span().clone();
        let ident = sig.ident.clone();
        let args = (&sig.inputs)
            .iter()
            .filter_map(|fn_arg| {
                // Ignore FnArg::Receiver inputs
                if let syn::FnArg::Typed(pat_type) = fn_arg {
                    arg_from_pat_type(pat_type)
                } else {
                    None
                }
            })
            .collect();
        Some(sig::Method { span, ident, args })
    }

    fn arg_from_pat_type(pat_type: &syn::PatType) -> Option<sig::Arg> {
        let ident =
            if let syn::Pat::Ident(ref pat_ident) = pat_type.pat.as_ref() {
                pat_ident.ident.clone()
            } else {
                return None;
                // err(&pat_type.pat, "Can't handle non-ident arg pattern");
            };
        let ty = (*pat_type.ty).clone();
        Some(sig::Arg { ident, ty })
    }
}

mod gen {
    //! Generate code from contract signature

    use crate::sig;
    use proc_macro2::TokenStream as TokenStream2;
    use quote::format_ident;
    use quote::ToTokens;
    use quote::{quote, quote_spanned};
    use syn::parse::{Error, Result};
    use syn::spanned::Spanned;

    // Generate a module `client` that contains the client type
    // (a wrapper around a contract address) and an implementation
    // of the contract trait that performs cross-contract calls.
    pub fn client_from_contract_signature(
        contract: &sig::Contract,
        config: &sig::Config,
    ) -> Result<TokenStream2> {
        let trait_ident = &contract.ident;
        let client_ident = format_ident!("{}Client", trait_ident);

        let mut calls = TokenStream2::new();
        for method in contract.methods.iter() {
            calls.extend(call_from_method_signature(
                get_method_num(&method.ident.to_string()),
                method,
            )?);
        }

        let mod_ident = config.mod_ident_or("client");

        Ok(quote_spanned! {contract.span=>

            #[cfg(feature = "client")]
            pub mod #mod_ident {
                use ::sdk::types::ContractAddress;

                // use super::#trait_ident;
                use super::*; // We may need various user types

                pub struct #client_ident {
                    address: ContractAddress
                }

                impl #client_ident {
                    pub fn new(address: &ContractAddress) -> Self {
                        let address = address.clone();
                        Self { address }
                    }
                }

                use ::sdk::types::RuntimeMemoryLength;
                impl #trait_ident for #client_ident {
                    #calls
                }
            }

        })
    }

    // Generate a client method that makes a cross-contract invocation
    fn call_from_method_signature(
        method_index: u32,
        method: &syn::Signature,
    ) -> Result<TokenStream2> {
        let ident = &method.ident;
        let formal_args: Vec<&syn::FnArg> = method.inputs.iter().collect();
        let arg_names: Vec<TokenStream2> =
            method.inputs.iter().filter_map(arg_pat).collect();
        let return_type = &method.output;
        let method = method_index as u32;
        let gas_limit = u64::MAX;

        Ok(quote_spanned! {method.span()=>

            fn #ident( #(#formal_args),* ) #return_type
            {
                // Wrap up the args into one value
                let args = ( #(#arg_names),* );

                ::sdk::invoke(
                    &self.address,
                    #method,
                    #gas_limit,
                    &args)
            }

        })
    }

    fn arg_pat(fn_arg: &syn::FnArg) -> Option<TokenStream2> {
        match fn_arg {
            syn::FnArg::Receiver(_) => None,
            syn::FnArg::Typed(pat_type) => Some(pat_type.pat.to_token_stream()),
        }
    }

    pub fn test_registration_from_method_signature(
        contract: &sig::ContractImpl,
    ) -> Result<TokenStream2> {
        let mut registrations = TokenStream2::new();
        for method in &contract.methods {
            registrations.extend(registration_from_method_signature(method)?);
        }

        Ok(quote_spanned! {contract.span=>

            #[cfg(feature="mock")]
            pub mod test_setup {
                use super::exports::*;

                #[allow(dead_code)]
                pub fn register(env : &mut ::sdk::TestEnvironment) -> ::sdk::types::Contract {
                    let mut contract = env.add_contract();

                    #registrations

                    contract.as_contract()
                }
            }
        })
    }

    pub fn exports_from_contract_signature(
        contract: &sig::ContractImpl,
        config: &sig::Config,
    ) -> Result<TokenStream2> {
        let self_ty = &contract.self_ty;
        let generics = &contract.generics;

        let mut exports = TokenStream2::new();
        for method in &contract.methods {
            exports.extend(export_from_method_signature(method)?);
        }

        let mod_ident = config.mod_ident_or("exports");

        Ok(quote_spanned! {contract.span=>

            pub mod #mod_ident {
                use super::*;

                fn contract() -> #self_ty {
                    use ::sdk::types::StorageKey;
                    use ::sdk::macros::static_key;

                    static CONTRACT_STORAGE_KEY: StorageKey =
                        static_key!("Contract storage");

                    #self_ty::new(&CONTRACT_STORAGE_KEY)
                }

                #exports
            }

        })
    }

    fn export_from_method_signature(
        method: &sig::Method,
    ) -> Result<TokenStream2> {
        let ident = &method.ident;
        let export_name = get_method_name(&ident.to_string());
        let received_args = received_args(method);
        let provided_args = provided_args(method);

        let internal_name = format_ident!("{}_internal", ident);

        Ok(quote_spanned! {method.span=>

            #[cfg_attr(target_family = "wasm", export_name = #export_name)]
            pub extern "C" fn #ident() -> i32 {
                #internal_name()
            }

            pub fn #internal_name() -> i32 {
                ::sdk::invoke::handle_call(| #received_args | {
                    contract().#ident( #provided_args )
                }).into()
            }

        })
    }

    fn registration_from_method_signature(
        method: &sig::Method,
    ) -> Result<TokenStream2> {
        let ident = &method.ident;
        let export_num = get_method_num(&ident.to_string());
        let internal_name = format_ident!("{}_internal", ident);

        Ok(quote! {
            contract.register_method(#export_num, #internal_name);
        })
    }

    fn get_method_num(arg: &str) -> u32 {
        use sha2::Digest;

        let mut hasher = sha2::Sha256::new();
        hasher.update(arg.as_bytes());
        let result = hasher.finalize();

        let prefix = &result[0..4];

        return u32::from_be_bytes(prefix.try_into().unwrap());
    }

    fn get_method_name(arg: &str) -> String {
        let num = get_method_num(arg);

        format!("{:X}", num)
    }

    fn provided_args(method: &sig::Method) -> TokenStream2 {
        let provided_args: Vec<TokenStream2> =
            method.args.iter().map(provided_arg).collect();
        quote! { #(#provided_args),* }
    }

    fn provided_arg(arg: &sig::Arg) -> TokenStream2 {
        let ident = &arg.ident;
        let ty = &arg.ty;
        match ty {
            // If the interface expects a reference ...
            syn::Type::Reference(_) =>
            // ... pass a reference to the deserialized calldata
            {
                quote! { &#ident }
            }
            _ =>
            // Else, pass the owned value
            {
                quote! { #ident }
            }
        }
    }

    fn received_args(method: &sig::Method) -> TokenStream2 {
        if method.args.is_empty() {
            return quote! { () };
        }

        let received_args: Vec<TokenStream2> =
            method.args.iter().map(received_arg).collect();
        quote! { ( #(#received_args),* ) }
    }

    fn received_arg(arg: &sig::Arg) -> TokenStream2 {
        let ident = &arg.ident;
        let ty = &arg.ty;
        let ty_no_ref = match ty {
            syn::Type::Reference(ref_ty) => {
                // Strip the reference from the argument type
                &*ref_ty.elem
            }
            _ => ty,
        };
        quote! { #ident }
    }
}

#[proc_macro]
pub fn static_key(item: TokenStream) -> TokenStream {
    fn make_static_key(arg: &str) -> [u8; 32] {
        use sha2::Digest;
        use sha2::Sha256;

        let mut hasher = Sha256::new();
        hasher.update(arg.as_bytes());
        hasher.finalize().into()
    }

    let input = syn::parse_macro_input!(item as syn::LitStr);

    let string = input.token().to_string();

    let res = make_static_key(&string);

    TokenStream::from(quote::quote! {
        [#(#res),*]
    })
}
