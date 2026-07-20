"""Tier 0 — the same config.env the engine, firm and scripts read.

Precedence: os.environ (what the orchestrator/`source config.env` exports)
overrides the file, which overrides the code defaults here. No parser, no
dependency — the file is `KEY=value` with optional inline `# comments`, exactly
what bash `source` tolerates.
"""
import os

# Code defaults; config.env overrides these, os.environ overrides both.
DEFAULTS = {
    "TCP_PORT": "9091",
    "MULTICAST_GROUP": "239.255.0.1",
    "MULTICAST_PORT": "12345",
    "NUM_SHARDS": "4",
    "GATEWAY_THREADS": "4",
    "GATEWAY_CORES": "1,3,5,7",
    "ENGINE_CORES": "2,4,6,8",
    "AUX_CORES": "0,9",
    "AUDIT_LOG_PATH": "order_audit.log",
    "STATS_SHM_PATH": "/dev/shm/hft_stats",
    "PID_FILE": "exchange.pid",
}


def _repo_root():
    # this file is monitoring/config.py → repo root is one dir up
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _parse_env_file(path):
    out = {}
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, val = line.split("=", 1)
                # ponytail: strip inline `# comment`; values here never contain '#'.
                val = val.split("#", 1)[0].strip()
                out[key.strip()] = val
    except FileNotFoundError:
        pass
    return out


class Config:
    def __init__(self, path=None):
        self.root = _repo_root()
        self.path = path or os.path.join(self.root, "config.env")
        self.file = _parse_env_file(self.path)

    def get(self, key, default=None):
        if key in os.environ:
            return os.environ[key]
        if key in self.file:
            return self.file[key]
        if key in DEFAULTS:
            return DEFAULTS[key]
        return default

    def get_int(self, key):
        return int(self.get(key))

    def get_path(self, key):
        """A path key resolved against the repo root when it is relative."""
        p = self.get(key)
        return p if os.path.isabs(p) else os.path.join(self.root, p)


def _selftest():
    c = Config(path="/nonexistent")   # falls back to DEFAULTS only
    assert c.get("TCP_PORT") == "9091" and c.get_int("NUM_SHARDS") == 4
    assert c.get_path("STATS_SHM_PATH") == "/dev/shm/hft_stats"  # absolute, unchanged
    assert c.get_path("AUDIT_LOG_PATH").endswith("/order_audit.log")  # relative → rooted
    os.environ["TCP_PORT"] = "5555"
    assert c.get("TCP_PORT") == "5555"   # env wins
    del os.environ["TCP_PORT"]
    # inline-comment parsing matches what bash source sees
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".env", delete=False) as f:
        f.write("MULTICAST_PORT=12345   # trailing comment\n# whole line\n\n")
        name = f.name
    c2 = Config(path=name)
    assert c2.get("MULTICAST_PORT") == "12345"
    os.unlink(name)
    print("config: OK")


if __name__ == "__main__":
    _selftest()
