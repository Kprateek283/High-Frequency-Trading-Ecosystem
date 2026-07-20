"""Tier 5 — launch/supervise the engine against a real readiness barrier (I8).

Replaces run_sharding.sh's `sleep 2` with the actual `READY` line the exchange
prints once sockets are bound and workers are pinned. Supervises via the PID and
the stats-region heartbeat, then delivers SIGINT and confirms a clean exit. The
shell script stays as the thin no-Python path; this is the reliable one.
"""
import os
import select
import signal
import subprocess
import time

from .config import Config
from .feeds.stats_reader import StatsReader
from .clock import Clock


class Orchestrator:
    def __init__(self, cfg=None, binary=None):
        self.cfg = cfg or Config()
        self.binary = binary or os.path.join(self.cfg.root, "build", "bin", "exchange")
        self.proc = None
        self._stdout_tail = []

    def _child_env(self):
        # Child inherits our env, then config.env values so every getenv() in the
        # engine agrees with what the monitor reads.
        env = dict(os.environ)
        env.update(self.cfg.file)
        env.setdefault("STATS_SHM_PATH", self.cfg.get("STATS_SHM_PATH"))
        env.setdefault("PID_FILE", self.cfg.get("PID_FILE"))
        return env

    def launch(self, ready_timeout=15.0):
        """Start the exchange, block until it prints READY. Returns True on READY."""
        self.proc = subprocess.Popen(
            [self.binary], cwd=self.cfg.root, env=self._child_env(),
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        deadline = time.monotonic() + ready_timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                return False                              # died before READY
            r, _, _ = select.select([self.proc.stdout], [], [], deadline - time.monotonic())
            if not r:
                continue
            line = self.proc.stdout.readline()
            if line == "":
                return False                              # EOF: process gone
            self._stdout_tail.append(line)
            if line.strip() == "READY":
                return True
        return False

    def pid(self):
        return self.proc.pid if self.proc else None

    def is_alive(self):
        return self.proc is not None and self.proc.poll() is None

    def heartbeat_age_ns(self):
        """Wall-clock age of the engine's stats-region heartbeat, or None if unreadable."""
        try:
            reader = StatsReader(self.cfg.get_path("STATS_SHM_PATH"))
        except OSError:
            return None
        try:
            snap = reader.read()
            if snap is None or not snap.valid():
                return None
            clock = Clock(*snap.anchor)
            return time.time_ns() - clock.to_epoch_ns(snap.heartbeat_tsc)
        finally:
            reader.close()

    def shutdown(self, timeout=10.0):
        """SIGINT, wait for a clean exit; escalate to SIGKILL if it hangs. Returns exit code."""
        if not self.proc:
            return None
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        try:
            self._stdout_tail.append(self.proc.stdout.read())   # final stats print
        except (OSError, ValueError):
            pass
        return self.proc.returncode

    def stdout_tail(self):
        return "".join(self._stdout_tail)


def _selftest():
    # CI-local: launch the real engine, prove the barrier and a fresh heartbeat,
    # then a clean SIGINT shutdown. Skips if the binary isn't built.
    cfg = Config()
    binary = os.path.join(cfg.root, "build", "bin", "exchange")
    if not os.path.exists(binary):
        print("orchestrator: SKIP (build/bin/exchange not built)")
        return
    orch = Orchestrator(cfg)
    assert orch.launch(ready_timeout=15.0), "engine never printed READY"
    assert orch.is_alive() and orch.pid() > 0
    time.sleep(0.3)                                     # let the heartbeat tick
    age = orch.heartbeat_age_ns()
    assert age is not None and age < 1_000_000_000, f"stale/absent heartbeat: {age}"
    code = orch.shutdown(timeout=10.0)
    assert not orch.is_alive()
    assert code == 0, f"unclean exit: {code}"
    print(f"orchestrator: OK (READY barrier, heartbeat {age/1e6:.0f}ms, exit {code})")


if __name__ == "__main__":
    _selftest()
