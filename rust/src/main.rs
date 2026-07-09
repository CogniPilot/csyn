mod bag;
mod cli;
mod graph;
mod types;
mod zenoh_util;

use std::fs;
use std::io::{BufWriter, Write};
use std::path::PathBuf;
use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};
use std::thread;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, anyhow};
use base64::Engine;
use base64::engine::general_purpose::STANDARD as BASE64;
use clap::{CommandFactory, Parser};
use serde::Serialize;
use zenoh::Wait;

use crate::bag::BagWriter;
use crate::cli::{
    BagCommand, BagExportFormat, Cli, Command, GraphCommand, TopicCommand, TopicOutput, TypeCommand,
};
use crate::types::{TopicType, publish_key, subscribe_keyexpr};
use crate::zenoh_util::open_session;

fn main() -> Result<()> {
    let cli = Cli::parse();
    if cli.version {
        print_version();
        return Ok(());
    }

    let command = match cli.command {
        Some(command) => command,
        None => {
            Cli::command().print_help()?;
            println!();
            return Ok(());
        }
    };
    let connect = cli.zenoh.connect;
    let shutdown = shutdown_flag()?;

    match command {
        Command::BuildInfo => {
            print_version();
            Ok(())
        }
        Command::Topic(command) => run_topic(connect, command, shutdown),
        Command::Type(command) => run_type(command),
        Command::Bag(command) => run_bag(connect, command, shutdown),
        Command::Graph(command) => run_graph(connect, command, shutdown),
    }
}

fn print_version() {
    println!(
        "csyn {} (synapse_fbs {})",
        env!("CARGO_PKG_VERSION"),
        synapse_fbs::VERSION
    );
}

fn run_graph(connect: String, command: GraphCommand, shutdown: Arc<AtomicBool>) -> Result<()> {
    match command {
        GraphCommand::Serve {
            bind,
            keyexpr,
            admin_poll_ms,
        } => graph::serve(
            graph::GraphConfig {
                connect,
                bind,
                keyexpr,
                admin_poll: Duration::from_millis(admin_poll_ms),
            },
            shutdown,
        ),
    }
}

fn run_topic(connect: String, command: TopicCommand, shutdown: Arc<AtomicBool>) -> Result<()> {
    match command {
        TopicCommand::List { duration } => topic_list(connect, duration, shutdown),
        TopicCommand::Echo {
            topic,
            ty,
            output,
            once,
            raw,
        } => topic_echo(connect, topic, ty, output, once, raw, shutdown),
        TopicCommand::Pub {
            topic,
            file,
            text,
            rate,
            count,
        } => topic_pub(connect, topic, file, text, rate, count, shutdown),
        TopicCommand::Info { topic, duration } => topic_info(connect, topic, duration, shutdown),
        TopicCommand::Hz { topic, duration } => topic_hz(connect, topic, duration, shutdown),
        TopicCommand::Bw { topic, duration } => topic_bw(connect, topic, duration, shutdown),
    }
}

fn topic_list(connect: String, duration: f64, shutdown: Arc<AtomicBool>) -> Result<()> {
    let session = open_session(&connect)?;
    let subscriber = session
        .declare_subscriber("**")
        .wait()
        .map_err(|error| anyhow!("failed to subscribe to **: {error}"))?;
    let deadline = Instant::now() + duration_from_secs(duration)?;
    let mut topics = std::collections::BTreeSet::new();

    while !shutdown.load(Ordering::Relaxed) && Instant::now() < deadline {
        let Some(sample) = subscriber
            .recv_timeout(Duration::from_millis(100))
            .map_err(|error| anyhow!("failed to receive sample: {error}"))?
        else {
            continue;
        };
        topics.insert(sample.key_expr().to_string());
    }

    for topic in topics {
        match TopicType::infer(&topic) {
            Some(known) => println!("{topic} [{}]", known.topic.name),
            None => println!("{topic}"),
        }
    }
    Ok(())
}

fn topic_echo(
    connect: String,
    topic: String,
    ty: Option<String>,
    output: TopicOutput,
    once: bool,
    raw: bool,
    shutdown: Arc<AtomicBool>,
) -> Result<()> {
    let keyexpr = subscribe_keyexpr(&topic);
    let session = open_session(&connect)?;
    let subscriber = session
        .declare_subscriber(keyexpr.clone())
        .wait()
        .map_err(|error| anyhow!("failed to subscribe to {keyexpr}: {error}"))?;
    let forced_type = ty.as_deref().map(TopicType::require).transpose()?;

    while !shutdown.load(Ordering::Relaxed) {
        let Some(sample) = subscriber
            .recv_timeout(Duration::from_millis(100))
            .map_err(|error| anyhow!("failed to receive sample: {error}"))?
        else {
            continue;
        };
        let key = sample.key_expr().to_string();
        let payload = sample.payload().to_bytes().to_vec();
        let known_type = forced_type.or_else(|| TopicType::infer(&key));

        if raw {
            println!("{}", hex_dump(&payload));
        } else {
            print_sample(&key, known_type, &payload, output)?;
        }

        if once {
            break;
        }
    }
    Ok(())
}

fn topic_pub(
    connect: String,
    topic: String,
    file: Option<PathBuf>,
    text: Option<String>,
    rate: f64,
    count: Option<u64>,
    shutdown: Arc<AtomicBool>,
) -> Result<()> {
    let payload = match (file, text) {
        (Some(path), None) => fs::read(&path)
            .with_context(|| format!("failed to read payload file {}", path.display()))?,
        (None, Some(text)) => text.into_bytes(),
        (None, None) => return Err(anyhow!("topic pub requires --file or --text")),
        (Some(_), Some(_)) => return Err(anyhow!("use only one of --file or --text")),
    };

    let key = publish_key(&topic);
    let session = open_session(&connect)?;
    let delay = if rate > 0.0 {
        Some(Duration::from_secs_f64(1.0 / rate))
    } else {
        None
    };
    let mut sent = 0_u64;

    loop {
        if shutdown.load(Ordering::Relaxed) {
            break;
        }
        if let Some(limit) = count
            && sent >= limit
        {
            break;
        }

        session
            .put(key.clone(), payload.clone())
            .wait()
            .map_err(|error| anyhow!("failed to publish {key}: {error}"))?;
        sent += 1;

        let Some(delay) = delay else {
            break;
        };
        thread::sleep(delay);
    }

    eprintln!("published {sent} sample(s) on {key}");
    Ok(())
}

fn topic_info(
    connect: String,
    topic: String,
    duration: f64,
    shutdown: Arc<AtomicBool>,
) -> Result<()> {
    let stats = observe_topic(connect, topic.clone(), duration, shutdown)?;
    println!("Topic: {topic}");
    match TopicType::find(&topic) {
        Some(known) => {
            println!("Type: {}", known.wire_type().unwrap_or(known.topic.name));
            println!(
                "Schema: {} (synapse_fbs {})",
                known.topic.schema_file,
                synapse_fbs::VERSION
            );
        }
        None => println!("Type: unknown"),
    }
    println!("Samples: {}", stats.samples);
    println!("Bytes: {}", stats.bytes);
    if stats.samples > 0 {
        println!(
            "Mean size: {:.1} B",
            stats.bytes as f64 / stats.samples as f64
        );
    }
    Ok(())
}

fn topic_hz(
    connect: String,
    topic: String,
    duration: f64,
    shutdown: Arc<AtomicBool>,
) -> Result<()> {
    let stats = observe_topic(connect, topic.clone(), duration, shutdown)?;
    println!("Topic: {topic}");
    println!("Samples: {}", stats.samples);
    println!("Window: {:.3} s", stats.elapsed.as_secs_f64());
    if stats.elapsed.as_secs_f64() > 0.0 {
        println!(
            "Rate: {:.3} Hz",
            stats.samples as f64 / stats.elapsed.as_secs_f64()
        );
    }
    Ok(())
}

fn topic_bw(
    connect: String,
    topic: String,
    duration: f64,
    shutdown: Arc<AtomicBool>,
) -> Result<()> {
    let stats = observe_topic(connect, topic.clone(), duration, shutdown)?;
    println!("Topic: {topic}");
    println!("Samples: {}", stats.samples);
    println!("Bytes: {}", stats.bytes);
    println!("Window: {:.3} s", stats.elapsed.as_secs_f64());
    if stats.elapsed.as_secs_f64() > 0.0 {
        println!(
            "Bandwidth: {:.1} B/s",
            stats.bytes as f64 / stats.elapsed.as_secs_f64()
        );
    }
    Ok(())
}

#[derive(Debug)]
struct TopicStats {
    samples: u64,
    bytes: u64,
    elapsed: Duration,
}

fn observe_topic(
    connect: String,
    topic: String,
    duration: f64,
    shutdown: Arc<AtomicBool>,
) -> Result<TopicStats> {
    let keyexpr = subscribe_keyexpr(&topic);
    let session = open_session(&connect)?;
    let subscriber = session
        .declare_subscriber(keyexpr.clone())
        .wait()
        .map_err(|error| anyhow!("failed to subscribe to {keyexpr}: {error}"))?;
    let requested = duration_from_secs(duration)?;
    let started = Instant::now();
    let deadline = started + requested;
    let mut samples = 0_u64;
    let mut bytes = 0_u64;

    while !shutdown.load(Ordering::Relaxed) && Instant::now() < deadline {
        let Some(sample) = subscriber
            .recv_timeout(Duration::from_millis(100))
            .map_err(|error| anyhow!("failed to receive sample: {error}"))?
        else {
            continue;
        };
        samples += 1;
        bytes += sample.payload().to_bytes().len() as u64;
    }

    Ok(TopicStats {
        samples,
        bytes,
        elapsed: started.elapsed(),
    })
}

fn run_type(command: TypeCommand) -> Result<()> {
    match command {
        TypeCommand::List => {
            println!("synapse_fbs release: {}", synapse_fbs::VERSION);
            println!("{:<24} {:>4} {:>6}  wire type", "topic", "id", "bytes");
            for known in TopicType::all() {
                println!(
                    "{:<24} {:>4} {:>6}  {}",
                    known.topic.name,
                    known.topic.id,
                    known
                        .topic
                        .payload_size
                        .map(|size| size.to_string())
                        .unwrap_or_else(|| "-".to_string()),
                    known.wire_type().unwrap_or("-"),
                );
            }
        }
        TypeCommand::Show { ty, fbs } => {
            let known = TopicType::require(&ty)?;
            println!("topic: {}", known.topic.name);
            println!("id: {}", known.topic.id);
            println!("key: {}", known.topic.key);
            println!("wire_type: {}", known.wire_type().unwrap_or("-"));
            println!("encoding: {}", known.topic.encoding);
            println!(
                "payload_size: {}",
                known
                    .topic
                    .payload_size
                    .map(|size| size.to_string())
                    .unwrap_or_else(|| "-".to_string())
            );
            println!("schema_file: {}", known.topic.schema_file);
            println!(
                "schema_root_type: {}",
                known.schema.root_type.unwrap_or("-")
            );
            println!(
                "schema_file_identifier: {}",
                known.schema.file_identifier.unwrap_or("-")
            );
            println!("bfbs_bytes: {}", known.schema.bfbs.len());
            println!("synapse_fbs: {}", synapse_fbs::VERSION);
            println!("description: {}", known.topic.description);
            if fbs {
                println!("---");
                println!("{}", known.schema.fbs);
            }
        }
    }
    Ok(())
}

fn run_bag(connect: String, command: BagCommand, shutdown: Arc<AtomicBool>) -> Result<()> {
    match command {
        BagCommand::Record {
            output,
            keyexpr,
            ty,
            duration,
            max_messages,
        } => bag_record(
            connect,
            output,
            keyexpr,
            ty,
            duration,
            max_messages,
            shutdown,
        ),
        BagCommand::Info { input } => bag_info(input),
        BagCommand::Play { input, rate, topic } => bag_play(connect, input, rate, topic, shutdown),
        BagCommand::Export {
            input,
            output,
            format,
            topic,
        } => bag_export(input, output, format, topic),
    }
}

fn bag_record(
    connect: String,
    output: PathBuf,
    keyexpr: String,
    ty: Option<String>,
    duration: Option<f64>,
    max_messages: Option<u64>,
    shutdown: Arc<AtomicBool>,
) -> Result<()> {
    let keyexpr = subscribe_keyexpr(&keyexpr);
    let session = open_session(&connect)?;
    let subscriber = session
        .declare_subscriber(keyexpr.clone())
        .wait()
        .map_err(|error| anyhow!("failed to subscribe to {keyexpr}: {error}"))?;
    let forced_type = ty.as_deref().map(TopicType::require).transpose()?;

    let library = format!(
        "csyn {} (synapse_fbs {})",
        env!("CARGO_PKG_VERSION"),
        synapse_fbs::VERSION
    );
    let mut writer = BagWriter::create(&output, &library)?;
    let started = Instant::now();
    let deadline = duration
        .map(duration_from_secs)
        .transpose()?
        .map(|duration| started + duration);
    let mut recorded = 0_u64;

    while !shutdown.load(Ordering::Relaxed) {
        if let Some(deadline) = deadline
            && Instant::now() >= deadline
        {
            break;
        }
        if let Some(limit) = max_messages
            && recorded >= limit
        {
            break;
        }

        let Some(sample) = subscriber
            .recv_timeout(Duration::from_millis(100))
            .map_err(|error| anyhow!("failed to receive sample: {error}"))?
        else {
            continue;
        };

        let key = sample.key_expr().to_string();
        let payload = sample.payload().to_bytes().to_vec();
        let known_type = forced_type.or_else(|| TopicType::infer(&key));
        writer.write_sample(&key, known_type, bag::unix_now_ns()?, &payload)?;
        recorded += 1;
    }

    writer.finish()?;
    eprintln!("recorded {recorded} sample(s) to {}", output.display());
    Ok(())
}

fn bag_info(input: PathBuf) -> Result<()> {
    let contents = bag::read_bag(&input)?;
    let summary = mcap::read::Summary::read(&contents)
        .context("failed to read bag summary")?
        .ok_or_else(|| anyhow!("{} has no MCAP summary section", input.display()))?;

    println!("file: {}", input.display());
    println!("format: mcap");
    println!("schemas: {}", summary.schemas.len());
    println!("channels: {}", summary.channels.len());
    if let Some(stats) = &summary.stats {
        println!("messages: {}", stats.message_count);
        let duration_ns = stats
            .message_end_time
            .saturating_sub(stats.message_start_time);
        if stats.message_count > 0 {
            println!("duration: {:.6} s", duration_ns as f64 / 1e9);
        }
    }
    if !summary.channels.is_empty() {
        println!();
        println!("channels:");
        let mut channels: Vec<_> = summary.channels.iter().collect();
        channels.sort_by_key(|(id, _)| **id);
        for (id, channel) in channels {
            let schema = channel
                .schema
                .as_ref()
                .map(|schema| schema.name.as_str())
                .unwrap_or("unknown");
            let count = summary
                .stats
                .as_ref()
                .and_then(|stats| stats.channel_message_counts.get(id))
                .copied()
                .unwrap_or(0);
            println!(
                "  {} [{}] messages={} encoding={}",
                channel.topic, schema, count, channel.message_encoding
            );
        }
    }
    Ok(())
}

fn bag_play(
    connect: String,
    input: PathBuf,
    rate: f64,
    topic_filter: Option<String>,
    shutdown: Arc<AtomicBool>,
) -> Result<()> {
    if rate <= 0.0 {
        return Err(anyhow!("--rate must be positive"));
    }

    let session = open_session(&connect)?;
    let contents = bag::read_bag(&input)?;
    let mut previous_ns = None;
    let mut played = 0_u64;

    for message in mcap::MessageStream::new(&contents).context("failed to open bag")? {
        let message = message.context("failed to read bag message")?;
        if topic_filter
            .as_deref()
            .is_some_and(|filter| filter != message.channel.topic)
        {
            continue;
        }
        if shutdown.load(Ordering::Relaxed) {
            break;
        }

        if let Some(previous_ns) = previous_ns {
            let delta_ns = message.log_time.saturating_sub(previous_ns);
            let scaled = Duration::from_secs_f64(delta_ns as f64 / 1e9 / rate);
            thread::sleep(scaled);
        }
        previous_ns = Some(message.log_time);

        session
            .put(message.channel.topic.clone(), message.data.to_vec())
            .wait()
            .map_err(|error| anyhow!("failed to publish {}: {error}", message.channel.topic))?;
        played += 1;
    }

    eprintln!("played {played} message(s) from {}", input.display());
    Ok(())
}

fn bag_export(
    input: PathBuf,
    output: Option<PathBuf>,
    format: BagExportFormat,
    topic_filter: Option<String>,
) -> Result<()> {
    match format {
        BagExportFormat::Jsonl => bag_export_jsonl(input, output, topic_filter),
    }
}

fn bag_export_jsonl(
    input: PathBuf,
    output: Option<PathBuf>,
    topic_filter: Option<String>,
) -> Result<()> {
    let contents = bag::read_bag(&input)?;
    let writer: Box<dyn Write> = match output {
        Some(path) => Box::new(
            fs::File::create(&path)
                .with_context(|| format!("failed to create export output {}", path.display()))?,
        ),
        None => Box::new(std::io::stdout()),
    };
    let mut writer = BufWriter::new(writer);

    for message in mcap::MessageStream::new(&contents).context("failed to open bag")? {
        let message = message.context("failed to read bag message")?;
        if topic_filter
            .as_deref()
            .is_some_and(|filter| filter != message.channel.topic)
        {
            continue;
        }

        let known_type = TopicType::infer(&message.channel.topic);
        let decoded = known_type.and_then(|known| known.decode(&message.data).ok());
        let line = ExportFrame {
            log_time_ns: message.log_time,
            topic: &message.channel.topic,
            wire_type: message
                .channel
                .schema
                .as_ref()
                .map(|schema| schema.name.clone()),
            encoding: &message.channel.message_encoding,
            payload_base64: BASE64.encode(&message.data),
            decoded,
        };
        serde_json::to_writer(&mut writer, &line)?;
        writer.write_all(b"\n")?;
    }

    writer.flush()?;
    Ok(())
}

#[derive(Serialize)]
struct ExportFrame<'a> {
    log_time_ns: u64,
    topic: &'a str,
    wire_type: Option<String>,
    encoding: &'a str,
    payload_base64: String,
    decoded: Option<String>,
}

fn print_sample(
    topic: &str,
    known_type: Option<TopicType>,
    payload: &[u8],
    output: TopicOutput,
) -> Result<()> {
    match output {
        TopicOutput::Debug => {
            println!("topic: {topic}");
            if let Some(known_type) = known_type {
                println!(
                    "type: {}",
                    known_type.wire_type().unwrap_or(known_type.topic.name)
                );
                match known_type.decode(payload) {
                    Ok(decoded) => println!("{decoded}"),
                    Err(error) => println!("decode_error: {error}"),
                }
            } else {
                println!("type: unknown");
                println!("payload: {}", hex_dump(payload));
            }
            println!("---");
        }
        TopicOutput::Json => {
            let decoded = known_type.and_then(|known_type| known_type.decode(payload).ok());
            let line = EchoJson {
                topic,
                wire_type: known_type.and_then(|known_type| known_type.wire_type()),
                payload_base64: BASE64.encode(payload),
                decoded,
            };
            println!("{}", serde_json::to_string(&line)?);
        }
    }
    Ok(())
}

#[derive(Serialize)]
struct EchoJson<'a> {
    topic: &'a str,
    wire_type: Option<&'static str>,
    payload_base64: String,
    decoded: Option<String>,
}

fn duration_from_secs(seconds: f64) -> Result<Duration> {
    if seconds < 0.0 || !seconds.is_finite() {
        return Err(anyhow!("duration must be a non-negative finite number"));
    }
    Ok(Duration::from_secs_f64(seconds))
}

fn shutdown_flag() -> Result<Arc<AtomicBool>> {
    let shutdown = Arc::new(AtomicBool::new(false));
    let signal_shutdown = shutdown.clone();
    ctrlc::set_handler(move || {
        signal_shutdown.store(true, Ordering::Relaxed);
    })
    .context("failed to install Ctrl-C handler")?;
    Ok(shutdown)
}

fn hex_dump(bytes: &[u8]) -> String {
    bytes
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect::<Vec<_>>()
        .join(" ")
}
