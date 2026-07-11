use std::collections::{BTreeMap, VecDeque};
use std::io::{Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicBool, Ordering},
};
use std::thread;
use std::time::{Duration, Instant};

use anyhow::{Context, Result};
use serde::Serialize;
use serde_json::{Map, Value};
use zenoh::Wait;

use crate::contract_warning::ContractWarningThrottle;
use crate::types::TopicType;
use crate::zenoh_util::open_session;

const HTML: &str = include_str!("graph_ui.html");
const JS: &str = include_str!("graph_ui.js");

#[derive(Debug, Clone)]
pub struct GraphConfig {
    pub connect: String,
    pub bind: String,
    pub keyexpr: String,
    pub admin_poll: Duration,
}

pub fn serve(config: GraphConfig, shutdown: Arc<AtomicBool>) -> Result<()> {
    let state = Arc::new(Mutex::new(GraphState::new(
        config.connect.clone(),
        config.keyexpr.clone(),
    )));

    spawn_observer(config.clone(), state.clone(), shutdown.clone());
    spawn_admin_poller(config.clone(), state.clone(), shutdown.clone());

    let listener = TcpListener::bind(&config.bind)
        .with_context(|| format!("failed to bind graph server to {}", config.bind))?;
    listener
        .set_nonblocking(true)
        .context("failed to set graph server nonblocking")?;

    eprintln!("csyn graph available at http://{}", config.bind);
    eprintln!("observing {} via {}", config.keyexpr, config.connect);

    while !shutdown.load(Ordering::Relaxed) {
        match listener.accept() {
            Ok((stream, peer)) => {
                let state = state.clone();
                thread::spawn(move || {
                    if let Err(error) = handle_connection(stream, peer, state) {
                        eprintln!("graph HTTP error: {error:#}");
                    }
                });
            }
            Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                thread::sleep(Duration::from_millis(25));
            }
            Err(error) => return Err(error.into()),
        }
    }

    Ok(())
}

fn spawn_observer(config: GraphConfig, state: Arc<Mutex<GraphState>>, shutdown: Arc<AtomicBool>) {
    thread::spawn(move || {
        let session = match open_session(&config.connect) {
            Ok(session) => session,
            Err(error) => {
                state.lock().expect("graph state poisoned").last_error =
                    Some(format!("traffic observer failed to connect: {error:#}"));
                return;
            }
        };
        let subscriber = match session.declare_subscriber(config.keyexpr.clone()).wait() {
            Ok(subscriber) => subscriber,
            Err(error) => {
                state.lock().expect("graph state poisoned").last_error =
                    Some(format!("traffic observer failed to subscribe: {error}"));
                return;
            }
        };
        let mut warnings = ContractWarningThrottle::default();

        while !shutdown.load(Ordering::Relaxed) {
            let sample = match subscriber.recv_timeout(Duration::from_millis(100)) {
                Ok(Some(sample)) => sample,
                Ok(None) => continue,
                Err(error) => {
                    state.lock().expect("graph state poisoned").last_error =
                        Some(format!("traffic observer receive error: {error}"));
                    continue;
                }
            };

            let topic = sample.key_expr().to_string();
            let payload_len = sample.payload().to_bytes().len();
            let known_type = match TopicType::from_value_encoding(sample.encoding()) {
                Ok(known) => known,
                Err(error) => {
                    warnings.warn(&topic, &error);
                    state.lock().expect("graph state poisoned").last_error =
                        Some(format!("rejecting {topic}: {error}"));
                    continue;
                }
            };
            let root_type = known_type.wire_type().map(str::to_owned);
            state.lock().expect("graph state poisoned").observe_topic(
                topic,
                payload_len,
                root_type,
            );
        }
    });
}

fn spawn_admin_poller(
    config: GraphConfig,
    state: Arc<Mutex<GraphState>>,
    shutdown: Arc<AtomicBool>,
) {
    thread::spawn(move || {
        let session = match open_session(&config.connect) {
            Ok(session) => session,
            Err(error) => {
                state.lock().expect("graph state poisoned").admin_error =
                    Some(format!("admin poller failed to connect: {error:#}"));
                return;
            }
        };

        while !shutdown.load(Ordering::Relaxed) {
            let mut entries = BTreeMap::new();
            match session.get("@/**").wait() {
                Ok(replies) => loop {
                    match replies.recv_timeout(Duration::from_millis(200)) {
                        Ok(Some(reply)) => match reply.into_result() {
                            Ok(sample) => {
                                let key = sample.key_expr().to_string();
                                let payload =
                                    String::from_utf8_lossy(&sample.payload().to_bytes()).into();
                                entries.insert(key, payload);
                            }
                            Err(error) => {
                                entries.insert("@error/reply".to_owned(), format!("{error:?}"));
                            }
                        },
                        Ok(None) => break,
                        Err(error) => {
                            entries.insert("@error/recv".to_owned(), format!("{error}"));
                            break;
                        }
                    }
                },
                Err(error) => {
                    state.lock().expect("graph state poisoned").admin_error =
                        Some(format!("admin query failed: {error}"));
                    thread::sleep(config.admin_poll);
                    continue;
                }
            }

            let mut state = state.lock().expect("graph state poisoned");
            state.admin_error = None;
            state.admin_entries = entries;
            state.last_admin_poll_ms = state.started.elapsed().as_millis() as u64;
            drop(state);

            thread::sleep(config.admin_poll);
        }
    });
}

fn handle_connection(
    mut stream: TcpStream,
    _peer: SocketAddr,
    state: Arc<Mutex<GraphState>>,
) -> Result<()> {
    let mut buffer = [0_u8; 4096];
    let read = stream.read(&mut buffer)?;
    if read == 0 {
        return Ok(());
    }
    let request = String::from_utf8_lossy(&buffer[..read]);
    let mut parts = request.lines().next().unwrap_or("").split_whitespace();
    let method = parts.next().unwrap_or("");
    let path = parts.next().unwrap_or("/");

    if method != "GET" {
        return write_response(
            &mut stream,
            "405 Method Not Allowed",
            "text/plain; charset=utf-8",
            b"method not allowed",
        );
    }

    match path.split('?').next().unwrap_or("/") {
        "/" => write_response(
            &mut stream,
            "200 OK",
            "text/html; charset=utf-8",
            HTML.as_bytes(),
        ),
        "/app.js" => write_response(
            &mut stream,
            "200 OK",
            "application/javascript; charset=utf-8",
            JS.as_bytes(),
        ),
        "/api/graph" => {
            let snapshot = state.lock().expect("graph state poisoned").snapshot();
            let body = serde_json::to_vec(&snapshot)?;
            write_response(&mut stream, "200 OK", "application/json", &body)
        }
        _ => write_response(
            &mut stream,
            "404 Not Found",
            "text/plain; charset=utf-8",
            b"not found",
        ),
    }
}

fn write_response(
    stream: &mut TcpStream,
    status: &str,
    content_type: &str,
    body: &[u8],
) -> Result<()> {
    write!(
        stream,
        "HTTP/1.1 {status}\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
        body.len()
    )?;
    stream.write_all(body)?;
    Ok(())
}

#[derive(Debug)]
struct GraphState {
    connect: String,
    keyexpr: String,
    started: Instant,
    topics: BTreeMap<String, TopicStats>,
    admin_entries: BTreeMap<String, String>,
    last_admin_poll_ms: u64,
    last_error: Option<String>,
    admin_error: Option<String>,
}

impl GraphState {
    fn new(connect: String, keyexpr: String) -> Self {
        Self {
            connect,
            keyexpr,
            started: Instant::now(),
            topics: BTreeMap::new(),
            admin_entries: BTreeMap::new(),
            last_admin_poll_ms: 0,
            last_error: None,
            admin_error: None,
        }
    }

    fn observe_topic(&mut self, topic: String, payload_len: usize, root_type: Option<String>) {
        let now = self.started.elapsed().as_millis() as u64;
        let topic_stats = self
            .topics
            .entry(topic.clone())
            .or_insert_with(|| TopicStats {
                topic,
                root_type: root_type.clone(),
                messages: 0,
                bytes: 0,
                last_payload_bytes: 0,
                last_seen_ms: now,
                recent: VecDeque::new(),
            });
        if topic_stats.root_type.is_none() {
            topic_stats.root_type = root_type;
        }
        topic_stats.messages += 1;
        topic_stats.bytes += payload_len as u64;
        topic_stats.last_payload_bytes = payload_len as u64;
        topic_stats.last_seen_ms = now;
        topic_stats.recent.push_back(now);
        while topic_stats
            .recent
            .front()
            .is_some_and(|oldest| now.saturating_sub(*oldest) > 5_000)
        {
            topic_stats.recent.pop_front();
        }
    }

    fn snapshot(&self) -> GraphSnapshot {
        let now_ms = self.started.elapsed().as_millis() as u64;
        let mut nodes = BTreeMap::<String, GraphNode>::new();
        let mut links = Vec::<GraphLink>::new();

        nodes.insert(
            "zenoh".to_owned(),
            GraphNode {
                id: "zenoh".to_owned(),
                label: "Zenoh".to_owned(),
                kind: "transport".to_owned(),
                detail: self.connect.clone(),
                messages: 0,
                bytes: 0,
                rate_hz: 0.0,
                stale: false,
            },
        );

        for topic in self.topics.values() {
            let stale = now_ms.saturating_sub(topic.last_seen_ms) > 5_000;
            let topic_id = format!("topic:{}", topic.topic);
            nodes.insert(
                topic_id.clone(),
                GraphNode {
                    id: topic_id.clone(),
                    label: topic.topic.clone(),
                    kind: "topic".to_owned(),
                    detail: topic
                        .root_type
                        .clone()
                        .unwrap_or_else(|| "unknown payload".to_owned()),
                    messages: topic.messages,
                    bytes: topic.bytes,
                    rate_hz: topic.recent.len() as f64 / 5.0,
                    stale,
                },
            );
            links.push(GraphLink {
                source: "zenoh".to_owned(),
                target: topic_id,
                label: format!("{} msg", topic.messages),
                kind: "traffic".to_owned(),
            });
        }

        add_admin_topology(&self.admin_entries, &mut nodes, &mut links);

        let admin_entities = parse_admin_entities(&self.admin_entries);
        for entity in &admin_entities {
            let router_id = admin_node_id(&entity.mode, &entity.zid);
            insert_admin_node(
                &mut nodes,
                router_id.clone(),
                &entity.mode,
                &entity.zid,
                entity.zid.clone(),
            );

            let entity_id = format!(
                "admin:{}:{}:{}",
                entity.kind,
                entity.zid,
                entity.expr.replace('/', "_")
            );
            nodes.entry(entity_id.clone()).or_insert_with(|| GraphNode {
                id: entity_id.clone(),
                label: entity.kind.clone(),
                kind: entity.kind.clone(),
                detail: entity.expr.clone(),
                messages: 0,
                bytes: 0,
                rate_hz: 0.0,
                stale: false,
            });

            links.push(GraphLink {
                source: router_id.clone(),
                target: entity_id.clone(),
                label: "admin".to_owned(),
                kind: "admin".to_owned(),
            });

            if matches!(entity.kind.as_str(), "publisher" | "subscriber") {
                let topic_id = format!("topic:{}", entity.expr);
                nodes.entry(topic_id.clone()).or_insert_with(|| GraphNode {
                    id: topic_id.clone(),
                    label: entity.expr.clone(),
                    kind: "topic".to_owned(),
                    detail: "declared in Zenoh admin space".to_owned(),
                    messages: 0,
                    bytes: 0,
                    rate_hz: 0.0,
                    stale: true,
                });

                let (source, target) = if entity.kind == "publisher" {
                    (entity_id, topic_id)
                } else {
                    (topic_id, entity_id)
                };
                links.push(GraphLink {
                    source,
                    target,
                    label: entity.kind.clone(),
                    kind: entity.kind.clone(),
                });
            }
        }

        GraphSnapshot {
            uptime_ms: now_ms,
            connect: self.connect.clone(),
            observed_keyexpr: self.keyexpr.clone(),
            admin_entries: self.admin_entries.len(),
            last_admin_poll_ms: self.last_admin_poll_ms,
            last_error: self.last_error.clone(),
            admin_error: self.admin_error.clone(),
            nodes: nodes.into_values().collect(),
            links,
            topics: self
                .topics
                .values()
                .map(|topic| TopicSnapshot {
                    topic: topic.topic.clone(),
                    root_type: topic.root_type.clone(),
                    messages: topic.messages,
                    bytes: topic.bytes,
                    last_payload_bytes: topic.last_payload_bytes,
                    age_ms: now_ms.saturating_sub(topic.last_seen_ms),
                    rate_hz_5s: topic.recent.len() as f64 / 5.0,
                })
                .collect(),
        }
    }
}

#[derive(Debug)]
struct TopicStats {
    topic: String,
    root_type: Option<String>,
    messages: u64,
    bytes: u64,
    last_payload_bytes: u64,
    last_seen_ms: u64,
    recent: VecDeque<u64>,
}

#[derive(Debug, Serialize)]
struct GraphSnapshot {
    uptime_ms: u64,
    connect: String,
    observed_keyexpr: String,
    admin_entries: usize,
    last_admin_poll_ms: u64,
    last_error: Option<String>,
    admin_error: Option<String>,
    nodes: Vec<GraphNode>,
    links: Vec<GraphLink>,
    topics: Vec<TopicSnapshot>,
}

#[derive(Debug, Serialize)]
struct GraphNode {
    id: String,
    label: String,
    kind: String,
    detail: String,
    messages: u64,
    bytes: u64,
    rate_hz: f64,
    stale: bool,
}

#[derive(Debug, Serialize)]
struct GraphLink {
    source: String,
    target: String,
    label: String,
    kind: String,
}

#[derive(Debug, Serialize)]
struct TopicSnapshot {
    topic: String,
    root_type: Option<String>,
    messages: u64,
    bytes: u64,
    last_payload_bytes: u64,
    age_ms: u64,
    rate_hz_5s: f64,
}

#[derive(Debug)]
struct AdminEntity {
    zid: String,
    mode: String,
    kind: String,
    expr: String,
}

fn parse_admin_entities(entries: &BTreeMap<String, String>) -> Vec<AdminEntity> {
    entries
        .keys()
        .filter_map(|key| {
            let parts: Vec<_> = key.split('/').collect();
            if parts.len() < 6 || parts.first().copied() != Some("@") {
                return None;
            }
            let zid = parts.get(1)?.to_string();
            let mode = parts.get(2)?.to_string();
            let kind = parts.get(3)?.to_string();
            let last = parts.last()?;
            if *last != kind {
                return None;
            }
            let expr = parts[4..parts.len().saturating_sub(1)].join("/");
            if expr.is_empty() {
                return None;
            }
            Some(AdminEntity {
                zid,
                mode,
                kind,
                expr,
            })
        })
        .collect()
}

fn add_admin_topology(
    entries: &BTreeMap<String, String>,
    nodes: &mut BTreeMap<String, GraphNode>,
    links: &mut Vec<GraphLink>,
) {
    for payload in entries.values() {
        let Ok(value) = serde_json::from_str::<Value>(payload) else {
            continue;
        };
        let Some(object) = value.as_object() else {
            continue;
        };

        if let (Some(zid), Some(whatami)) =
            (string_field(object, "zid"), string_field(object, "whatami"))
        {
            insert_admin_node(
                nodes,
                admin_node_id(whatami, zid),
                whatami,
                zid,
                zid.to_owned(),
            );
        }

        if let Some(router_list) = object.get("routers").and_then(Value::as_array) {
            for zid in router_list.iter().filter_map(Value::as_str) {
                insert_admin_node(
                    nodes,
                    admin_node_id("router", zid),
                    "router",
                    zid,
                    zid.to_owned(),
                );
            }
        }

        if let Some(peer_list) = object.get("peers").and_then(Value::as_array) {
            for zid in peer_list.iter().filter_map(Value::as_str) {
                insert_admin_node(
                    nodes,
                    admin_node_id("peer", zid),
                    "peer",
                    zid,
                    zid.to_owned(),
                );
            }
        }

        if let Some(client_list) = object.get("clients").and_then(Value::as_array) {
            for zid in client_list.iter().filter_map(Value::as_str) {
                insert_admin_node(
                    nodes,
                    admin_node_id("client", zid),
                    "client",
                    zid,
                    zid.to_owned(),
                );
            }
        }

        let Some(router_zid) = string_field(object, "zid") else {
            continue;
        };
        let Some(sessions) = object.get("sessions").and_then(Value::as_array) else {
            continue;
        };
        let router_id = admin_node_id("router", router_zid);
        insert_admin_node(
            nodes,
            router_id.clone(),
            "router",
            router_zid,
            router_zid.to_owned(),
        );

        for session in sessions.iter().filter_map(Value::as_object) {
            let Some(peer_zid) = string_field(session, "peer") else {
                continue;
            };
            let mode = string_field(session, "whatami").unwrap_or("client");
            let node_id = admin_node_id(mode, peer_zid);
            let region = string_field(session, "region").unwrap_or("");
            let detail = session_links(session)
                .map(|links| {
                    if region.is_empty() {
                        links
                    } else {
                        format!("{region}; {links}")
                    }
                })
                .unwrap_or_else(|| {
                    if region.is_empty() {
                        peer_zid.to_owned()
                    } else {
                        region.to_owned()
                    }
                });

            insert_admin_node(nodes, node_id.clone(), mode, peer_zid, detail);
            links.push(GraphLink {
                source: router_id.clone(),
                target: node_id,
                label: if region.is_empty() {
                    "session".to_owned()
                } else {
                    region.to_owned()
                },
                kind: "admin".to_owned(),
            });
        }
    }
}

fn insert_admin_node(
    nodes: &mut BTreeMap<String, GraphNode>,
    id: String,
    mode: &str,
    zid: &str,
    detail: String,
) {
    let kind = admin_kind(mode);
    nodes.entry(id.clone()).or_insert_with(|| GraphNode {
        id,
        label: format!("{kind} {}", shorten(zid, 8)),
        kind: kind.to_owned(),
        detail,
        messages: 0,
        bytes: 0,
        rate_hz: 0.0,
        stale: false,
    });
}

fn admin_node_id(mode: &str, zid: &str) -> String {
    format!("{}:{}", admin_kind(mode), zid)
}

fn admin_kind(mode: &str) -> &str {
    match mode {
        "router" | "peer" | "client" => mode,
        _ => "other",
    }
}

fn string_field<'a>(object: &'a Map<String, Value>, field: &str) -> Option<&'a str> {
    object.get(field).and_then(Value::as_str)
}

fn session_links(session: &Map<String, Value>) -> Option<String> {
    let links = session.get("links")?.as_array()?;
    let rendered = links
        .iter()
        .filter_map(Value::as_object)
        .filter_map(|link| {
            let src = string_field(link, "src")?;
            let dst = string_field(link, "dst")?;
            Some(format!("{src} -> {dst}"))
        })
        .collect::<Vec<_>>();

    if rendered.is_empty() {
        None
    } else {
        Some(rendered.join(", "))
    }
}

fn shorten(value: &str, limit: usize) -> String {
    if value.len() <= limit {
        value.to_owned()
    } else {
        format!("{}...", &value[..limit])
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_admin_entity_keys() {
        let mut entries = BTreeMap::new();
        entries.insert(
            "@/abc/router/subscriber/synapse/manual_control/subscriber".to_owned(),
            "{}".to_owned(),
        );

        let entities = parse_admin_entities(&entries);
        assert_eq!(entities.len(), 1);
        assert_eq!(entities[0].zid, "abc");
        assert_eq!(entities[0].mode, "router");
        assert_eq!(entities[0].kind, "subscriber");
        assert_eq!(entities[0].expr, "synapse/manual_control");
    }

    #[test]
    fn parses_admin_router_sessions() {
        let mut entries = BTreeMap::new();
        entries.insert(
            "@/router/status".to_owned(),
            r#"{
                "zid": "router-zid",
                "whatami": "router",
                "sessions": [{
                    "peer": "client-zid",
                    "whatami": "client",
                    "region": "south:0:client",
                    "links": [{"src": "tcp/127.0.0.1:7447", "dst": "tcp/127.0.0.1:50000"}]
                }]
            }"#
            .to_owned(),
        );

        let mut nodes = BTreeMap::new();
        let mut links = Vec::new();
        add_admin_topology(&entries, &mut nodes, &mut links);

        assert!(nodes.contains_key("router:router-zid"));
        assert!(nodes.contains_key("client:client-zid"));
        assert_eq!(links.len(), 1);
        assert_eq!(links[0].source, "router:router-zid");
        assert_eq!(links[0].target, "client:client-zid");
    }
}
