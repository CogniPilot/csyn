use std::collections::BTreeMap;
use std::fmt::Display;
use std::time::{Duration, Instant};

const WARNING_INTERVAL: Duration = Duration::from_secs(10);

#[derive(Debug, Default)]
pub struct ContractWarningThrottle {
    last_warning: BTreeMap<String, Instant>,
}

impl ContractWarningThrottle {
    pub fn warn(&mut self, key: &str, error: impl Display) {
        let now = Instant::now();
        self.last_warning
            .retain(|_, last| now.duration_since(*last) < WARNING_INTERVAL);
        let should_warn = self
            .last_warning
            .get(key)
            .is_none_or(|last| now.duration_since(*last) >= WARNING_INTERVAL);
        if should_warn {
            eprintln!("warning: rejecting {key}: {error}");
            self.last_warning.insert(key.to_owned(), now);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn warning_state_is_tracked_per_topic() {
        let mut throttle = ContractWarningThrottle::default();
        throttle.warn("cub1/odom", "schema hash mismatch");
        let first = throttle.last_warning["cub1/odom"];
        throttle.warn("cub1/odom", "schema hash mismatch");
        assert_eq!(throttle.last_warning["cub1/odom"], first);
        throttle.warn("cub2/odom", "schema hash mismatch");
        assert_eq!(throttle.last_warning.len(), 2);

        throttle
            .last_warning
            .insert("cub1/odom".to_owned(), Instant::now() - WARNING_INTERVAL);
        let expired = throttle.last_warning["cub1/odom"];
        throttle.warn("cub1/odom", "schema hash mismatch");
        assert!(throttle.last_warning["cub1/odom"] > expired);
    }
}
