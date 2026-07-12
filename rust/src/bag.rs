use std::collections::HashMap;
use std::fs::File;
use std::io::BufWriter;
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

use anyhow::{Context, Result, anyhow};
use synapse_fbs::mcap::container as mcap;
use synapse_fbs::mcap::container::records::Record;
use synapse_fbs::mcap::{TimeBasis, TopicChannel, Writer};
use synapse_fbs::topic_catalog::{
    MCAP_MESSAGE_ENCODING, MCAP_METADATA_NAME, MCAP_PROFILE, MCAP_SCHEMA_ENCODING,
    MCAP_SCHEMA_SET_HASH_KEY, MCAP_SESSION_ID_KEY, MCAP_SOURCE_KEY, MCAP_TIME_BASIS_CORRELATED,
    MCAP_TIME_BASIS_KEY, MCAP_TIME_BASIS_MONOTONIC_BOOT, MCAP_TIME_BASIS_UNIX_EPOCH,
    MCAP_TOPIC_ID_KEY,
};

use crate::types::TopicType;

/// Required file-level metadata from the frozen `synapse/1` profile.
#[derive(Debug)]
pub struct ProfileMetadata {
    pub library: String,
    pub schema_set_hash: String,
    pub session_id: String,
    pub source: String,
    pub time_basis: String,
}

/// Writes Synapse samples through the canonical `synapse_fbs` MCAP writer.
/// Each Zenoh channel is registered against the catalog's rooted schema and
/// fixed-layout wire structs are wrapped before being logged.
pub struct BagWriter {
    writer: Option<Writer<BufWriter<File>>>,
    channels: HashMap<String, TopicChannel>,
}

impl BagWriter {
    pub fn create(path: &Path, library: &str, source: &str) -> Result<Self> {
        let file = File::create(path)
            .with_context(|| format!("failed to create bag {}", path.display()))?;
        let session_id = uuid::Uuid::new_v4().simple().to_string();
        let writer = Writer::new(
            BufWriter::new(file),
            library,
            &session_id,
            source,
            TimeBasis::UnixEpoch,
        )
        .context("failed to start Synapse MCAP file")?;
        Ok(Self {
            writer: Some(writer),
            channels: HashMap::new(),
        })
    }

    pub fn write_sample(
        &mut self,
        key: &str,
        known_type: TopicType,
        log_time_ns: u64,
        payload: &[u8],
    ) -> Result<()> {
        let writer = self
            .writer
            .as_mut()
            .ok_or_else(|| anyhow!("bag writer is already finished"))?;
        if !self.channels.contains_key(key) {
            let channel = writer
                .add_topic(known_type.topic, key)
                .with_context(|| format!("failed to add Synapse MCAP channel {key}"))?;
            self.channels.insert(key.to_owned(), channel);
        }

        let channel = self.channels.get_mut(key).expect("channel was inserted");
        let publish_time_ns = known_type.mcap_publish_time_ns(payload, log_time_ns)?;
        let data = known_type.to_mcap_payload(payload)?;
        writer
            .write(channel, log_time_ns, publish_time_ns, &data)
            .with_context(|| format!("failed to write Synapse MCAP sample on {key}"))?;
        Ok(())
    }

    pub fn finish(mut self) -> Result<()> {
        self.writer
            .take()
            .expect("bag writer is present")
            .finish()
            .context("failed to finish Synapse MCAP file")?;
        Ok(())
    }
}

/// Reads a bag fully into memory; MCAP needs random access for chunked files.
pub fn read_bag(path: &Path) -> Result<Vec<u8>> {
    std::fs::read(path).with_context(|| format!("failed to read bag {}", path.display()))
}

/// Validate and return the required file metadata for `synapse/1`.
pub fn profile_metadata(contents: &[u8]) -> Result<ProfileMetadata> {
    let mut records = mcap::read::LinearReader::new(contents).context("failed to open MCAP")?;
    let header = match records
        .next()
        .transpose()
        .context("failed to read MCAP header")?
    {
        Some(Record::Header(header)) => header,
        _ => return Err(anyhow!("MCAP file does not begin with a header")),
    };
    if header.profile != MCAP_PROFILE {
        return Err(anyhow!(
            "unsupported MCAP profile '{}'; expected {MCAP_PROFILE}",
            header.profile
        ));
    }
    if header.library.is_empty() {
        return Err(anyhow!("Synapse MCAP library identifier is empty"));
    }

    for record in records {
        match record.context("failed to read MCAP profile metadata")? {
            Record::Metadata(metadata) if metadata.name == MCAP_METADATA_NAME => {
                let required = |key: &str| {
                    metadata
                        .metadata
                        .get(key)
                        .filter(|value| !value.is_empty())
                        .cloned()
                        .ok_or_else(|| anyhow!("Synapse MCAP metadata is missing {key}"))
                };
                let schema_set_hash = required(MCAP_SCHEMA_SET_HASH_KEY)?;
                let session_id = required(MCAP_SESSION_ID_KEY)?;
                let source = required(MCAP_SOURCE_KEY)?;
                let time_basis = required(MCAP_TIME_BASIS_KEY)?;
                if !is_lowercase_hex_128(&schema_set_hash) {
                    return Err(anyhow!("invalid Synapse MCAP schema-set hash"));
                }
                if !is_lowercase_hex_128(&session_id) {
                    return Err(anyhow!("invalid Synapse MCAP session id"));
                }
                if !matches!(
                    time_basis.as_str(),
                    MCAP_TIME_BASIS_MONOTONIC_BOOT
                        | MCAP_TIME_BASIS_UNIX_EPOCH
                        | MCAP_TIME_BASIS_CORRELATED
                ) {
                    return Err(anyhow!("invalid Synapse MCAP time basis '{time_basis}'"));
                }
                return Ok(ProfileMetadata {
                    library: header.library,
                    schema_set_hash,
                    session_id,
                    source,
                    time_basis,
                });
            }
            Record::Message { .. } | Record::Chunk { .. } => {
                return Err(anyhow!(
                    "Synapse MCAP metadata must precede the first message"
                ));
            }
            _ => {}
        }
    }
    Err(anyhow!("Synapse MCAP metadata record is missing"))
}

/// Validate a channel against the exact catalog and BFBS embedded in this
/// build, returning the matching topic for payload conversion.
pub fn channel_type(channel: &mcap::Channel<'_>) -> Result<TopicType> {
    let schema = channel.schema.as_ref().ok_or_else(|| {
        anyhow!(
            "MCAP channel {} has no required Synapse schema",
            channel.topic
        )
    })?;
    let known_type = TopicType::find(&schema.name)
        .ok_or_else(|| anyhow!("unknown MCAP Synapse type {}", schema.name))?;
    let expected_topic_id = known_type.topic.id.to_string();
    if schema.name != known_type.topic.mcap_schema_name
        || schema.encoding != MCAP_SCHEMA_ENCODING
        || schema.data.as_ref() != known_type.mcap_schema().bfbs
        || channel.message_encoding != MCAP_MESSAGE_ENCODING
        || channel.metadata.get(MCAP_TOPIC_ID_KEY) != Some(&expected_topic_id)
    {
        return Err(anyhow!(
            "MCAP channel {} has an incompatible Synapse schema contract",
            channel.topic
        ));
    }
    Ok(known_type)
}

pub fn unix_now_ns() -> Result<u64> {
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .context("system time is before Unix epoch")?;
    Ok(now.as_nanos().min(u128::from(u64::MAX)) as u64)
}

fn is_lowercase_hex_128(value: &str) -> bool {
    value.len() == 32
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn writes_synapse_profile_mcap_bags() {
        let dir = std::env::temp_dir();
        let path = dir.join(format!("csyn-test-{}.mcap", std::process::id()));
        let known = TopicType::find("VehicleHealth").unwrap();
        let mut payload = vec![0_u8; known.topic.payload_size.unwrap()];
        payload[0..8].copy_from_slice(&42_u64.to_le_bytes());

        {
            let mut writer = BagWriter::create(&path, "csyn/test", "test-source").unwrap();
            writer
                .write_sample(known.topic.key, known, 1_000, &payload)
                .unwrap();
            writer
                .write_sample(known.topic.key, known, 2_000, &payload)
                .unwrap();
            writer.finish().unwrap();
        }

        let contents = read_bag(&path).unwrap();
        let metadata = profile_metadata(&contents).unwrap();
        assert_eq!(metadata.library, "csyn/test");
        assert_eq!(metadata.source, "test-source");
        assert_eq!(metadata.time_basis, MCAP_TIME_BASIS_UNIX_EPOCH);

        let messages: Vec<_> = mcap::MessageStream::new(&contents)
            .unwrap()
            .collect::<mcap::McapResult<_>>()
            .unwrap();
        assert_eq!(messages.len(), 2);
        assert_eq!(messages[0].sequence, 0);
        assert_eq!(messages[1].sequence, 1);
        assert_eq!(messages[0].publish_time, 42_000);
        let schema = messages[0].channel.schema.as_ref().unwrap();
        assert_eq!(schema.name, "synapse.topic.VehicleHealth");
        assert_eq!(schema.data.as_ref(), known.mcap_schema().bfbs);
        assert_eq!(
            messages[0].channel.metadata[MCAP_TOPIC_ID_KEY],
            known.topic.id.to_string()
        );
        assert_eq!(
            known
                .mcap_to_zenoh_payload(&messages[0].data)
                .unwrap()
                .as_ref(),
            payload
        );
        assert!(
            flatbuffers::root::<synapse_fbs::topic::VehicleHealth<'_>>(&messages[0].data).is_ok()
        );

        let _ = std::fs::remove_file(path);
    }
}
