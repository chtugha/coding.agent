pub const REFERENCE_PROMPT_TEMPLATE_ORIGINAL: &str =
    include_str!(concat!(env!("CARGO_MANIFEST_DIR"), "/prompts/reference_prompt_template.txt"));
pub const REFERENCE_PROMPT_TEMPLATE_SIMPLIFIED: &str = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/prompts/reference_prompt_template_simplified.txt"
));

pub fn bundled_prompt_for_style(style: &str) -> &'static str {
    match style {
        "original" => REFERENCE_PROMPT_TEMPLATE_ORIGINAL,
        _ => REFERENCE_PROMPT_TEMPLATE_SIMPLIFIED,
    }
}
