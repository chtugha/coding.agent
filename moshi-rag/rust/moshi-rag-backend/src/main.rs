use anyhow::Result;
use clap::Parser;
use std::str::FromStr;
use std::sync::Arc;

mod action;
mod config;
mod server;
mod text_stream;
mod tomedo_client;

#[derive(Parser, Debug)]
#[clap(name = "moshi-rag-backend", about = "moshi-rag backend service")]
struct Args {
    #[clap(long)]
    config: String,

    #[clap(short = 'l', long = "log", default_value = "info")]
    log_level: String,
}

fn tracing_init(
    log_dir: &str,
    instance_name: &str,
    log_level: &str,
) -> Result<tracing_appender::non_blocking::WorkerGuard> {
    use tracing_subscriber::prelude::*;

    let file_appender = tracing_appender::rolling::daily(log_dir, format!("log.{instance_name}"));
    let (non_blocking, guard) = tracing_appender::non_blocking(file_appender);
    let filter = tracing_subscriber::filter::LevelFilter::from_str(log_level)?;
    let layers = vec![
        tracing_subscriber::fmt::layer()
            .with_writer(non_blocking)
            .with_filter(filter)
            .boxed(),
        tracing_subscriber::fmt::layer()
            .with_writer(std::io::stdout)
            .with_filter(filter)
            .boxed(),
    ];
    tracing_subscriber::registry().with(layers).init();
    Ok(guard)
}

#[tokio::main(flavor = "multi_thread")]
async fn main() -> Result<()> {
    let args = Args::parse();
    let app_config = config::AppConfig::load(&args.config)?;

    std::fs::create_dir_all(&app_config.log_dir)?;

    let _guard = tracing_init(&app_config.log_dir, &app_config.instance_name, &args.log_level)?;

    let listen_addr = format!("{}:{}", app_config.listen_addr, app_config.listen_port);

    tracing::info!(
        "starting moshi-rag-backend on {} (llm_mode={}, arc_mode={})",
        listen_addr,
        app_config.llm_mode_enabled,
        app_config.arc_mode_enabled,
    );

    let shared_config = config::SharedConfig::new(app_config);

    let (action_tx, action_rx) = tokio::sync::mpsc::channel::<text_stream::ActionRequest>(256);

    let tomedo = {
        let cfg = shared_config.read();
        cfg.tomedo_crawl_url.as_deref().map(|url| {
            Arc::new(tomedo_client::TomedoClient::new(url))
        })
    };

    let dispatcher = Arc::new(action::ActionDispatcher::new(
        shared_config.clone(),
        tomedo.clone(),
    ));

    let app_state = Arc::new(server::AppState {
        config: shared_config,
        action_tx,
        http_client: reqwest::Client::new(),
        tomedo_client: tomedo,
        action_dispatcher: dispatcher.clone(),
    });

    tokio::spawn(action::run_action_loop(action_rx, dispatcher));

    let router = server::create_router(app_state);
    let listener = tokio::net::TcpListener::bind(&listen_addr).await?;
    tracing::info!("listening on {}", listen_addr);
    axum::serve(listener, router).await?;

    Ok(())
}
