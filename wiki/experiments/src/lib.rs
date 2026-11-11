pub mod attention;
pub mod projection;
pub mod vulkan;

pub type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;
