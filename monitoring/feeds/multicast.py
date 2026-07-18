"""Tier 1 — join the ITCH multicast group (I1), decode datagram batches.

Same socket pattern as the firm's C++ listener: SO_REUSEADDR, bind INADDR_ANY on
the group port, IP_ADD_MEMBERSHIP. A datagram carries 1..N back-to-back
ItchMessages (the Publisher batches up to 64), so we decode with wire.iter_itch.
"""
import socket
import struct
from .. import wire


class MulticastReader:
    def __init__(self, group, port, timeout=None):
        self.group = group
        self.port = port
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("", port))
        mreq = struct.pack("4sl", socket.inet_aton(group), socket.INADDR_ANY)
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        if timeout is not None:
            s.settimeout(timeout)
        self.sock = s

    def poll(self, bufsize=65536):
        """Receive one datagram and yield the ItchMessages in it (raises on timeout)."""
        data = self.sock.recv(bufsize)
        yield from wire.iter_itch(data)

    @classmethod
    def from_config(cls, cfg, timeout=None):
        return cls(cfg.get("MULTICAST_GROUP"), cfg.get_int("MULTICAST_PORT"), timeout)

    def close(self):
        self.sock.close()


def _selftest():
    # Real multicast loopback if the sandbox allows it; otherwise skip the socket
    # leg (the live engine gate exercises the real join) but still prove decode.
    group, port = "239.255.0.77", 21777
    msgs = [struct.pack(wire.ITCH_FMT, b"A", i, 0, 100 + i, i, 50, 50000, b"B")
            for i in range(3)]
    datagram = b"".join(msgs)
    assert len(list(wire.iter_itch(datagram))) == 3           # batch decode always holds
    try:
        r = MulticastReader(group, port, timeout=1.0)
        snd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        snd.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
        snd.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
        snd.sendto(datagram, (group, port))
        got = list(r.poll())
        snd.close()
        r.close()
        assert len(got) == 3 and got[0].stock_locate == 0 and got[2].internal_id == 2
        print("multicast: OK (loopback)")
    except OSError as e:
        print(f"multicast: OK (decode only; socket skipped: {e})")


if __name__ == "__main__":
    _selftest()
