use std::path::PathBuf;

use clap::{ArgAction, Args, Parser, Subcommand, ValueEnum};

#[derive(Debug, Parser)]
#[command(
    name = "csyn",
    about = "ROS-like Synapse CLI over Zenoh and FlatBuffers",
    arg_required_else_help = true,
    next_line_help = true
)]
pub struct Cli {
    #[command(flatten)]
    pub zenoh: ZenohArgs,

    #[arg(
        short = 'V',
        long = "version",
        global = true,
        action = ArgAction::SetTrue,
        help = "Print csyn and schema build version information"
    )]
    pub version: bool,

    #[command(subcommand)]
    pub command: Option<Command>,
}

#[derive(Debug, Args)]
pub struct ZenohArgs {
    #[arg(
        long = "connect",
        env = "CSYN_CONNECT",
        global = true,
        default_value = "tcp/127.0.0.1:7447",
        help = "Zenoh router endpoint"
    )]
    pub connect: String,
}

#[derive(Debug, Subcommand)]
pub enum Command {
    /// Print csyn and schema build version information.
    BuildInfo,
    /// Inspect and publish Synapse topics.
    #[command(subcommand)]
    Topic(TopicCommand),
    /// Inspect catalog topics and their embedded schemas.
    #[command(subcommand)]
    Type(TypeCommand),
    /// Record, replay, inspect, and export MCAP bags.
    #[command(subcommand)]
    Bag(BagCommand),
    /// Serve a local web graph for observed topics and Zenoh admin data.
    #[command(subcommand)]
    Graph(GraphCommand),
}

#[derive(Debug, Subcommand)]
pub enum TopicCommand {
    /// List topics observed during a short subscription window.
    List {
        #[arg(
            value_name = "TOPIC",
            help = "Only observe this catalog topic or key expression"
        )]
        filter: Option<String>,
        #[arg(
            long = "type",
            value_name = "TYPE",
            help = "Only list topics carrying this catalog type"
        )]
        ty: Option<String>,
        #[arg(long, default_value_t = 2.0, help = "Observation window in seconds")]
        duration: f64,
    },
    /// Subscribe to a topic (catalog name or key expression) and print samples.
    Echo {
        topic: String,
        #[arg(
            long = "type",
            value_name = "TYPE",
            help = "Decode as a known catalog topic"
        )]
        ty: Option<String>,
        #[arg(long, value_enum, default_value_t = TopicOutput::Debug)]
        output: TopicOutput,
        #[arg(long, help = "Print one sample and exit")]
        once: bool,
        #[arg(long, help = "Print raw payload bytes as hex")]
        raw: bool,
    },
    /// Publish a raw payload from a file or text.
    Pub(TopicPubArgs),
    /// Show observed topic stats and inferred type.
    Info {
        topic: String,
        #[arg(long, default_value_t = 2.0)]
        duration: f64,
    },
    /// Estimate topic publish rate.
    Hz {
        topic: String,
        #[arg(long, default_value_t = 5.0)]
        duration: f64,
    },
    /// Estimate topic payload bandwidth.
    Bw {
        topic: String,
        #[arg(long, default_value_t = 5.0)]
        duration: f64,
    },
}

#[derive(Debug, Args)]
pub struct TopicPubArgs {
    pub topic: String,
    #[arg(
        long = "type",
        value_name = "TYPE",
        required = true,
        help = "Required Synapse catalog type for the value contract"
    )]
    pub ty: String,
    #[arg(long, value_name = "PATH", conflicts_with = "text")]
    pub file: Option<PathBuf>,
    #[arg(long, conflicts_with = "file")]
    pub text: Option<String>,
    #[arg(
        long,
        default_value_t = 0.0,
        help = "Publish rate in Hz; 0 publishes once"
    )]
    pub rate: f64,
    #[arg(
        long,
        help = "Number of samples to publish; unlimited with --rate if omitted"
    )]
    pub count: Option<u64>,
}

#[derive(Debug, Clone, Copy, ValueEnum)]
pub enum TopicOutput {
    Debug,
    Json,
}

#[derive(Debug, Subcommand)]
pub enum TypeCommand {
    /// List catalog topics from the embedded synapse_fbs release.
    List,
    /// Show catalog and schema metadata for a topic.
    Show {
        #[arg(value_name = "TYPE")]
        ty: String,
        #[arg(long, help = "Print embedded FBS schema text")]
        fbs: bool,
    },
}

#[derive(Debug, Subcommand)]
pub enum BagCommand {
    /// Record samples from a Zenoh key expression to an MCAP file.
    Record {
        #[arg(value_name = "KEYEXPR")]
        keyexpr: String,
        #[arg(short, long, value_name = "PATH")]
        output: PathBuf,
        #[arg(
            long = "type",
            value_name = "TYPE",
            help = "Force all samples to this catalog topic"
        )]
        ty: Option<String>,
        #[arg(
            long,
            default_value = "csyn",
            help = "Source identity stored in required Synapse MCAP metadata"
        )]
        source: String,
        #[arg(long, help = "Stop after this many seconds")]
        duration: Option<f64>,
        #[arg(long, help = "Stop after this many samples")]
        max_messages: Option<u64>,
    },
    /// Print bag metadata and per-topic summary.
    Info {
        #[arg(value_name = "PATH")]
        input: PathBuf,
    },
    /// Replay bag frames onto Zenoh.
    Play {
        #[arg(value_name = "PATH")]
        input: PathBuf,
        #[arg(long, default_value_t = 1.0, help = "Timing multiplier")]
        rate: f64,
        #[arg(long, help = "Only replay this topic name")]
        topic: Option<String>,
    },
    /// Export bag frames to a portable interchange format.
    Export {
        #[arg(value_name = "PATH")]
        input: PathBuf,
        #[arg(short, long, value_name = "PATH")]
        output: Option<PathBuf>,
        #[arg(long, value_enum, default_value_t = BagExportFormat::Jsonl)]
        format: BagExportFormat,
        #[arg(long, help = "Only export this topic name")]
        topic: Option<String>,
    },
}

#[derive(Debug, Clone, Copy, ValueEnum)]
pub enum BagExportFormat {
    Jsonl,
}

#[derive(Debug, Subcommand)]
pub enum GraphCommand {
    /// Serve a browser-based debugging graph.
    Serve {
        #[arg(
            long,
            default_value = "127.0.0.1:8088",
            help = "HTTP bind address for the graph UI"
        )]
        bind: String,
        #[arg(
            long,
            default_value = "**",
            help = "Zenoh key expression to observe for message traffic"
        )]
        keyexpr: String,
        #[arg(
            long,
            default_value_t = 1000,
            help = "Admin-space polling interval in milliseconds"
        )]
        admin_poll_ms: u64,
    },
}
