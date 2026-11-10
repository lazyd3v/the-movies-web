-- Problem reports from the public play page (workers/crash-reports/src/index.js).
-- Apply: npx wrangler d1 execute movies-crash-reports --remote --file workers/crash-reports/schema.sql
CREATE TABLE IF NOT EXISTS reports (
  id TEXT PRIMARY KEY,
  created INTEGER NOT NULL,          -- ms since epoch
  client TEXT NOT NULL,              -- salted daily hash of the IP, for throttling only
  kind TEXT NOT NULL,                -- 'crash' (automatic) or 'user' (Report a problem)
  signature TEXT NOT NULL,           -- groups reports of the same bug
  reason TEXT,                       -- status text shown to the player (crash reports)
  description TEXT,                  -- what the player wrote (user reports)
  contact TEXT,                      -- optional email/handle the player gave for a reply
  site_version TEXT,                 -- release version + git commit
  build TEXT,                        -- probe.wasm SHA-256
  user_agent TEXT,
  gpu TEXT,                          -- WebGPU adapter vendor / architecture
  country TEXT,                      -- Cloudflare country code
  session_seconds REAL,
  frames INTEGER,
  log_tail TEXT                      -- last lines of the technical log (optional for user reports)
);
CREATE INDEX IF NOT EXISTS reports_created ON reports (created);
CREATE INDEX IF NOT EXISTS reports_signature ON reports (kind, signature, created);
CREATE INDEX IF NOT EXISTS reports_client ON reports (client, created);
