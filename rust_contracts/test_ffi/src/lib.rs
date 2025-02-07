pub mod types;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_my_function() {
        let _ = types::Environment::new();
    }
}
