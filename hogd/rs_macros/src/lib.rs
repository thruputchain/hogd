#[macro_use]
extern crate syn;
#[macro_use]
extern crate quote;

use syn::FnArg::Typed;
use syn::{Ident, Type, ItemFn, LitStr};
use proc_macro::TokenStream;
use proc_macro2::TokenStream as TokenStream2;
use quote::ToTokens;
use sha2::Sha256;
use sha2::Digest;

fn get_method_name(arg: &str) -> Ident {
	let mut hasher = Sha256::new();
    hasher.update(arg.as_bytes());
    let result = hasher.finalize();

    let prefix = &result[0..4];

    let hex = hex::encode_upper(&prefix);

    let m_write = format_ident!("{}", hex);

    m_write
}

fn get_method_name_int(arg: &str) -> u32 {
	let mut hasher = Sha256::new();
    hasher.update(arg.as_bytes());
    let result = hasher.finalize();

    let prefix = &result[0..4];

    u32::from_be_bytes(prefix.try_into().unwrap())
}

#[proc_macro_attribute]
pub fn public_method(_attr: TokenStream, item: TokenStream) -> TokenStream
{
	let func : ItemFn = parse_macro_input!(item as ItemFn);

    let function_signature = &func.sig;

    let function_name = &function_signature.ident;

    let n_args = function_signature.inputs.len();

    let wrap_fn_name=format_ident!("wrapper_{}", function_name.to_string());

    let export_name = format!("{}", get_method_name(&function_name.to_string()));

    let parse_from_calldata_name = format_ident!("parse_from_calldata_{}", n_args);

    let mut types : Vec<TokenStream2> = vec![];

    for input in &function_signature.inputs {
    	match input {
    		Typed(t) => {
    			let type_arg = *t.ty.clone();
    			types.push(quote!{#type_arg});
    			types.push(quote!{, });
    		},
    		_ => {
    			panic!("invalid arg type");
    		}
    	}
    }


    let args : TokenStream2 = types.iter().map(|t| -> TokenStream2 { t.clone() } ).collect();

    TokenStream::from(quote!{
    	#[cfg_attr(target_family = "wasm", export_name = #export_name)]
    	#[no_mangle]
		pub extern "C" fn #wrap_fn_name () -> i32 {
		    let calldata = match ::rs_contract_sdk::GetCalldata() {
		    	Ok(x) => x,
		    	Err(err) => {return err}
		    };

		    match ::rs_contract_sdk::parse_trait::#parse_from_calldata_name::<
		        #args
		        (),
		    >(&calldata, #function_name)
		    {
		        Ok(()) => {
		        	return 0;
		        }
		        Err(x) => {
		        	return x;
		        }
		    }
		}

		#func
    }) 
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn check_methods_eq() {
    	let strmethod : String = get_method_name("abc").to_string();

    	let intmethod : String = format!("{:2X}", get_method_name_int("abc"));

    	assert_eq!(strmethod, intmethod);

    }
}