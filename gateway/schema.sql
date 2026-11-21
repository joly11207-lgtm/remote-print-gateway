-- Remote Print Gateway / Compatibility Engine V0.1
-- SQLite schema. Every table the V0.1 scope explicitly requires, plus the two
-- run-time tables (safe_test_runs) needed to prove "safe test -> instance".

PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS gateways (
  gateway_id      TEXT PRIMARY KEY,
  user_id         TEXT NOT NULL,
  name            TEXT NOT NULL,
  hardware_family TEXT NOT NULL,
  firmware_version TEXT NOT NULL,
  status          TEXT NOT NULL,          -- online | offline
  last_seen       TEXT NOT NULL,
  created_at      TEXT NOT NULL,
  -- 'simulator' = V0.1 fixtures; 'real' = an actual ESP32 reachable at base_url.
  mode            TEXT NOT NULL DEFAULT 'simulator',
  base_url        TEXT                    -- real only; never sent to the browser
);

CREATE TABLE IF NOT EXISTS gateway_capabilities (
  id              INTEGER PRIMARY KEY AUTOINCREMENT,
  gateway_id      TEXT NOT NULL REFERENCES gateways(gateway_id) ON DELETE CASCADE,
  capability_type TEXT NOT NULL,          -- transport | protocol | profile | engine
  capability_name TEXT NOT NULL,
  version         INTEGER NOT NULL DEFAULT 1,
  UNIQUE(gateway_id, capability_type, capability_name)
);

CREATE TABLE IF NOT EXISTS printer_profiles (
  profile_id            TEXT PRIMARY KEY,
  brand                 TEXT,
  model                 TEXT,
  display_name          TEXT,
  transport             TEXT NOT NULL,    -- ble | bluetooth_classic | wifi
  protocol              TEXT NOT NULL,    -- tiny_iprint | escpos | ipp | memobird
  parameters_json       TEXT NOT NULL,
  fingerprint_rules_json TEXT NOT NULL,
  version               INTEGER NOT NULL DEFAULT 1,
  status                TEXT NOT NULL,    -- AI_CANDIDATE | EXPERIMENTAL | VERIFIED | SUPPORTED | UNSUPPORTED
  source                TEXT
);

CREATE TABLE IF NOT EXISTS printer_instances (
  printer_id       TEXT PRIMARY KEY,
  gateway_id       TEXT NOT NULL REFERENCES gateways(gateway_id),
  profile_id       TEXT NOT NULL REFERENCES printer_profiles(profile_id),
  display_name     TEXT NOT NULL,
  physical_address TEXT,
  status           TEXT NOT NULL,         -- active | disabled
  case_id          TEXT REFERENCES compatibility_cases(case_id),
  created_at       TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS fingerprints (
  fingerprint_id        TEXT PRIMARY KEY,
  gateway_id            TEXT NOT NULL REFERENCES gateways(gateway_id),
  transport             TEXT NOT NULL,
  raw_data_json         TEXT NOT NULL,
  normalized_data_json  TEXT NOT NULL,
  created_at            TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS compatibility_cases (
  case_id        TEXT PRIMARY KEY,
  user_id        TEXT NOT NULL,
  gateway_id     TEXT NOT NULL REFERENCES gateways(gateway_id),
  fingerprint_id TEXT REFERENCES fingerprints(fingerprint_id),
  brand          TEXT,
  model          TEXT,
  original_app   TEXT,
  -- What the user believes the printer uses: bluetooth | wifi | unknown.
  -- Never a transport: the gateway fingerprint decides that.
  connection_hint TEXT NOT NULL DEFAULT 'unknown',
  status         TEXT NOT NULL,
  result_json    TEXT,
  created_at     TEXT NOT NULL,
  updated_at     TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS firmware_releases (
  id                INTEGER PRIMARY KEY AUTOINCREMENT,
  firmware_version  TEXT NOT NULL,
  hardware_family   TEXT NOT NULL,
  channel           TEXT NOT NULL,        -- internal | beta | stable
  capabilities_json TEXT NOT NULL,
  status            TEXT NOT NULL,        -- available | deprecated | revoked
  -- 1 = demo/test fixture only. Never shipped, never selected unless the
  -- caller explicitly asks for simulated firmware.
  simulated         INTEGER NOT NULL DEFAULT 0,
  released_at       TEXT,
  UNIQUE(firmware_version, hardware_family, channel, simulated)
);

CREATE TABLE IF NOT EXISTS safe_test_runs (
  run_id       TEXT PRIMARY KEY,
  case_id      TEXT NOT NULL REFERENCES compatibility_cases(case_id),
  safe_test_id TEXT NOT NULL,
  gateway_id   TEXT NOT NULL,
  status       TEXT NOT NULL,             -- passed | failed | blocked
  detail_json  TEXT NOT NULL,
  created_at   TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_caps_gateway ON gateway_capabilities(gateway_id);
CREATE INDEX IF NOT EXISTS idx_instances_gateway ON printer_instances(gateway_id);
CREATE INDEX IF NOT EXISTS idx_cases_status ON compatibility_cases(status);
CREATE INDEX IF NOT EXISTS idx_firmware_family ON firmware_releases(hardware_family, channel);
