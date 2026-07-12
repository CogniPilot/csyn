use std::borrow::Cow;

use anyhow::{Result, anyhow};
use synapse_fbs::schemas::{self, EmbeddedSchema};
use synapse_fbs::topic_catalog::{self, TopicInfo};
use synapse_fbs::topic_decode;
use synapse_fbs::value_contract;
use zenoh::bytes::Encoding;

/// A catalog topic paired with the embedded schema file that defines it. All
/// metadata resolves against the synapse_fbs release this binary was built
/// with, so the wire contract is pinned by the crate, not vendored copies.
#[derive(Clone, Copy, Debug)]
pub struct TopicType {
    pub topic: &'static TopicInfo,
    pub schema: &'static EmbeddedSchema,
}

impl TopicType {
    fn from_topic(topic: &'static TopicInfo) -> Self {
        let schema = schemas::schema_by_name(topic.schema_file)
            .expect("catalog topics reference an embedded schema");
        Self { topic, schema }
    }

    pub fn all() -> impl Iterator<Item = TopicType> {
        topic_catalog::TOPICS.iter().map(Self::from_topic)
    }

    /// Find a topic by catalog name, canonical key, wire type, or any
    /// namespaced/instance-suffixed key expression.
    pub fn find(name: &str) -> Option<TopicType> {
        topic_catalog::TOPICS
            .iter()
            .find(|topic| {
                topic.name.eq_ignore_ascii_case(name)
                    || topic.key == name
                    || topic.root_table.eq_ignore_ascii_case(name)
            })
            .map(Self::from_topic)
            .or_else(|| Self::infer(name))
            .or_else(|| {
                Self::all().find(|known| {
                    known
                        .wire_type()
                        .is_some_and(|wire| wire.eq_ignore_ascii_case(name))
                        || known.topic.mcap_schema_name.eq_ignore_ascii_case(name)
                })
            })
    }

    pub fn require(name: &str) -> Result<TopicType> {
        Self::find(name).ok_or_else(|| anyhow!("unknown Synapse topic '{name}'"))
    }

    /// Infer the topic from a zenoh key expression, including namespaced and
    /// instance-suffixed keys.
    pub fn infer(keyexpr: &str) -> Option<TopicType> {
        topic_catalog::topic_by_key(keyexpr).map(Self::from_topic)
    }

    /// Resolve a topic exclusively from its required Zenoh value contract.
    /// The contract must exactly match this csyn build's embedded schema.
    pub fn from_value_encoding(encoding: &Encoding) -> Result<TopicType> {
        let received = encoding.to_string();
        let topic =
            value_contract::topic_for_encoding(&received).map_err(|error| anyhow!(error))?;
        Ok(Self::from_topic(topic))
    }

    /// Fully qualified FlatBuffers type carried on the wire: the bare struct
    /// for fixed-layout topics, the root table otherwise.
    pub fn wire_type(self) -> Option<&'static str> {
        Some(self.topic.wire_type)
    }

    pub fn schema_hash(self) -> &'static str {
        self.topic.schema_hash
    }

    /// Embedded BFBS used by this topic's canonical MCAP root table.
    pub fn mcap_schema(self) -> &'static EmbeddedSchema {
        self.schema
    }

    /// Canonical, mandatory Zenoh encoding and exact schema fingerprint.
    pub fn zenoh_encoding(self) -> Encoding {
        Encoding::from(value_contract::encoding_for_topic(self.topic))
    }

    /// Decode a payload and render it with the generated pretty Debug format.
    pub fn decode(self, payload: &[u8]) -> Result<String> {
        topic_decode::decode_topic_debug(self.topic, payload)
            .map_err(|error| anyhow!("{}: {error}", self.topic.name))
    }

    /// Convert a canonical Zenoh payload into the rooted payload required by
    /// the `synapse/1` MCAP profile. Variable-size topics are already rooted;
    /// fixed-layout topic structs receive their generated one-field wrapper.
    pub fn to_mcap_payload<'a>(self, payload: &'a [u8]) -> Result<Cow<'a, [u8]>> {
        self.decode(payload)?;
        if self.topic.fixed_layout {
            Ok(Cow::Owned(wrap_fixed_payload(payload)?))
        } else {
            Ok(Cow::Borrowed(payload))
        }
    }

    /// Publication timestamp required by `synapse/1`. Fixed-layout structs
    /// place `timestamp_us` first; root tables use the first FlatBuffers slot.
    /// Topics without that field fall back to the logger acceptance time.
    pub fn mcap_publish_time_ns(self, payload: &[u8], log_time_ns: u64) -> Result<u64> {
        self.decode(payload)?;
        let timestamp_us = if self.topic.fixed_layout {
            payload
                .get(0..8)
                .map(|bytes| u64::from_le_bytes(bytes.try_into().expect("slice is eight bytes")))
        } else {
            let root_offset = u32::from_le_bytes(
                payload
                    .get(0..4)
                    .expect("validated FlatBuffer has a root offset")
                    .try_into()
                    .expect("slice is four bytes"),
            ) as usize;
            // SAFETY: decode above verifies the complete topic root table.
            let table = unsafe { flatbuffers::Table::new(payload, root_offset) };
            // SAFETY: slot 4 is either absent or the catalog topic's u64
            // timestamp_us field, as fixed by the Synapse topic contract.
            unsafe { table.get::<u64>(4, None) }
        };
        timestamp_us
            .map(|timestamp| {
                timestamp.checked_mul(1_000).ok_or_else(|| {
                    anyhow!("{} timestamp_us overflows nanoseconds", self.topic.name)
                })
            })
            .transpose()
            .map(|timestamp| timestamp.unwrap_or(log_time_ns))
    }

    /// Recover the canonical Zenoh payload from a `synapse/1` MCAP message.
    pub fn mcap_to_zenoh_payload<'a>(self, payload: &'a [u8]) -> Result<Cow<'a, [u8]>> {
        if self.topic.fixed_layout {
            let expected = self
                .topic
                .payload_size
                .expect("fixed-layout topics have a payload size");
            Ok(Cow::Borrowed(unwrap_fixed_payload(payload, expected)?))
        } else {
            self.decode(payload)?;
            Ok(Cow::Borrowed(payload))
        }
    }
}

fn wrap_fixed_payload(payload: &[u8]) -> Result<Vec<u8>> {
    let object_size = payload
        .len()
        .checked_add(4)
        .ok_or_else(|| anyhow!("fixed payload is too large to wrap"))?;
    if object_size > u16::MAX as usize || payload.len() > u32::MAX as usize - 14 {
        return Err(anyhow!("fixed payload is too large to wrap"));
    }

    let mut output = Vec::with_capacity(payload.len() + 14);
    output.extend_from_slice(&4_u32.to_le_bytes());
    output.extend_from_slice(&(-(object_size as i32)).to_le_bytes());
    output.extend_from_slice(payload);
    output.extend_from_slice(&6_u16.to_le_bytes());
    output.extend_from_slice(&(object_size as u16).to_le_bytes());
    output.extend_from_slice(&4_u16.to_le_bytes());
    Ok(output)
}

fn unwrap_fixed_payload(payload: &[u8], expected: usize) -> Result<&[u8]> {
    let expected_len = expected
        .checked_add(14)
        .ok_or_else(|| anyhow!("fixed MCAP payload size overflow"))?;
    let object_size = expected
        .checked_add(4)
        .ok_or_else(|| anyhow!("fixed MCAP object size overflow"))?;
    if payload.len() != expected_len
        || payload.get(0..4) != Some(4_u32.to_le_bytes().as_slice())
        || payload.get(4..8) != Some((-(object_size as i32)).to_le_bytes().as_slice())
        || payload.get(expected + 8..expected + 10) != Some(6_u16.to_le_bytes().as_slice())
        || payload.get(expected + 10..expected + 12)
            != Some((object_size as u16).to_le_bytes().as_slice())
        || payload.get(expected + 12..expected + 14) != Some(4_u16.to_le_bytes().as_slice())
    {
        return Err(anyhow!(
            "invalid fixed-layout MCAP wrapper ({} bytes, expected {})",
            payload.len(),
            expected_len
        ));
    }
    Ok(&payload[8..expected + 8])
}

/// Expand a bare catalog topic reference into a subscription key expression
/// covering canonical, namespaced, and instance-suffixed keys. Anything with
/// a '/' is passed through as a raw zenoh key expression.
pub fn subscribe_keyexpr(arg: &str) -> String {
    if arg.contains('/') {
        return arg.to_string();
    }
    match TopicType::find(arg) {
        Some(known) => format!("**/{}/**", known.topic.key),
        None => arg.to_string(),
    }
}

/// Resolve a bare catalog topic reference to its canonical publication key.
/// Anything with a '/' is passed through as a raw key.
pub fn publish_key(arg: &str) -> String {
    if arg.contains('/') {
        return arg.to_string();
    }
    match TopicType::find(arg) {
        Some(known) => known.topic.key.to_string(),
        None => arg.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_topics_by_name_key_and_wire_type() {
        assert!(TopicType::find("VehicleHealth").is_some());
        assert!(TopicType::find("health").is_some());
        assert!(TopicType::find("synapse.topic.VehicleHealthData").is_some());
        assert!(TopicType::find("no_such_topic").is_none());
    }

    #[test]
    fn infers_topics_from_key_expressions() {
        let known = TopicType::infer("cub1/att").unwrap();
        assert_eq!(known.topic.name, "AttitudeEstimate");
        assert_eq!(known.schema.file, known.topic.schema_file);

        let instanced = TopicType::infer("cub1/imu/0").unwrap();
        assert_eq!(instanced.topic.name, "InertialSample");
    }

    #[test]
    fn uses_the_0_8_topic_keys() {
        assert_eq!(
            TopicType::infer("qualisys/cub1/odom").unwrap().topic.name,
            "Odometry"
        );
        assert_eq!(
            TopicType::infer("qualisys/cub1/odom_cov")
                .unwrap()
                .topic
                .name,
            "OdometryWithCovariance"
        );
        assert_eq!(
            TopicType::find("MocapPoseFrame").unwrap().topic.key,
            "mocap"
        );
        assert_eq!(
            TopicType::infer("qualisys/mocap").unwrap().topic.name,
            "MocapPoseFrame"
        );
    }

    #[test]
    fn requires_an_exact_zenoh_value_contract() {
        let known = TopicType::find("VehicleHealth").unwrap();
        let encoding = known.zenoh_encoding();
        assert_eq!(
            TopicType::from_value_encoding(&encoding)
                .unwrap()
                .topic
                .name,
            "VehicleHealth"
        );

        assert!(TopicType::from_value_encoding(&Encoding::ZENOH_BYTES).is_err());
        let unknown = Encoding::from(
            "application/x-synapse-struct;type=synapse.topic.UnknownData;schema=sha256-128:00000000000000000000000000000000",
        );
        assert!(
            TopicType::from_value_encoding(&unknown)
                .unwrap_err()
                .to_string()
                .contains("unknown Synapse wire type")
        );
        let mismatched = Encoding::from(
            encoding
                .to_string()
                .replace("schema=sha256-128:", "schema=sha256-128:deadbeef"),
        );
        assert!(TopicType::from_value_encoding(&mismatched).is_err());
        let extra = Encoding::from(format!("{encoding};extra=not-allowed"));
        assert!(TopicType::from_value_encoding(&extra).is_err());
    }

    #[test]
    fn reads_table_publication_timestamps_for_mcap() {
        let mut builder = flatbuffers::FlatBufferBuilder::new();
        let text = builder.create_string("ready");
        let root = synapse_fbs::topic::TextStatus::create(
            &mut builder,
            &synapse_fbs::topic::TextStatusArgs {
                timestamp_us: 123,
                text: Some(text),
                ..Default::default()
            },
        );
        builder.finish(root, None);

        let known = TopicType::find("TextStatus").unwrap();
        assert_eq!(
            known
                .mcap_publish_time_ns(builder.finished_data(), 999)
                .unwrap(),
            123_000
        );
    }

    #[test]
    fn expands_bare_names_to_key_expressions() {
        assert_eq!(subscribe_keyexpr("VehicleHealth"), "**/health/**");
        assert_eq!(subscribe_keyexpr("health"), "**/health/**");
        assert_eq!(subscribe_keyexpr("a/b/**"), "a/b/**");
        assert_eq!(publish_key("VehicleHealth"), "health");
        assert_eq!(publish_key("health"), "health");
    }

    #[test]
    fn every_topic_has_schema_and_wire_type() {
        for known in TopicType::all() {
            assert!(!known.schema.bfbs.is_empty(), "{}", known.topic.name);
            assert!(known.wire_type().is_some(), "{}", known.topic.name);
            assert_eq!(known.schema_hash().len(), 32, "{}", known.topic.name);
        }
    }

    #[test]
    fn decodes_fixed_layout_payloads() {
        let known = TopicType::find("ControlLoopMetrics").unwrap();
        let payload = vec![0_u8; known.topic.payload_size.unwrap()];
        let rendered = known.decode(&payload).unwrap();
        assert!(rendered.contains("timestamp_us"));
        assert!(known.decode(&payload[1..]).is_err());
    }
}
