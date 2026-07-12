mod bag;
mod cli;
mod contract_warning;
mod graph;
mod types;
mod zenoh_util;

use std::collections::{BTreeMap, BTreeSet};
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
use synapse_fbs::mcap::container as mcap;
use zenoh::Wait;

use crate::bag::BagWriter;
use crate::cli::{
    BagCommand, BagExportFormat, Cli, Command, GraphCommand, TopicCommand, TopicOutput,
    TopicPubArgs, TypeCommand,
};
use crate::contract_warning::ContractWarningThrottle;
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
        TopicCommand::List {
            filter,
            ty,
            duration,
        } => topic_list(connect, filter, ty, duration, shutdown),
        TopicCommand::Echo {
            topic,
            ty,
            output,
            once,
            raw,
        } => topic_echo(connect, topic, ty, output, once, raw, shutdown),
        TopicCommand::Pub(args) => topic_pub(connect, args, shutdown),
        TopicCommand::Info { topic, duration } => topic_info(connect, topic, duration, shutdown),
        TopicCommand::Hz { topic, duration } => topic_hz(connect, topic, duration, shutdown),
        TopicCommand::Bw { topic, duration } => topic_bw(connect, topic, duration, shutdown),
    }
}

fn topic_list(
    connect: String,
    filter: Option<String>,
    ty: Option<String>,
    duration: f64,
    shutdown: Arc<AtomicBool>,
) -> Result<()> {
    let keyexpr = filter
        .as_deref()
        .map(subscribe_keyexpr)
        .unwrap_or_else(|| "**".to_string());
    let type_filter = ty.as_deref().map(TopicType::require).transpose()?;
    let session = open_session(&connect)?;
    let subscriber = session
        .declare_subscriber(keyexpr.clone())
        .wait()
        .map_err(|error| anyhow!("failed to subscribe to {keyexpr}: {error}"))?;
    let deadline = Instant::now() + duration_from_secs(duration)?;
    let mut topics = std::collections::BTreeMap::new();
    let mut warnings = ContractWarningThrottle::default();

    while !shutdown.load(Ordering::Relaxed) && Instant::now() < deadline {
        let Some(sample) = subscriber
            .recv_timeout(Duration::from_millis(100))
            .map_err(|error| anyhow!("failed to receive sample: {error}"))?
        else {
            continue;
        };
        let key = sample.key_expr().to_string();
        let known = match require_value_type(&key, sample.encoding(), None) {
            Ok(known) => known,
            Err(error) => {
                warnings.warn(&key, error);
                continue;
            }
        };
        if type_filter.is_some_and(|expected| expected.topic.id != known.topic.id) {
            continue;
        }
        if let Some(previous) = topics.insert(key.clone(), known.topic.name)
            && previous != known.topic.name
        {
            return Err(anyhow!(
                "topic {key} changed value type from {previous} to {}",
                known.topic.name
            ));
        }
    }

    for (topic, topic_type) in topics {
        println!("{topic} [{topic_type}]");
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
    let mut warnings = ContractWarningThrottle::default();

    while !shutdown.load(Ordering::Relaxed) {
        let Some(sample) = subscriber
            .recv_timeout(Duration::from_millis(100))
            .map_err(|error| anyhow!("failed to receive sample: {error}"))?
        else {
            continue;
        };
        let key = sample.key_expr().to_string();
        let payload = sample.payload().to_bytes().to_vec();
        let known_type = match require_value_type(&key, sample.encoding(), forced_type) {
            Ok(known) => known,
            Err(error) => {
                warnings.warn(&key, error);
                continue;
            }
        };

        if raw {
            println!("{}", hex_dump(&payload));
        } else {
            print_sample(&key, Some(known_type), &payload, output)?;
        }

        if once {
            break;
        }
    }
    Ok(())
}

fn topic_pub(connect: String, args: TopicPubArgs, shutdown: Arc<AtomicBool>) -> Result<()> {
    let TopicPubArgs {
        topic,
        ty,
        file,
        text,
        rate,
        count,
    } = args;
    let payload = match (file, text) {
        (Some(path), None) => fs::read(&path)
            .with_context(|| format!("failed to read payload file {}", path.display()))?,
        (None, Some(text)) => text.into_bytes(),
        (None, None) => return Err(anyhow!("topic pub requires --file or --text")),
        (Some(_), Some(_)) => return Err(anyhow!("use only one of --file or --text")),
    };

    let key = publish_key(&topic);
    let known_type = TopicType::require(&ty)?;
    known_type
        .decode(&payload)
        .with_context(|| format!("payload does not match required type {ty}"))?;
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
            .encoding(known_type.zenoh_encoding())
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
    match stats.known_type {
        Some(known) => {
            println!("Type: {}", known.wire_type().unwrap_or(known.topic.name));
            println!(
                "Schema: {} (synapse_fbs {})",
                known.topic.schema_file,
                synapse_fbs::VERSION
            );
            println!("Encoding: {}", known.zenoh_encoding());
        }
        None => println!("Type: not observed"),
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
    known_type: Option<TopicType>,
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
    let mut known_type = None;
    let mut warnings = ContractWarningThrottle::default();

    while !shutdown.load(Ordering::Relaxed) && Instant::now() < deadline {
        let Some(sample) = subscriber
            .recv_timeout(Duration::from_millis(100))
            .map_err(|error| anyhow!("failed to receive sample: {error}"))?
        else {
            continue;
        };
        let key = sample.key_expr().to_string();
        let sample_type = match require_value_type(&key, sample.encoding(), known_type) {
            Ok(known) => known,
            Err(error) => {
                warnings.warn(&key, error);
                continue;
            }
        };
        known_type = Some(sample_type);
        samples += 1;
        bytes += sample.payload().to_bytes().len() as u64;
    }

    Ok(TopicStats {
        samples,
        bytes,
        elapsed: started.elapsed(),
        known_type,
    })
}

fn require_value_type(
    key: &str,
    encoding: &zenoh::bytes::Encoding,
    expected: Option<TopicType>,
) -> Result<TopicType> {
    let actual = TopicType::from_value_encoding(encoding)
        .with_context(|| format!("topic {key} has no valid Synapse value contract"))?;
    if let Some(expected) = expected
        && expected.topic.id != actual.topic.id
    {
        return Err(anyhow!(
            "topic {key} advertises {}, expected {}",
            actual.topic.name,
            expected.topic.name
        ));
    }
    Ok(actual)
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
            println!("zenoh_value_contract: {}", known.zenoh_encoding());
            println!("schema_hash: sha256-128:{}", known.schema_hash());
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
            source,
            duration,
            max_messages,
        } => bag_record(
            connect,
            BagRecordOptions {
                output,
                keyexpr,
                ty,
                source,
                duration,
                max_messages,
            },
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

struct BagRecordOptions {
    output: PathBuf,
    keyexpr: String,
    ty: Option<String>,
    source: String,
    duration: Option<f64>,
    max_messages: Option<u64>,
}

fn bag_record(connect: String, options: BagRecordOptions, shutdown: Arc<AtomicBool>) -> Result<()> {
    let BagRecordOptions {
        output,
        keyexpr,
        ty,
        source,
        duration,
        max_messages,
    } = options;
    let keyexpr = subscribe_keyexpr(&keyexpr);
    let session = open_session(&connect)?;
    let subscriber = session
        .declare_subscriber(keyexpr.clone())
        .wait()
        .map_err(|error| anyhow!("failed to subscribe to {keyexpr}: {error}"))?;
    let forced_type = ty.as_deref().map(TopicType::require).transpose()?;

    let library = format!("csyn/{}", env!("CARGO_PKG_VERSION"));
    let mut writer = BagWriter::create(&output, &library, &source)?;
    let mut warnings = ContractWarningThrottle::default();
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
        let known_type = match require_value_type(&key, sample.encoding(), forced_type) {
            Ok(known) => known,
            Err(error) => {
                warnings.warn(&key, error);
                continue;
            }
        };
        writer.write_sample(&key, known_type, bag::unix_now_ns()?, &payload)?;
        recorded += 1;
    }

    writer.finish()?;
    eprintln!("recorded {recorded} sample(s) to {}", output.display());
    Ok(())
}

fn bag_info(input: PathBuf) -> Result<()> {
    let contents = bag::read_bag(&input)?;
    let profile = bag::profile_metadata(&contents)?;
    let mut channels = BTreeMap::new();
    let mut schemas = BTreeSet::new();
    let mut message_count = 0_u64;
    let mut start_time = u64::MAX;
    let mut end_time = 0_u64;

    for message in mcap::MessageStream::new(&contents).context("failed to open bag")? {
        let message = message.context("failed to read bag message")?;
        let schema = message
            .channel
            .schema
            .as_ref()
            .expect("validated channels have schemas");
        schemas.insert(schema.id);
        let entry = channels
            .entry(message.channel.id)
            .or_insert_with(|| (message.channel.clone(), 0_u64));
        entry.1 += 1;
        message_count += 1;
        start_time = start_time.min(message.log_time);
        end_time = end_time.max(message.log_time);
    }

    println!("file: {}", input.display());
    println!("format: mcap");
    println!("profile: {}", synapse_fbs::topic_catalog::MCAP_PROFILE);
    println!("library: {}", profile.library);
    println!("source: {}", profile.source);
    println!("session: {}", profile.session_id);
    println!("time basis: {}", profile.time_basis);
    println!("schema set: {}", profile.schema_set_hash);
    println!("schemas: {}", schemas.len());
    println!("channels: {}", channels.len());
    println!("messages: {message_count}");
    if message_count > 0 {
        let duration_ns = end_time.saturating_sub(start_time);
        println!("duration: {:.6} s", duration_ns as f64 / 1e9);
    }
    if !channels.is_empty() {
        println!();
        println!("channels:");
        for (_, (channel, count)) in channels {
            let schema = channel
                .schema
                .as_ref()
                .map(|schema| schema.name.as_str())
                .unwrap_or("unknown");
            println!(
                "  {} [{}] messages={} encoding={} topic_id={}",
                channel.topic,
                schema,
                count,
                channel.message_encoding,
                channel
                    .metadata
                    .get(synapse_fbs::topic_catalog::MCAP_TOPIC_ID_KEY)
                    .map(String::as_str)
                    .unwrap_or("missing")
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
    bag::profile_metadata(&contents)?;
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

        let known_type = bag::channel_type(&message.channel)?;
        let payload = known_type
            .mcap_to_zenoh_payload(&message.data)
            .with_context(|| {
                format!(
                    "MCAP payload on {} does not match {}",
                    message.channel.topic, known_type.topic.name
                )
            })?;
        known_type.decode(&payload)?;

        session
            .put(message.channel.topic.clone(), payload.into_owned())
            .encoding(known_type.zenoh_encoding())
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
    bag::profile_metadata(&contents)?;
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

        let known_type = bag::channel_type(&message.channel)?;
        let payload = known_type
            .mcap_to_zenoh_payload(&message.data)
            .with_context(|| {
                format!(
                    "MCAP payload on {} does not match {}",
                    message.channel.topic, known_type.topic.name
                )
            })?;
        let decoded = Some(known_type.decode(&payload)?);
        let value_contract = known_type.zenoh_encoding().to_string();
        let line = ExportFrame {
            log_time_ns: message.log_time,
            topic: &message.channel.topic,
            wire_type: known_type.wire_type(),
            encoding: known_type.topic.encoding,
            value_contract: &value_contract,
            payload_base64: BASE64.encode(&payload),
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
    wire_type: Option<&'static str>,
    encoding: &'a str,
    value_contract: &'a str,
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
