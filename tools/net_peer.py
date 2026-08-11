#!/usr/bin/env python3
"""Deterministic Ethernet peer for NorthstarOS QEMU interoperability tests.

QEMU's socket netdev transports one Ethernet frame per UDP datagram.  This peer
answers ARP, ICMP echo, DHCP, DNS, UDP echo, and a small TCP/HTTP service.  It is
deliberately a protocol peer rather than a test shortcut: packets still traverse
the guest's RTL8139 device, DMA rings, Ethernet/IP stack, and socket layer.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import selectors
import socket
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

ETH_IPV4 = 0x0800
ETH_ARP = 0x0806
IP_ICMP = 1
IP_TCP = 6
IP_UDP = 17
DHCP_MAGIC = b"\x63\x82\x53\x63"
BROADCAST_MAC = b"\xff" * 6


class PcapWriter:
    """Minimal nanosecond-resolution classic-PCAP writer for Ethernet frames."""

    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.stream = path.open("wb")
        self.stream.write(struct.pack("<IHHIIII", 0xA1B23C4D, 2, 4, 0, 0,
                                      65535, 1))

    def write(self, frame: bytes) -> None:
        timestamp = time.time_ns()
        seconds, nanoseconds = divmod(timestamp, 1_000_000_000)
        self.stream.write(struct.pack("<IIII", seconds, nanoseconds,
                                      len(frame), len(frame)))
        self.stream.write(frame)

    def close(self) -> None:
        self.stream.flush()
        os.fsync(self.stream.fileno())
        self.stream.close()


class PacketError(ValueError):
    """Raised for an invalid or truncated frame; malformed input is dropped."""


def parse_mac(value: str) -> bytes:
    pieces = value.split(":")
    if len(pieces) != 6:
        raise argparse.ArgumentTypeError("MAC must contain six octets")
    try:
        result = bytes(int(piece, 16) for piece in pieces)
    except ValueError as error:
        raise argparse.ArgumentTypeError("invalid MAC") from error
    if any(len(piece) != 2 for piece in pieces):
        raise argparse.ArgumentTypeError("each MAC octet must use two digits")
    return result


def ip_bytes(value: str) -> bytes:
    try:
        return socket.inet_aton(value)
    except OSError as error:
        raise argparse.ArgumentTypeError(str(error)) from error


def checksum(data: bytes) -> int:
    if len(data) & 1:
        data += b"\0"
    total = 0
    for offset in range(0, len(data), 2):
        total += (data[offset] << 8) | data[offset + 1]
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def ethernet(destination: bytes, source: bytes, ethertype: int,
             payload: bytes) -> bytes:
    if len(destination) != 6 or len(source) != 6:
        raise ValueError("Ethernet address length")
    return destination + source + struct.pack("!H", ethertype) + payload


def ipv4(source: bytes, destination: bytes, protocol: int, payload: bytes,
         identification: int, flags_fragment: int = 0x4000,
         ttl: int = 64) -> bytes:
    total_length = 20 + len(payload)
    if total_length > 65535:
        raise ValueError("IPv4 packet too large")
    header = struct.pack(
        "!BBHHHBBH4s4s", 0x45, 0, total_length, identification & 0xFFFF,
        flags_fragment, ttl, protocol, 0, source, destination
    )
    header = header[:10] + struct.pack("!H", checksum(header)) + header[12:]
    return header + payload


def transport_checksum(source: bytes, destination: bytes, protocol: int,
                       segment: bytes) -> int:
    pseudo = source + destination + struct.pack("!BBH", 0, protocol,
                                                 len(segment))
    value = checksum(pseudo + segment)
    return value if value != 0 else 0xFFFF


def udp(source_ip: bytes, destination_ip: bytes, source_port: int,
        destination_port: int, payload: bytes) -> bytes:
    length = 8 + len(payload)
    header = struct.pack("!HHHH", source_port, destination_port, length, 0)
    value = transport_checksum(source_ip, destination_ip, IP_UDP,
                               header + payload)
    return struct.pack("!HHHH", source_port, destination_port, length,
                       value) + payload


def tcp(source_ip: bytes, destination_ip: bytes, source_port: int,
        destination_port: int, sequence: int, acknowledgement: int,
        flags: int, window: int, payload: bytes = b"",
        options: bytes = b"") -> bytes:
    if len(options) % 4 or len(options) > 40:
        raise ValueError("TCP options must be padded to a dword")
    data_offset = 5 + len(options) // 4
    header = struct.pack(
        "!HHIIBBHHH", source_port, destination_port, sequence & 0xFFFFFFFF,
        acknowledgement & 0xFFFFFFFF, data_offset << 4, flags, window, 0, 0
    ) + options
    value = transport_checksum(source_ip, destination_ip, IP_TCP,
                               header + payload)
    return header[:16] + struct.pack("!H", value) + header[18:] + payload


def parse_options(data: bytes) -> dict[int, bytes]:
    result: dict[int, bytes] = {}
    offset = 0
    while offset < len(data):
        code = data[offset]
        offset += 1
        if code == 0:
            continue
        if code == 255:
            break
        if offset >= len(data):
            raise PacketError("truncated option length")
        length = data[offset]
        offset += 1
        if offset + length > len(data):
            raise PacketError("truncated option value")
        result[code] = data[offset:offset + length]
        offset += length
    return result


def encode_dhcp_option(code: int, value: bytes) -> bytes:
    if len(value) > 255:
        raise ValueError("DHCP option too long")
    return bytes((code, len(value))) + value


def decode_dns_name(packet: bytes, offset: int,
                    allow_compression: bool = True) -> tuple[str, int]:
    labels: list[str] = []
    cursor = offset
    end = offset
    jumped = False
    visited: set[int] = set()
    for _ in range(128):
        if cursor >= len(packet):
            raise PacketError("truncated DNS name")
        length = packet[cursor]
        if length & 0xC0 == 0xC0:
            if not allow_compression or cursor + 1 >= len(packet):
                raise PacketError("invalid DNS compression")
            pointer = ((length & 0x3F) << 8) | packet[cursor + 1]
            if pointer >= len(packet) or pointer in visited:
                raise PacketError("DNS compression loop")
            visited.add(pointer)
            if not jumped:
                end = cursor + 2
                jumped = True
            cursor = pointer
            continue
        if length & 0xC0:
            raise PacketError("reserved DNS label encoding")
        cursor += 1
        if length == 0:
            if not jumped:
                end = cursor
            return ".".join(labels).lower(), end
        if length > 63 or cursor + length > len(packet):
            raise PacketError("invalid DNS label")
        try:
            labels.append(packet[cursor:cursor + length].decode("ascii"))
        except UnicodeDecodeError as error:
            raise PacketError("non-ASCII DNS label") from error
        cursor += length
    raise PacketError("DNS name exceeds traversal bound")


@dataclass
class TcpConnection:
    client_mac: bytes
    client_ip: bytes
    client_port: int
    client_next: int
    server_next: int
    state: str = "SYN_RCVD"
    response: bytes = b""
    response_sent: bool = False
    fin_sent: bool = False
    dropped_data_once: bool = False


@dataclass
class PeerConfig:
    peer_mac: bytes
    peer_ip: bytes
    guest_ip: bytes
    dns_name: str
    dns_ip: bytes
    lease_seconds: int
    drop_first_tcp_data: bool
    duplicate_tcp_response: bool
    reorder_tcp_response: bool
    inject_malformed: bool


@dataclass
class NetworkPeer:
    config: PeerConfig
    emit_frame: Callable[[bytes], None]
    log_event: Callable[[str, dict[str, object]], None]
    ip_identification: int = 1
    tcp_connections: dict[tuple[bytes, int], TcpConnection] = field(
        default_factory=dict
    )
    malformed_injected: bool = False

    def send_ipv4(self, destination_mac: bytes, destination_ip: bytes,
                  protocol: int, payload: bytes) -> None:
        packet = ipv4(self.config.peer_ip, destination_ip, protocol, payload,
                      self.ip_identification)
        self.ip_identification = (self.ip_identification + 1) & 0xFFFF
        self.emit_frame(ethernet(destination_mac, self.config.peer_mac,
                                 ETH_IPV4, packet))

    def handle_frame(self, frame: bytes) -> None:
        try:
            if len(frame) < 14:
                raise PacketError("truncated Ethernet frame")
            destination, source = frame[:6], frame[6:12]
            ethertype = struct.unpack_from("!H", frame, 12)[0]
            if destination not in (self.config.peer_mac, BROADCAST_MAC):
                return
            if ethertype == ETH_ARP:
                self.handle_arp(source, frame[14:])
            elif ethertype == ETH_IPV4:
                self.handle_ipv4(source, frame[14:])
        except PacketError as error:
            self.log_event("drop", {"reason": str(error)})

    def handle_arp(self, source_mac: bytes, packet: bytes) -> None:
        if len(packet) < 28:
            raise PacketError("truncated ARP")
        htype, ptype, hlen, plen, operation = struct.unpack_from(
            "!HHBBH", packet, 0
        )
        if (htype, ptype, hlen, plen) != (1, ETH_IPV4, 6, 4):
            raise PacketError("unsupported ARP format")
        sender_mac = packet[8:14]
        sender_ip = packet[14:18]
        target_ip = packet[24:28]
        if sender_mac != source_mac:
            raise PacketError("ARP/Ethernet source mismatch")
        if operation != 1 or target_ip != self.config.peer_ip:
            return
        reply = struct.pack("!HHBBH", 1, ETH_IPV4, 6, 4, 2)
        reply += self.config.peer_mac + self.config.peer_ip
        reply += sender_mac + sender_ip
        self.emit_frame(ethernet(sender_mac, self.config.peer_mac, ETH_ARP,
                                 reply))
        self.log_event("arp_reply", {"target": socket.inet_ntoa(sender_ip)})

    def handle_ipv4(self, source_mac: bytes, packet: bytes) -> None:
        if len(packet) < 20:
            raise PacketError("truncated IPv4")
        version_ihl, _, total, _, flags_fragment, ttl, protocol, _, source_ip, destination_ip = struct.unpack_from(
            "!BBHHHBBH4s4s", packet, 0
        )
        ihl = (version_ihl & 0x0F) * 4
        if version_ihl >> 4 != 4 or ihl < 20 or ihl > len(packet):
            raise PacketError("invalid IPv4 header")
        if total < ihl or total > len(packet):
            raise PacketError("invalid IPv4 total length")
        if checksum(packet[:ihl]) != 0:
            raise PacketError("bad IPv4 checksum")
        if flags_fragment & 0x3FFF:
            raise PacketError("fragmented IPv4 unsupported by peer")
        if ttl == 0:
            raise PacketError("expired IPv4 TTL")
        if destination_ip not in (self.config.peer_ip, b"\xff" * 4):
            return
        payload = packet[ihl:total]
        if protocol == IP_ICMP:
            self.handle_icmp(source_mac, source_ip, payload)
        elif protocol == IP_UDP:
            self.handle_udp(source_mac, source_ip, destination_ip, payload)
        elif protocol == IP_TCP:
            self.handle_tcp(source_mac, source_ip, destination_ip, payload)

    def handle_icmp(self, source_mac: bytes, source_ip: bytes,
                    packet: bytes) -> None:
        if len(packet) < 8 or checksum(packet) != 0:
            raise PacketError("invalid ICMP")
        if packet[0] != 8 or packet[1] != 0:
            return
        reply = b"\x00\x00\x00\x00" + packet[4:]
        reply = reply[:2] + struct.pack("!H", checksum(reply)) + reply[4:]
        self.send_ipv4(source_mac, source_ip, IP_ICMP, reply)
        self.log_event("icmp_echo", {"bytes": len(packet) - 8})

    def handle_udp(self, source_mac: bytes, source_ip: bytes,
                   destination_ip: bytes, packet: bytes) -> None:
        if len(packet) < 8:
            raise PacketError("truncated UDP")
        source_port, destination_port, length, received_sum = struct.unpack_from(
            "!HHHH", packet, 0
        )
        if length < 8 or length > len(packet):
            raise PacketError("invalid UDP length")
        segment = packet[:length]
        if received_sum and transport_checksum(source_ip, destination_ip,
                                                IP_UDP, segment) != 0xFFFF:
            # transport_checksum maps a computed zero to ffff; direct checksum
            # is clearer when validating a segment containing its checksum.
            pseudo = source_ip + destination_ip + struct.pack(
                "!BBH", 0, IP_UDP, len(segment)
            )
            if checksum(pseudo + segment) != 0:
                raise PacketError("bad UDP checksum")
        payload = segment[8:]
        if destination_port == 67:
            self.handle_dhcp(source_mac, source_ip, source_port, payload)
        elif destination_port == 53:
            self.handle_dns(source_mac, source_ip, source_port, payload)
        elif destination_port == 9000:
            response = udp(self.config.peer_ip, source_ip, 9000, source_port,
                           payload)
            self.send_ipv4(source_mac, source_ip, IP_UDP, response)
            self.log_event("udp_echo", {"bytes": len(payload)})

    def handle_dhcp(self, source_mac: bytes, source_ip: bytes,
                    source_port: int, packet: bytes) -> None:
        if source_port != 68 or len(packet) < 240:
            raise PacketError("invalid DHCP request")
        if packet[0:4] != b"\x01\x01\x06\x00" or packet[236:240] != DHCP_MAGIC:
            raise PacketError("invalid BOOTP/DHCP header")
        xid = packet[4:8]
        client_mac = packet[28:34]
        if client_mac != source_mac:
            raise PacketError("DHCP chaddr mismatch")
        options = parse_options(packet[240:])
        if len(options.get(53, b"")) != 1:
            raise PacketError("missing DHCP message type")
        request_type = options[53][0]
        if request_type not in (1, 3):
            return
        response_type = 2 if request_type == 1 else 5
        fixed = b"\x02\x01\x06\x00" + xid + b"\0\0\0\0"
        fixed += b"\0" * 4 + self.config.guest_ip + self.config.peer_ip
        fixed += b"\0" * 4 + client_mac.ljust(16, b"\0")
        fixed += b"\0" * 64 + b"\0" * 128
        lease = struct.pack("!I", self.config.lease_seconds)
        renew = struct.pack("!I", self.config.lease_seconds // 2)
        rebind = struct.pack("!I", self.config.lease_seconds * 7 // 8)
        options_out = encode_dhcp_option(53, bytes((response_type,)))
        options_out += encode_dhcp_option(54, self.config.peer_ip)
        options_out += encode_dhcp_option(51, lease)
        options_out += encode_dhcp_option(1, b"\xff\xff\xff\x00")
        options_out += encode_dhcp_option(3, self.config.peer_ip)
        options_out += encode_dhcp_option(6, self.config.peer_ip)
        options_out += encode_dhcp_option(58, renew)
        options_out += encode_dhcp_option(59, rebind) + b"\xff"
        response = fixed + DHCP_MAGIC + options_out
        destination_ip = b"\xff" * 4
        segment = udp(self.config.peer_ip, destination_ip, 67, 68, response)
        packet_out = ipv4(self.config.peer_ip, destination_ip, IP_UDP, segment,
                          self.ip_identification)
        self.ip_identification += 1
        self.emit_frame(ethernet(BROADCAST_MAC, self.config.peer_mac, ETH_IPV4,
                                 packet_out))
        self.log_event("dhcp_offer" if response_type == 2 else "dhcp_ack",
                       {"xid": xid.hex()})
        if response_type == 5 and self.config.inject_malformed and not self.malformed_injected:
            self.inject_malformed_frames(client_mac)

    def inject_malformed_frames(self, client_mac: bytes) -> None:
        """Inject independently observable checksum failures after DHCP bind."""

        bad_udp = udp(self.config.peer_ip, self.config.guest_ip, 41000, 41001,
                      b"bad-ip-checksum" * 2)
        bad_ip = bytearray(ipv4(self.config.peer_ip, self.config.guest_ip,
                                IP_UDP, bad_udp, self.ip_identification))
        self.ip_identification += 1
        bad_ip[10] ^= 0x80
        self.emit_frame(ethernet(client_mac, self.config.peer_mac, ETH_IPV4,
                                 bytes(bad_ip)))

        bad_tcp = bytearray(tcp(self.config.peer_ip, self.config.guest_ip,
                                41002, 8080, 7, 0, 0x10, 32768,
                                b"bad-tcp-checksum"))
        bad_tcp[16] ^= 0x40
        self.send_ipv4(client_mac, self.config.guest_ip, IP_TCP,
                       bytes(bad_tcp))
        self.malformed_injected = True
        self.log_event("malformed_injected", {"frames": 2})

    def handle_dns(self, source_mac: bytes, source_ip: bytes,
                   source_port: int, packet: bytes) -> None:
        if len(packet) < 12:
            raise PacketError("truncated DNS")
        transaction, flags, qdcount, _, _, _ = struct.unpack_from("!HHHHHH",
                                                                  packet, 0)
        if flags & 0x8000 or qdcount != 1:
            raise PacketError("invalid DNS query")
        name, end = decode_dns_name(packet, 12, allow_compression=False)
        if end + 4 > len(packet):
            raise PacketError("truncated DNS question")
        qtype, qclass = struct.unpack_from("!HH", packet, end)
        question = packet[12:end + 4]
        found = name == self.config.dns_name and qtype == 1 and qclass == 1
        response_flags = 0x8180 if found else 0x8183
        header = struct.pack("!HHHHHH", transaction, response_flags, 1,
                             1 if found else 0, 0, 0)
        answer = b""
        if found:
            answer = b"\xc0\x0c" + struct.pack("!HHIH", 1, 1, 60, 4)
            answer += self.config.dns_ip
        response = header + question + answer
        segment = udp(self.config.peer_ip, source_ip, 53, source_port, response)
        self.send_ipv4(source_mac, source_ip, IP_UDP, segment)
        self.log_event("dns_answer" if found else "dns_nxdomain",
                       {"name": name})

    def send_tcp(self, connection: TcpConnection, flags: int,
                 payload: bytes = b"", sequence: int | None = None) -> None:
        seq = connection.server_next if sequence is None else sequence
        segment = tcp(self.config.peer_ip, connection.client_ip, 8080,
                      connection.client_port, seq, connection.client_next,
                      flags, 32768, payload)
        self.send_ipv4(connection.client_mac, connection.client_ip, IP_TCP,
                       segment)

    def handle_tcp(self, source_mac: bytes, source_ip: bytes,
                   destination_ip: bytes, packet: bytes) -> None:
        if len(packet) < 20:
            raise PacketError("truncated TCP")
        source_port, destination_port, sequence, acknowledgement, offset_byte, flags, window, received_sum, _ = struct.unpack_from(
            "!HHIIBBHHH", packet, 0
        )
        header_length = (offset_byte >> 4) * 4
        if header_length < 20 or header_length > len(packet):
            raise PacketError("invalid TCP header length")
        pseudo = source_ip + destination_ip + struct.pack(
            "!BBH", 0, IP_TCP, len(packet)
        )
        if checksum(pseudo + packet) != 0:
            raise PacketError("bad TCP checksum")
        if destination_port != 8080:
            return
        key = (source_ip, source_port)
        connection = self.tcp_connections.get(key)
        if flags & 0x02:
            if flags & 0x10:
                raise PacketError("unexpected SYN+ACK")
            server_isn = (0x4E530000 + source_port) & 0xFFFFFFFF
            connection = TcpConnection(source_mac, source_ip, source_port,
                                       (sequence + 1) & 0xFFFFFFFF,
                                       (server_isn + 1) & 0xFFFFFFFF)
            self.tcp_connections[key] = connection
            self.send_tcp(connection, 0x12, sequence=server_isn)
            self.log_event("tcp_syn", {"port": source_port})
            return
        if connection is None:
            raise PacketError("TCP segment for unknown connection")
        if flags & 0x04:
            del self.tcp_connections[key]
            self.log_event("tcp_reset", {"port": source_port})
            return
        if flags & 0x10 and connection.state == "SYN_RCVD":
            if acknowledgement != connection.server_next:
                raise PacketError("bad handshake ACK")
            connection.state = "ESTABLISHED"
            self.log_event("tcp_established", {"port": source_port})
        payload = packet[header_length:]
        if payload:
            if sequence != connection.client_next:
                self.send_tcp(connection, 0x10)
                return
            if self.config.drop_first_tcp_data and not connection.dropped_data_once:
                connection.dropped_data_once = True
                self.log_event("tcp_injected_drop", {"port": source_port})
                return
            connection.client_next = (connection.client_next + len(payload)) & 0xFFFFFFFF
            body = b"northstar-network-ok\n"
            connection.response = (
                b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                + f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n".encode()
                + body
            )
            self.send_tcp_response(connection)
        if flags & 0x01:
            connection.client_next = (connection.client_next + 1) & 0xFFFFFFFF
            self.send_tcp(connection, 0x10)
            connection.state = "CLOSE_WAIT"
            self.log_event("tcp_client_fin", {"port": source_port})

    def send_tcp_response(self, connection: TcpConnection) -> None:
        response = connection.response
        start = connection.server_next
        if self.config.reorder_tcp_response and len(response) > 16:
            midpoint = len(response) // 2
            second = response[midpoint:]
            first = response[:midpoint]
            self.send_tcp(connection, 0x18, second, sequence=start + midpoint)
            self.send_tcp(connection, 0x18, first, sequence=start)
        else:
            self.send_tcp(connection, 0x18, response, sequence=start)
        if self.config.duplicate_tcp_response:
            self.send_tcp(connection, 0x18, response, sequence=start)
        connection.server_next = (start + len(response)) & 0xFFFFFFFF
        connection.response_sent = True
        self.log_event("http_response", {"bytes": len(response)})


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="127.0.0.1:10001")
    parser.add_argument("--qemu", default="127.0.0.1:10000")
    parser.add_argument("--peer-mac", type=parse_mac,
                        default=parse_mac("52:54:00:12:34:56"))
    parser.add_argument("--peer-ip", type=ip_bytes,
                        default=ip_bytes("10.0.2.2"))
    parser.add_argument("--guest-ip", type=ip_bytes,
                        default=ip_bytes("10.0.2.15"))
    parser.add_argument("--dns-name", default="northstar.test")
    parser.add_argument("--dns-ip", type=ip_bytes,
                        default=ip_bytes("10.0.2.2"))
    parser.add_argument("--lease-seconds", type=int, default=3600)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--events", type=Path)
    parser.add_argument("--drop-first-tcp-data", action="store_true")
    parser.add_argument("--duplicate-tcp-response", action="store_true")
    parser.add_argument("--reorder-tcp-response", action="store_true")
    parser.add_argument("--inject-malformed", action="store_true")
    parser.add_argument("--pcap", type=Path)
    parser.add_argument("--require", action="append", default=[],
                        help="event name required before successful exit")
    return parser


def split_endpoint(value: str) -> tuple[str, int]:
    host, separator, port = value.rpartition(":")
    if not separator or not host:
        raise ValueError(f"invalid endpoint: {value}")
    return host, int(port)


def run(arguments: argparse.Namespace) -> int:
    bind = split_endpoint(arguments.bind)
    qemu = split_endpoint(arguments.qemu)
    output = arguments.events.open("w", encoding="utf-8") if arguments.events else sys.stdout
    capture = PcapWriter(arguments.pcap) if arguments.pcap else None
    seen: set[str] = set()

    def log_event(name: str, fields: dict[str, object]) -> None:
        seen.add(name)
        record = {"event": name, "monotonic_ns": time.monotonic_ns(), **fields}
        print(json.dumps(record, sort_keys=True), file=output, flush=True)

    udp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_socket.bind(bind)
    udp_socket.setblocking(False)
    def emit_frame(frame: bytes) -> None:
        if capture is not None:
            capture.write(frame)
        log_event("ethernet_tx", {
            "bytes": len(frame),
            "sha256": hashlib.sha256(frame).hexdigest(),
        })
        udp_socket.sendto(frame, qemu)

    peer = NetworkPeer(
        PeerConfig(arguments.peer_mac, arguments.peer_ip, arguments.guest_ip,
                   arguments.dns_name.lower().rstrip("."), arguments.dns_ip,
                   arguments.lease_seconds, arguments.drop_first_tcp_data,
                   arguments.duplicate_tcp_response,
                   arguments.reorder_tcp_response,
                   arguments.inject_malformed),
        emit_frame, log_event
    )
    selector = selectors.DefaultSelector()
    selector.register(udp_socket, selectors.EVENT_READ)
    deadline = time.monotonic() + arguments.timeout
    try:
        log_event("peer_ready", {"bind": arguments.bind, "qemu": arguments.qemu})
        while time.monotonic() < deadline:
            if arguments.require and all(item in seen for item in arguments.require):
                log_event("peer_complete", {"required": arguments.require})
                return 0
            events = selector.select(min(0.25, max(0, deadline - time.monotonic())))
            for key, _ in events:
                frame, address = key.fileobj.recvfrom(65535)
                if capture is not None:
                    capture.write(frame)
                log_event("ethernet_rx", {"bytes": len(frame),
                                           "source": f"{address[0]}:{address[1]}",
                                           "sha256": hashlib.sha256(frame).hexdigest()})
                peer.handle_frame(frame)
        missing = sorted(set(arguments.require) - seen)
        log_event("peer_timeout", {"missing": missing})
        return 1
    finally:
        selector.close()
        udp_socket.close()
        if output is not sys.stdout:
            output.close()
        if capture is not None:
            capture.close()


def main() -> int:
    parser = make_parser()
    arguments = parser.parse_args()
    if arguments.timeout <= 0 or not (60 <= arguments.lease_seconds <= 86400):
        parser.error("timeout must be positive and lease must be 60..86400 seconds")
    try:
        return run(arguments)
    except (OSError, ValueError) as error:
        print(f"net_peer: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
