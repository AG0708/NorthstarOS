#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import socket
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("net_peer", ROOT / "tools/net_peer.py")
assert SPEC and SPEC.loader
NET = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = NET
SPEC.loader.exec_module(NET)


def config(**overrides):
    values = dict(
        peer_mac=NET.parse_mac("52:54:00:12:34:56"),
        peer_ip=NET.ip_bytes("10.0.2.2"),
        guest_ip=NET.ip_bytes("10.0.2.15"),
        dns_name="northstar.test",
        dns_ip=NET.ip_bytes("10.0.2.2"),
        lease_seconds=3600,
        drop_first_tcp_data=False,
        duplicate_tcp_response=False,
        reorder_tcp_response=False,
        inject_malformed=False,
    )
    values.update(overrides)
    return NET.PeerConfig(**values)


def guest_ipv4(protocol: int, payload: bytes, destination: bytes | None = None):
    source_ip = NET.ip_bytes("10.0.2.15")
    destination_ip = destination or NET.ip_bytes("10.0.2.2")
    packet = NET.ipv4(source_ip, destination_ip, protocol, payload, 7)
    return NET.ethernet(NET.parse_mac("52:54:00:12:34:56"),
                        NET.parse_mac("52:54:00:12:34:57"),
                        NET.ETH_IPV4, packet)


def parse_ipv4_frame(frame: bytes):
    assert frame[12:14] == b"\x08\x00"
    packet = frame[14:]
    assert NET.checksum(packet[:20]) == 0
    total = struct.unpack_from("!H", packet, 2)[0]
    return packet[9], packet[12:16], packet[16:20], packet[20:total]


def make_peer(**overrides):
    sent = []
    events = []
    peer = NET.NetworkPeer(config(**overrides), sent.append,
                           lambda name, fields: events.append((name, fields)))
    return peer, sent, events


def test_checksum_and_dns_name_bounds():
    assert NET.checksum(b"\x00\x01\xf2\x03\xf4\xf5\xf6\xf7") == 0x220D
    packet = b"\x03www\x07example\x03com\x00"
    assert NET.decode_dns_name(packet, 0) == ("www.example.com", len(packet))
    try:
        NET.decode_dns_name(b"\xc0\x00", 0)
    except NET.PacketError:
        pass
    else:
        raise AssertionError("compression loop accepted")


def test_arp_and_icmp():
    peer, sent, events = make_peer()
    guest_mac = NET.parse_mac("52:54:00:12:34:57")
    guest_ip = NET.ip_bytes("10.0.2.15")
    request = struct.pack("!HHBBH", 1, NET.ETH_IPV4, 6, 4, 1)
    request += guest_mac + guest_ip + b"\0" * 6 + peer.config.peer_ip
    peer.handle_frame(NET.ethernet(NET.BROADCAST_MAC, guest_mac, NET.ETH_ARP,
                                   request))
    assert len(sent) == 1 and sent[0][:6] == guest_mac
    assert sent[0][20:22] == b"\x00\x02"

    echo = b"\x08\x00\x00\x00\x12\x34\x00\x01payload"
    echo = echo[:2] + struct.pack("!H", NET.checksum(echo)) + echo[4:]
    peer.handle_frame(guest_ipv4(NET.IP_ICMP, echo))
    protocol, source, destination, response = parse_ipv4_frame(sent[-1])
    assert protocol == NET.IP_ICMP and source == peer.config.peer_ip
    assert destination == guest_ip and response[0] == 0
    assert NET.checksum(response) == 0
    assert {event[0] for event in events} >= {"arp_reply", "icmp_echo"}


def dhcp_request(peer, message_type: int) -> bytes:
    guest_mac = NET.parse_mac("52:54:00:12:34:57")
    xid = b"\x12\x34\x56\x78"
    fixed = b"\x01\x01\x06\x00" + xid + b"\0" * 20
    fixed += guest_mac.ljust(16, b"\0") + b"\0" * 64 + b"\0" * 128
    payload = fixed + NET.DHCP_MAGIC + NET.encode_dhcp_option(
        53, bytes((message_type,))) + b"\xff"
    segment = NET.udp(b"\0" * 4, b"\xff" * 4, 68, 67, payload)
    packet = NET.ipv4(b"\0" * 4, b"\xff" * 4, NET.IP_UDP, segment, 1)
    return NET.ethernet(NET.BROADCAST_MAC, guest_mac, NET.ETH_IPV4, packet)


def test_dhcp_and_dns():
    peer, sent, events = make_peer()
    peer.handle_frame(dhcp_request(peer, 1))
    assert events[-1][0] == "dhcp_offer"
    _, _, _, segment = parse_ipv4_frame(sent[-1])
    assert struct.unpack_from("!HH", segment, 0) == (67, 68)
    options = NET.parse_options(segment[8 + 240:])
    assert options[53] == b"\x02" and options[54] == peer.config.peer_ip

    name = b"\x09northstar\x04test\x00"
    query = struct.pack("!HHHHHH", 0xBEEF, 0x0100, 1, 0, 0, 0)
    query += name + struct.pack("!HH", 1, 1)
    guest_ip = NET.ip_bytes("10.0.2.15")
    segment = NET.udp(guest_ip, peer.config.peer_ip, 49152, 53, query)
    peer.handle_frame(guest_ipv4(NET.IP_UDP, segment))
    _, _, _, response_udp = parse_ipv4_frame(sent[-1])
    response_dns = response_udp[8:]
    assert struct.unpack_from("!H", response_dns, 0)[0] == 0xBEEF
    assert struct.unpack_from("!H", response_dns, 6)[0] == 1
    assert response_dns.endswith(peer.config.dns_ip)


def make_tcp_guest(peer, source_port, sequence, acknowledgement, flags,
                   payload=b""):
    guest_ip = NET.ip_bytes("10.0.2.15")
    segment = NET.tcp(guest_ip, peer.config.peer_ip, source_port, 8080,
                      sequence, acknowledgement, flags, 32768, payload)
    return guest_ipv4(NET.IP_TCP, segment)


def parse_tcp_frame(frame):
    _, _, _, segment = parse_ipv4_frame(frame)
    fields = struct.unpack_from("!HHIIBBHHH", segment, 0)
    header_length = (fields[4] >> 4) * 4
    return fields, segment[header_length:]


def test_tcp_http_and_loss_injection():
    peer, sent, events = make_peer(drop_first_tcp_data=True)
    client_isn = 1000
    port = 50000
    peer.handle_frame(make_tcp_guest(peer, port, client_isn, 0, 0x02))
    synack, _ = parse_tcp_frame(sent[-1])
    server_isn = synack[2]
    assert synack[5] == 0x12 and synack[3] == client_isn + 1
    peer.handle_frame(make_tcp_guest(peer, port, client_isn + 1,
                                     server_isn + 1, 0x10))
    request = b"GET / HTTP/1.1\r\nHost: northstar.test\r\n\r\n"
    data_frame = make_tcp_guest(peer, port, client_isn + 1,
                                server_isn + 1, 0x18, request)
    count = len(sent)
    peer.handle_frame(data_frame)
    assert len(sent) == count
    peer.handle_frame(data_frame)
    response_fields, response_payload = parse_tcp_frame(sent[-1])
    assert response_fields[5] == 0x18
    assert response_payload.startswith(b"HTTP/1.1 200 OK")
    assert response_payload.endswith(b"northstar-network-ok\n")
    names = [event[0] for event in events]
    assert "tcp_injected_drop" in names and "http_response" in names


def test_malformed_packets_drop_without_response():
    peer, sent, events = make_peer()
    peer.handle_frame(b"tiny")
    bad_ip = bytearray(guest_ipv4(NET.IP_ICMP, b"\x08" + b"\0" * 7))
    bad_ip[24] ^= 1
    peer.handle_frame(bytes(bad_ip))
    assert sent == []
    assert [event[0] for event in events] == ["drop", "drop"]


def test_peer_injects_two_distinct_malformed_checksum_frames():
    peer, sent, events = make_peer(inject_malformed=True)
    guest_mac = NET.parse_mac("52:54:00:12:34:57")
    peer.inject_malformed_frames(guest_mac)
    assert len(sent) == 2
    assert sent[0][:6] == guest_mac and sent[1][:6] == guest_mac
    first_ip = sent[0][14:]
    second_ip = sent[1][14:]
    assert NET.checksum(first_ip[:20]) != 0
    assert NET.checksum(second_ip[:20]) == 0
    second_tcp = second_ip[20:]
    pseudo = second_ip[12:20] + struct.pack("!BBH", 0, NET.IP_TCP,
                                             len(second_tcp))
    assert NET.checksum(pseudo + second_tcp) != 0
    assert events[-1] == ("malformed_injected", {"frames": 2})


def main():
    test_checksum_and_dns_name_bounds()
    test_arp_and_icmp()
    test_dhcp_and_dns()
    test_tcp_http_and_loss_injection()
    test_malformed_packets_drop_without_response()
    test_peer_injects_two_distinct_malformed_checksum_frames()
    print("test_net_peer: PASS")


if __name__ == "__main__":
    main()
