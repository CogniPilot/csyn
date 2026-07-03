use anyhow::{Result, anyhow};
use synapse_fbs::schemas::{self, EmbeddedSchema};
use synapse_fbs::topic_catalog::{self, TopicInfo};
use synapse_fbs::topic_decode;

/// MCAP well-known schema encoding for flatc binary schemas.
pub const SCHEMA_ENCODING_FLATBUFFER: &str = "flatbuffer";
/// MCAP well-known message encoding for FlatBuffers root-table payloads.
pub const MESSAGE_ENCODING_FLATBUFFER: &str = "flatbuffer";
/// Message encoding for the canonical Synapse bare fixed-layout struct
/// payloads; these carry no root offset, so they are not plain "flatbuffer".
pub const MESSAGE_ENCODING_SYNAPSE_STRUCT: &str = "synapse_struct";

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

    /// Find a topic by catalog name, key suffix, canonical key, wire type,
    /// or any namespaced/instance-suffixed key expression.
    pub fn find(name: &str) -> Option<TopicType> {
        topic_catalog::TOPICS
            .iter()
            .find(|topic| {
                topic.name.eq_ignore_ascii_case(name)
                    || topic.key_suffix == name
                    || topic.root_table.eq_ignore_ascii_case(name)
            })
            .map(Self::from_topic)
            .or_else(|| Self::infer(name))
            .or_else(|| {
                Self::all().find(|known| {
                    known
                        .wire_type()
                        .is_some_and(|wire| wire.eq_ignore_ascii_case(name))
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

    /// Fully qualified FlatBuffers type carried on the wire: the bare struct
    /// for fixed-layout topics, the root table otherwise.
    pub fn wire_type(self) -> Option<&'static str> {
        topic_decode::topic_wire_type(self.topic)
    }

    /// MCAP message encoding for this topic's canonical Zenoh payload.
    pub fn message_encoding(self) -> &'static str {
        if self.topic.fixed_layout {
            MESSAGE_ENCODING_SYNAPSE_STRUCT
        } else {
            MESSAGE_ENCODING_FLATBUFFER
        }
    }

    /// Decode a payload and render it with the generated pretty Debug format.
    pub fn decode(self, payload: &[u8]) -> Result<String> {
        topic_decode::decode_topic_debug(self.topic, payload)
            .map_err(|error| anyhow!("{}: {error}", self.topic.name))
    }
}

/// Expand a bare catalog topic reference into a subscription key expression
/// covering canonical, namespaced, and instance-suffixed keys. Anything with
/// a '/' is passed through as a raw zenoh key expression.
pub fn subscribe_keyexpr(arg: &str) -> String {
    if arg.contains('/') {
        return arg.to_string();
    }
    match TopicType::find(arg) {
        Some(known) => format!("**/{}/**", known.topic.key_suffix),
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
    fn finds_topics_by_name_suffix_and_wire_type() {
        assert!(TopicType::find("VehicleHealth").is_some());
        assert!(TopicType::find("vehicle_health").is_some());
        assert!(TopicType::find("synapse.topic.VehicleHealthData").is_some());
        assert!(TopicType::find("no_such_topic").is_none());
    }

    #[test]
    fn infers_topics_from_key_expressions() {
        let known = TopicType::infer("cub1/synapse/v1/topic/attitude_estimate").unwrap();
        assert_eq!(known.topic.name, "AttitudeEstimate");
        assert_eq!(known.schema.file, known.topic.schema_file);
    }

    #[test]
    fn expands_bare_names_to_key_expressions() {
        assert_eq!(subscribe_keyexpr("vehicle_health"), "**/vehicle_health/**");
        assert_eq!(subscribe_keyexpr("a/b/**"), "a/b/**");
        assert!(publish_key("vehicle_health").ends_with("/vehicle_health"));
    }

    #[test]
    fn every_topic_has_schema_and_wire_type() {
        for known in TopicType::all() {
            assert!(!known.schema.bfbs.is_empty(), "{}", known.topic.name);
            assert!(known.wire_type().is_some(), "{}", known.topic.name);
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
