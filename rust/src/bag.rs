use std::collections::{BTreeMap, HashMap};
use std::fs::File;
use std::io::BufWriter;
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

use anyhow::{Context, Result, anyhow};
use mcap::records::MessageHeader;

use crate::types::{SCHEMA_ENCODING_FLATBUFFER, TopicType};

/// Writes Synapse samples to an MCAP file. Each topic becomes a channel;
/// schema records carry the wire type name and the embedded binary schema
/// from the pinned synapse_fbs release, so bags are self-describing.
pub struct BagWriter {
    writer: mcap::Writer<BufWriter<File>>,
    channels: HashMap<String, u16>,
    sequences: HashMap<u16, u32>,
}

impl BagWriter {
    pub fn create(path: &Path, library: &str) -> Result<Self> {
        let file = File::create(path)
            .with_context(|| format!("failed to create bag {}", path.display()))?;
        let writer = mcap::WriteOptions::new()
            .profile("")
            .library(library)
            .create(BufWriter::new(file))
            .context("failed to start MCAP file")?;
        Ok(Self {
            writer,
            channels: HashMap::new(),
            sequences: HashMap::new(),
        })
    }

    pub fn write_sample(
        &mut self,
        key: &str,
        known_type: Option<TopicType>,
        log_time_ns: u64,
        payload: &[u8],
    ) -> Result<()> {
        let channel_id = match self.channels.get(key) {
            Some(&channel_id) => channel_id,
            None => {
                let (schema_id, message_encoding) = match known_type {
                    Some(known) => {
                        let wire_type = known
                            .wire_type()
                            .ok_or_else(|| anyhow!("{} has no wire type", known.topic.name))?;
                        let schema_id = self.writer.add_schema(
                            wire_type,
                            SCHEMA_ENCODING_FLATBUFFER,
                            known.schema.bfbs,
                        )?;
                        (schema_id, known.message_encoding())
                    }
                    None => (0, ""),
                };
                let channel_id =
                    self.writer
                        .add_channel(schema_id, key, message_encoding, &BTreeMap::new())?;
                self.channels.insert(key.to_string(), channel_id);
                channel_id
            }
        };

        let sequence = self.sequences.entry(channel_id).or_insert(0);
        self.writer.write_to_known_channel(
            &MessageHeader {
                channel_id,
                sequence: *sequence,
                log_time: log_time_ns,
                publish_time: log_time_ns,
            },
            payload,
        )?;
        *sequence = sequence.wrapping_add(1);
        Ok(())
    }

    pub fn finish(&mut self) -> Result<()> {
        self.writer.finish().context("failed to finish bag")?;
        Ok(())
    }
}

/// Reads a bag fully into memory; MCAP needs random access for the summary
/// section and chunked messages.
pub fn read_bag(path: &Path) -> Result<Vec<u8>> {
    std::fs::read(path).with_context(|| format!("failed to read bag {}", path.display()))
}

pub fn unix_now_ns() -> Result<u64> {
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .context("system time is before Unix epoch")?;
    Ok(now.as_nanos().min(u128::from(u64::MAX)) as u64)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn writes_and_reads_mcap_bags() {
        let dir = std::env::temp_dir();
        let path = dir.join(format!("csyn-test-{}.mcap", std::process::id()));
        let known = TopicType::find("VehicleHealth").unwrap();
        let payload = vec![0_u8; known.topic.payload_size.unwrap()];

        {
            let mut writer = BagWriter::create(&path, "csyn test").unwrap();
            writer
                .write_sample(known.topic.key, Some(known), 1_000, &payload)
                .unwrap();
            writer
                .write_sample(known.topic.key, Some(known), 2_000, &payload)
                .unwrap();
            writer
                .write_sample("raw/key", None, 3_000, &[1, 2, 3])
                .unwrap();
            writer.finish().unwrap();
        }

        let contents = read_bag(&path).unwrap();
        let summary = mcap::read::Summary::read(&contents).unwrap().unwrap();
        assert_eq!(summary.channels.len(), 2);
        assert_eq!(summary.schemas.len(), 1);
        let stats = summary.stats.unwrap();
        assert_eq!(stats.message_count, 3);

        let messages: Vec<_> = mcap::MessageStream::new(&contents)
            .unwrap()
            .collect::<mcap::McapResult<_>>()
            .unwrap();
        assert_eq!(messages.len(), 3);
        let schema = messages[0].channel.schema.as_ref().unwrap();
        assert_eq!(schema.name, "synapse.topic.VehicleHealthData");
        assert_eq!(schema.data.as_ref(), known.schema.bfbs);

        let _ = std::fs::remove_file(path);
    }
}
