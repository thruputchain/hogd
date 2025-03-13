use proc_macro2::TokenStream;
use syn::Result;
use syn::spanned::Spanned;
use syn::Ident;

use quote::{quote, format_ident};

pub fn generate(input: &syn::DeriveInput) -> Result<TokenStream> {

    let ident = &input.ident;
    let generics = &input.generics;

    let mut output = TokenStream::new();

    /* This is only wanted for the root object
    output.extend(quote!{
        impl ::sdk::contract::ContractStorage for #ident #generics {}
    });
    */

    output.extend(match &input.data {
        syn::Data::Struct(ref item) => generate_for_struct(ident, generics, item)?,
        _ => err(input, "Can only derive Contract for structs")?
    });

    output.extend(generate_load_impl(ident, generics)?);

    let ident_lower = Ident::new(&ident.to_string().to_lowercase(), ident.span());
    let impl_mod = format_ident!("_{}_contract_impls", ident_lower);

    Ok(quote!{

        mod #impl_mod {
            use super::*;

            #output
        }

    })
}

fn generate_for_struct(
    ident: &syn::Ident,
    generics: &syn::Generics,
    item: &syn::DataStruct,
) -> Result<TokenStream> {
    let fields = &item.fields;

    let deserialize_impl = generate_deserialize_impl(ident, generics, fields)?;

    Ok(quote!{

        use ::core::marker::PhantomData;
        use ::core::fmt;
        use ::serde::de::{Deserialize, DeserializeSeed, SeqAccess, Visitor};
        use ::serde::Deserializer;
        use ::sdk::storage::{Keypath, KeypathSeed, Load};
        use ::sdk::bytestring::BytestringGet;
        use ::sdk::serialize;

        struct _KeypathSeed<T> {
            keypath: Keypath,
            marker: PhantomData<T>,
        }

        impl<T> _KeypathSeed<T> {
            pub fn new(keypath: &Keypath) -> Self {
                Self {
                    keypath: keypath.clone(),
                    marker: PhantomData,
                }
            }
        }

        #deserialize_impl
    })
}

fn generate_load_impl(
    ident: &syn::Ident,
    generics: &syn::Generics,
) -> Result<TokenStream> {
    Ok(quote!{

        impl #generics Load for #ident #generics {
            fn load_keypath(keypath: &Keypath) -> ::sdk::result::Result<Self> {
                let key = keypath.generate_key();
                let buf = BytestringGet(&key)?;

                let seed = _KeypathSeed::<#ident #generics>::new(keypath);
                let value = serialize::from_bytes_seed(seed, &buf)?;

                Ok(value)
            }
        }

    })
}

fn generate_deserialize_impl(
    ident: &syn::Ident,
    generics: &syn::Generics,
    fields: &syn::Fields,
) -> Result<TokenStream> {
    let generic_params = &generics.params;

    let fields = if let syn::Fields::Named(ref fields_named) = fields {
        &fields_named.named
    } else {
        return err(fields, "Can only handle named struct fields");
    };

    fn visit_element(field: &syn::Field) -> Result<TokenStream> {
        let ident = &field.ident;
        let ty = &field.ty;
        Ok(quote!{

            let seed = KeypathSeed::<#ty>::new(self.keypath);
            let #ident = seq.next_element_seed(seed)?.unwrap();
            self.keypath.seq_next();

        })
    }

    let n_fields = fields.len();
    let mut visit_field_names = TokenStream::new();
    let mut visit_elements = TokenStream::new();
    for field in fields.iter() {
        let ident = &field.ident;
        visit_field_names.extend(quote!{ #ident, });

        visit_elements.extend(visit_element(field)?);
    }

    Ok(quote!{

        impl<'de, #generic_params> DeserializeSeed<'de> for _KeypathSeed<#ident #generic_params> {

            type Value = #ident #generic_params;

            fn deserialize<D>(self, deserializer: D) -> ::core::result::Result<Self::Value, D::Error>
            where
                D: Deserializer<'de>,
            {
                struct KeypathVisitor<'a> {
                    keypath: &'a mut Keypath,
                }

                impl<'de> Visitor<'de> for KeypathVisitor<'_> {
                    type Value = #ident #generic_params;

                    fn expecting(
                        &self,
                        formatter: &mut fmt::Formatter,
                    ) -> fmt::Result {
                        write!(formatter, "a #ident")
                    }

                    fn visit_seq<A>(self, mut seq: A) -> ::core::result::Result<#ident #generic_params, A::Error>
                    where
                        A: SeqAccess<'de>,
                    {
                        self.keypath.enter_seq();

                        #visit_elements

                        self.keypath.leave_seq();

                        Ok(#ident { #visit_field_names })
                    }
                }

                let mut keypath = self.keypath.clone();
                let visitor = KeypathVisitor {
                    keypath: &mut keypath,
                };
                deserializer.deserialize_tuple(#n_fields, visitor)
            }
        }

    })
}

// Produce an error referencing a particular code span
fn err<T, S, M>(spanned: &S, msg: M) -> Result<T>
where
    S: Spanned,
    M: AsRef<str>,
{
    Err(syn::Error::new(spanned.span(), msg.as_ref()))
}
