use anyhow::Result;
use serde::{Deserialize, Serialize};
use std::path::Path;
use std::sync::{Arc, RwLock};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TriggerConfig {
    pub id: String,
    pub trigger_type: String,
    pub match_pattern: String,
    pub label: String,
    #[serde(default = "default_action_type")]
    pub action_type: String,
    #[serde(default)]
    pub action_url: Option<String>,
    #[serde(default = "default_cooldown")]
    pub cooldown_secs: f64,
    #[serde(default)]
    pub inject_result: bool,
}

fn default_action_type() -> String {
    "tomedo-crawl-query".to_string()
}

const fn default_cooldown() -> f64 {
    10.0
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LlmProfile {
    pub id: String,
    #[serde(alias = "url")]
    pub base_url: String,
    pub model: String,
    #[serde(default, alias = "api_key")]
    pub api_key_env: Option<String>,
    #[serde(default = "default_prompt_style")]
    pub prompt_style: String,
    #[serde(default)]
    pub is_default: bool,
}

fn default_prompt_style() -> String {
    "simplified".to_string()
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AppConfig {
    #[serde(default = "default_listen_addr")]
    pub listen_addr: String,
    #[serde(default = "default_listen_port")]
    pub listen_port: u16,
    #[serde(default = "default_log_dir")]
    pub log_dir: String,
    #[serde(default = "default_instance_name")]
    pub instance_name: String,
    #[serde(default)]
    pub llm_mode_enabled: bool,
    #[serde(default)]
    pub arc_mode_enabled: bool,
    #[serde(default = "default_tomedo_crawl_url")]
    pub tomedo_crawl_url: Option<String>,
    #[serde(default = "default_trigger_window")]
    pub trigger_window_chars: usize,
    #[serde(default)]
    pub llm_profiles: Vec<LlmProfile>,
    #[serde(default = "default_timeout")]
    pub default_timeout_secs: f64,
    #[serde(default = "default_max_tokens")]
    pub default_max_tokens: usize,
    #[serde(default)]
    pub triggers: Vec<TriggerConfig>,
    #[serde(default)]
    pub allowed_webhook_hosts: Vec<String>,
    #[serde(default)]
    pub allowed_script_dir: Option<String>,
    #[serde(default = "default_ret_action_type")]
    pub ret_action_type: String,
    #[serde(default = "default_ret_inject_result")]
    pub ret_inject_result: bool,
    #[serde(default)]
    pub ret_action_url: Option<String>,
}

fn default_listen_addr() -> String {
    "127.0.0.1".to_string()
}

const fn default_listen_port() -> u16 {
    8090
}

fn default_log_dir() -> String {
    "$HOME/tmp/moshi-rag-backend-logs".to_string()
}

fn default_instance_name() -> String {
    "moshi-rag-backend".to_string()
}

#[allow(clippy::unnecessary_wraps)] // serde(default) requires matching the field's Option<String> type
fn default_tomedo_crawl_url() -> Option<String> {
    Some("http://127.0.0.1:13181".to_string())
}

const fn default_trigger_window() -> usize {
    200
}

pub const fn default_timeout() -> f64 {
    3.0
}

pub const fn default_max_tokens() -> usize {
    256
}

fn default_ret_action_type() -> String {
    "tomedo-crawl-query".to_string()
}

const fn default_ret_inject_result() -> bool {
    true
}

fn replace_env_vars(s: &str) -> String {
    static RE: std::sync::OnceLock<regex::Regex> = std::sync::OnceLock::new();
    let re = RE.get_or_init(|| {
        regex::Regex::new(r"\$([A-Za-z_][A-Za-z0-9_]*)").expect("env-var regex is valid")
    });
    re.replace_all(s, |caps: &regex::Captures| std::env::var(&caps[1]).unwrap_or_default())
        .into_owned()
}

impl AppConfig {
    pub fn load<P: AsRef<Path>>(path: P) -> Result<Self> {
        let content = std::fs::read_to_string(path)?;
        let mut config: Self = serde_json::from_str(&content)?;
        config.log_dir = replace_env_vars(&config.log_dir);
        if let Some(ref mut url) = config.tomedo_crawl_url {
            *url = replace_env_vars(url);
        }
        if let Some(ref mut dir) = config.allowed_script_dir {
            *dir = replace_env_vars(dir);
        }
        for host in config.allowed_webhook_hosts.iter_mut() {
            *host = replace_env_vars(host);
        }
        Ok(config)
    }
}

#[derive(Clone)]
pub struct SharedConfig {
    inner: Arc<RwLock<AppConfig>>,
}

impl SharedConfig {
    pub fn new(config: AppConfig) -> Self {
        Self { inner: Arc::new(RwLock::new(config)) }
    }

    pub fn read(&self) -> std::sync::RwLockReadGuard<'_, AppConfig> {
        self.inner.read().expect("config lock poisoned")
    }

    pub fn write(&self) -> std::sync::RwLockWriteGuard<'_, AppConfig> {
        self.inner.write().expect("config lock poisoned")
    }
}
