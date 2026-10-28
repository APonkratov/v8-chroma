//! Process identity shared by policy and platform backends.
use serde::{Deserialize, Serialize};
use std::path::PathBuf;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct Identity {
    pub pid: u32,
    pub created_time: u64,
    pub exe: PathBuf,
}
