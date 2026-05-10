use std::sync::Arc;
use std::time::Duration;

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize)]
struct EmbeddingQueryRequest {
    text: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    patient_id: Option<i64>,
    top_k: usize,
}

#[derive(Debug, Clone, Deserialize)]
pub struct EmbeddingResult {
    pub text: String,
    #[serde(default)]
    pub source: Option<String>,
    #[serde(default)]
    pub score: Option<f64>,
    #[serde(default)]
    pub patient_id: Option<i64>,
}

#[derive(Debug, Clone, Deserialize)]
struct EmbeddingQueryResponse {
    results: Vec<EmbeddingResult>,
}

#[derive(Debug, Clone, Deserialize)]
struct CallerResponse {
    status: String,
    #[serde(default)]
    patient_id: Option<i64>,
}

pub fn format_chunks_into_context(chunks: &[EmbeddingResult], context: &str) -> String {
    if chunks.is_empty() {
        return context.to_string();
    }
    let mut out = String::new();
    out.push_str("[Retrieved medical context]\n");
    for (i, chunk) in chunks.iter().enumerate() {
        out.push_str(&format!("({}) {}", i + 1, chunk.text.trim()));
        if let Some(ref src) = chunk.source {
            out.push_str(&format!(" [source: {}]", src));
        }
        out.push('\n');
    }
    out.push_str("[End of retrieved context]\n\n");
    out.push_str(context);
    out
}

pub async fn fetch_embedding_results(
    http_client: &reqwest::Client,
    embedding_url: &str,
    context: &str,
    patient_id: Option<i64>,
    top_k: usize,
) -> Result<Vec<EmbeddingResult>, Box<dyn std::error::Error + Send + Sync>> {
    let request = EmbeddingQueryRequest {
        text: context.to_string(),
        patient_id,
        top_k,
    };

    tracing::info!(
        "RAG Step A: querying embeddings, context_len={}, patient_id={:?}, top_k={}",
        context.len(),
        patient_id,
        top_k,
    );

    let response = http_client
        .post(embedding_url)
        .json(&request)
        .send()
        .await
        .map_err(|e| -> Box<dyn std::error::Error + Send + Sync> {
            format!("Embedding query failed: {}", e).into()
        })?;

    if !response.status().is_success() {
        let status = response.status();
        let body = response.text().await.unwrap_or_else(|_| "unknown".to_string());
        return Err(format!("Embedding API returned {} - {}", status, body).into());
    }

    let resp: EmbeddingQueryResponse = response.json().await.map_err(|e| -> Box<dyn std::error::Error + Send + Sync> {
        format!("Failed to parse embedding response: {}", e).into()
    })?;

    tracing::info!("RAG Step A: got {} embedding results", resp.results.len());
    Ok(resp.results)
}

pub async fn fetch_patient_id(
    call_id: u32,
) -> Result<Option<i64>, Box<dyn std::error::Error + Send + Sync>> {
    let client = reqwest::Client::builder()
        .danger_accept_invalid_certs(true)
        .timeout(Duration::from_secs(5))
        .build()
        .map_err(|e| -> Box<dyn std::error::Error + Send + Sync> { e.to_string().into() })?;

    let url = format!("https://127.0.0.1:13181/caller/{}", call_id);
    tracing::info!("Fetching patient ID for call_id={}", call_id);

    let response = client
        .get(&url)
        .send()
        .await
        .map_err(|e| -> Box<dyn std::error::Error + Send + Sync> {
            format!("Patient ID fetch failed: {}", e).into()
        })?;

    if !response.status().is_success() {
        let status = response.status();
        return Err(format!("Patient ID API returned {}", status).into());
    }

    let resp: CallerResponse = response.json().await.map_err(|e| -> Box<dyn std::error::Error + Send + Sync> {
        format!("Failed to parse caller response: {}", e).into()
    })?;

    if resp.status == "found" {
        if let Some(pid) = resp.patient_id {
            if pid >= 0 {
                tracing::info!("Patient ID resolved: {} for call_id={}", pid, call_id);
                return Ok(Some(pid));
            }
        }
    }

    tracing::info!("Patient ID not available for call_id={} (status={})", call_id, resp.status);
    Ok(None)
}

pub struct TwoStepRagClient {
    embedding_client: reqwest::Client,
    embedding_url: String,
    top_k: usize,
}

impl TwoStepRagClient {
    pub fn new(rag_timeout: f32) -> Self {
        let timeout = if rag_timeout > 0.0 {
            Duration::from_secs_f64(rag_timeout as f64)
        } else {
            Duration::from_secs(3)
        };

        let embedding_client = reqwest::Client::builder()
            .timeout(timeout)
            .build()
            .unwrap_or_else(|_| reqwest::Client::new());

        Self {
            embedding_client,
            embedding_url: "http://127.0.0.1:8080/api/embeddings/query".to_string(),
            top_k: 5,
        }
    }

    pub async fn retrieve_and_augment_context(
        &self,
        context: &str,
        patient_id: Option<i64>,
    ) -> String {
        match fetch_embedding_results(
            &self.embedding_client,
            &self.embedding_url,
            context,
            patient_id,
            self.top_k,
        )
        .await
        {
            Ok(chunks) if !chunks.is_empty() => {
                let augmented = format_chunks_into_context(&chunks, context);
                tracing::info!("RAG Step A succeeded: {} chunks, augmented context_len={}", chunks.len(), augmented.len());
                augmented
            }
            Ok(_) => {
                tracing::info!("RAG Step A returned no results, using plain context");
                context.to_string()
            }
            Err(e) => {
                tracing::warn!("RAG Step A failed: {}. Falling through to Step B with context only", e);
                context.to_string()
            }
        }
    }
}

pub fn new_client(rag_timeout: f32) -> Arc<TwoStepRagClient> {
    Arc::new(TwoStepRagClient::new(rag_timeout))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn format_chunks_empty() {
        let result = format_chunks_into_context(&[], "user: hello\nmoshi: hi");
        assert_eq!(result, "user: hello\nmoshi: hi");
    }

    #[test]
    fn format_chunks_single_no_source() {
        let chunks = vec![EmbeddingResult {
            text: "Patient has diabetes type 2.".to_string(),
            source: None,
            score: Some(0.95),
            patient_id: Some(42),
        }];
        let result = format_chunks_into_context(&chunks, "user: what diagnosis?");
        assert!(result.starts_with("[Retrieved medical context]\n"));
        assert!(result.contains("(1) Patient has diabetes type 2."));
        assert!(!result.contains("[source:"));
        assert!(result.contains("[End of retrieved context]"));
        assert!(result.ends_with("user: what diagnosis?"));
    }

    #[test]
    fn format_chunks_multiple_with_source() {
        let chunks = vec![
            EmbeddingResult {
                text: "Blood pressure 140/90.".to_string(),
                source: Some("vitals.pdf".to_string()),
                score: Some(0.9),
                patient_id: None,
            },
            EmbeddingResult {
                text: "Prescribed Metoprolol 50mg.".to_string(),
                source: Some("prescriptions.pdf".to_string()),
                score: Some(0.85),
                patient_id: None,
            },
        ];
        let result = format_chunks_into_context(&chunks, "context");
        assert!(result.contains("(1) Blood pressure 140/90. [source: vitals.pdf]"));
        assert!(result.contains("(2) Prescribed Metoprolol 50mg. [source: prescriptions.pdf]"));
    }

    #[test]
    fn embedding_request_omits_patient_id_when_none() {
        let req = EmbeddingQueryRequest {
            text: "test".to_string(),
            patient_id: None,
            top_k: 5,
        };
        let json = serde_json::to_string(&req).unwrap();
        assert!(!json.contains("patient_id"));
        assert!(json.contains("\"text\":\"test\""));
        assert!(json.contains("\"top_k\":5"));
    }

    #[test]
    fn embedding_request_includes_patient_id_when_some() {
        let req = EmbeddingQueryRequest {
            text: "test".to_string(),
            patient_id: Some(42),
            top_k: 5,
        };
        let json = serde_json::to_string(&req).unwrap();
        assert!(json.contains("\"patient_id\":42"));
    }

    #[test]
    fn embedding_request_does_not_send_null() {
        let req = EmbeddingQueryRequest {
            text: "test".to_string(),
            patient_id: None,
            top_k: 5,
        };
        let json = serde_json::to_string(&req).unwrap();
        assert!(!json.contains("null"));
    }

    #[test]
    fn caller_response_found_with_valid_pid() {
        let json = r#"{"status":"found","patient_id":42}"#;
        let resp: CallerResponse = serde_json::from_str(json).unwrap();
        assert_eq!(resp.status, "found");
        assert_eq!(resp.patient_id, Some(42));
    }

    #[test]
    fn caller_response_pending() {
        let json = r#"{"status":"pending","patient_id":-1}"#;
        let resp: CallerResponse = serde_json::from_str(json).unwrap();
        assert_eq!(resp.status, "pending");
        assert_eq!(resp.patient_id, Some(-1));
    }

    #[test]
    fn caller_response_not_found() {
        let json = r#"{"status":"not_found","patient_id":-1}"#;
        let resp: CallerResponse = serde_json::from_str(json).unwrap();
        assert_eq!(resp.status, "not_found");
    }

    #[test]
    fn caller_response_found_patient_id_zero_is_valid() {
        let json = r#"{"status":"found","patient_id":0}"#;
        let resp: CallerResponse = serde_json::from_str(json).unwrap();
        assert_eq!(resp.status, "found");
        assert_eq!(resp.patient_id, Some(0));
        assert!(resp.patient_id.map_or(false, |pid| pid >= 0));
    }

    #[test]
    fn caller_response_found_negative_pid_is_rejected() {
        let json = r#"{"status":"found","patient_id":-1}"#;
        let resp: CallerResponse = serde_json::from_str(json).unwrap();
        assert_eq!(resp.status, "found");
        assert_eq!(resp.patient_id, Some(-1));
        assert!(!resp.patient_id.map_or(false, |pid| pid >= 0));
    }

    #[test]
    fn caller_response_found_missing_patient_id_field() {
        let json = r#"{"status":"found"}"#;
        let resp: CallerResponse = serde_json::from_str(json).unwrap();
        assert_eq!(resp.status, "found");
        assert_eq!(resp.patient_id, None);
    }

    #[test]
    fn format_chunks_trims_trailing_newline_from_chunk_text() {
        let chunks = vec![EmbeddingResult {
            text: "Patient data with trailing newline.\n".to_string(),
            source: None,
            score: None,
            patient_id: None,
        }];
        let result = format_chunks_into_context(&chunks, "user: hello");
        assert!(result.contains("(1) Patient data with trailing newline.\n"));
        assert!(!result.contains("(1) Patient data with trailing newline.\n\n"));
    }
}
