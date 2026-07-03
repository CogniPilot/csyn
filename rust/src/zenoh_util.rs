use anyhow::{Result, anyhow};
use zenoh::{Wait, config::Config};

pub const DEFAULT_CONNECT: &str = "tcp/127.0.0.1:7447";

pub fn open_session(connect: &str) -> Result<zenoh::Session> {
    zenoh::open(config(connect)?).wait().map_err(|error| {
        anyhow!(
            "failed to open Zenoh session at {connect}: {error}\n\n{}",
            connection_hint(connect)
        )
    })
}

fn config(connect: &str) -> Result<Config> {
    let mut config = Config::default();
    config
        .insert_json5("mode", "\"client\"")
        .map_err(|error| anyhow!("failed to configure Zenoh mode: {error}"))?;
    config
        .insert_json5("connect/endpoints", &format!("[\"{connect}\"]"))
        .map_err(|error| anyhow!("failed to configure Zenoh endpoint: {error}"))?;
    Ok(config)
}

fn connection_hint(connect: &str) -> String {
    let mut hint = format!("hint: make sure a Zenoh router is reachable at `{connect}`.");

    if connect == DEFAULT_CONNECT {
        hint.push_str("\nstart a local router in another terminal with:");
        hint.push_str(&format!("\n  zenohd -l {DEFAULT_CONNECT}"));
    } else {
        hint.push_str("\nfor a local router, use:");
        hint.push_str(&format!("\n  zenohd -l {DEFAULT_CONNECT}"));
    }

    hint.push_str("\nor point csyn at an existing router with:");
    hint.push_str("\n  csyn --connect tcp/<host>:7447 topic list");
    hint.push_str("\n  CSYN_CONNECT=tcp/<host>:7447 csyn topic list");
    hint
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_connection_hint_mentions_zenohd() {
        let hint = connection_hint(DEFAULT_CONNECT);

        assert!(hint.contains("zenohd -l tcp/127.0.0.1:7447"));
        assert!(hint.contains("csyn --connect tcp/<host>:7447 topic list"));
    }
}
