#!/usr/bin/env python3
"""Contra NES-X v3 directory, UDP rendezvous and bounded two-player relay.
Public rooms support optional four-digit passcodes. No player accounts or ROMs.
"""
import argparse
import hashlib
import hmac
import secrets
import socket
import struct
import time
from dataclasses import dataclass

CONTROL_SIZE = 96
GAME_SIZE = 144
PORT = 6475
CHALLENGE, REGISTER, REGISTERED, LIST, ROOMS, JOIN, MATCH, KEEP, LEAVE, ERROR = range(1, 11)
ERR_ROOM, ERR_PASSCODE, ERR_RATE, ERR_FORMAT = range(1, 5)
TTL = 30.0
MAX_CLIENTS = 256
MAX_ROOMS = 128

def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]

def put(data, offset, value):
    struct.pack_into('<I', data, offset, value)

@dataclass
class Client:
    address: tuple
    nonce: int
    cookie: bytes
    identity: tuple
    last: float
    room: int = 0
    role: int = 0
    join_attempt: int = 0

@dataclass
class Room:
    number: int
    host: bytes
    guest: bytes = b''
    token: bytes = b''
    passcode_hash: bytes = b''

class Directory:
    def __init__(self, clock=time.monotonic, secret=None):
        self.clock = clock
        self.secret = secret or secrets.token_bytes(32)
        self.clients = {}
        self.rooms = {}
        self.limits = {}
        self.last_prune = self.clock()

    def cookie(self, address, nonce, bucket):
        body = f'{address[0]}:{address[1]}:{nonce}:{bucket}'.encode('ascii')
        return hmac.new(self.secret, body, hashlib.sha256).digest()[:16]

    def response(self, client, kind):
        data = bytearray(CONTROL_SIZE)
        data[:4] = b'CDR3'; data[4] = kind; data[5] = 3; data[7] = client.role
        put(data, 8, client.nonce); put(data, 12, client.room)
        for offset, value in zip((16, 20, 24), client.identity): put(data, offset, value)
        data[28:44] = client.cookie
        put(data, 92, client.join_attempt)
        return data

    def error(self, client, reason):
        data = self.response(client, ERROR); data[6] = reason
        return [(client.address, bytes(data))]

    def passcode_hash(self, number, code):
        return hmac.new(self.secret, b'room:' + struct.pack('<I', number) + code,
                        hashlib.sha256).digest()

    def allow(self, key, maximum, now, period=1.0):
        if key not in self.limits and len(self.limits) >= 4096: return False
        stamp, count, duration = self.limits.get(key, (now, 0, period))
        if now - stamp >= duration: stamp, count = now, 0
        if count >= maximum: return False
        self.limits[key] = (stamp, count + 1, duration)
        return True

    def remove(self, cookie):
        client = self.clients.pop(cookie, None)
        if client and client.room:
            room = self.rooms.get(client.room)
            if room:
                # A departed participant ends the session; never insert a new
                # player into an existing game's input timeline.
                self.rooms.pop(room.number, None)
                for other_cookie in (room.host, room.guest):
                    other = self.clients.get(other_cookie)
                    if other: other.room = 0

    def prune(self, now):
        if now - self.last_prune < 1.0: return
        self.last_prune = now
        for cookie, client in list(self.clients.items()):
            if now - client.last > TTL: self.remove(cookie)
        self.limits = {k: v for k, v in self.limits.items() if now - v[0] < max(2.0, v[2])}

    def matches(self, room):
        a, b = self.clients.get(room.host), self.clients.get(room.guest)
        if not a or not b: return []
        result = []
        for client, peer in ((a, b), (b, a)):
            data = self.response(client, MATCH)
            data[44:48] = socket.inet_aton(peer.address[0])
            struct.pack_into('<H', data, 48, peer.address[1])
            data[64:80] = room.token
            result.append((client.address, bytes(data)))
        return result

    def handle(self, data, address):
        now = self.clock()
        self.prune(now)
        if not self.allow(('global',), 12000, now): return []
        if len(data) == 20 + GAME_SIZE and data[:4] == b'CRL2':
            cookie = data[4:20]; client = self.clients.get(cookie)
            if not client or client.address != address: return []
            room = self.rooms.get(client.room)
            if not room or not room.guest: return []
            payload = data[20:]
            if (payload[:4] != b'CNX2' or payload[120:136] != room.token or
                (u32(payload, 16), u32(payload, 20), u32(payload, 116)) != client.identity): return []
            if not self.allow(('relay', cookie), 240, now): return []
            peer = self.clients.get(room.guest if client.role == 0 else room.host)
            if not peer: return []
            client.last = now
            return [(peer.address, b'CRL2' + peer.cookie + payload)]
        if len(data) != CONTROL_SIZE or data[:4] != b'CDR3' or data[5] != 3: return []
        if not self.allow(('control', address[0]), 40, now): return []
        kind, nonce = data[4], u32(data, 8)
        if kind not in (REGISTER, LIST, JOIN, KEEP, LEAVE) or not nonce: return []
        cookie = data[28:44]
        identity = tuple(u32(data, off) for off in (16, 20, 24))
        client = self.clients.get(cookie)
        if client:
            if client.address != address or client.nonce != nonce or client.identity != identity: return []
        else:
            bucket = int(now // 60)
            valid = any(hmac.compare_digest(cookie, self.cookie(address, nonce, b)) for b in (bucket, bucket - 1))
            if not valid:
                if not self.allow(('challenge', address[0]), 10, now): return []
                temp = Client(address, nonce, self.cookie(address, nonce, bucket), identity, now)
                return [(address, bytes(self.response(temp, CHALLENGE)))]
            if len(self.clients) >= MAX_CLIENTS: return []
            # One client for an endpoint; a restart invalidates its old room.
            for old, value in list(self.clients.items()):
                if value.address == address: self.remove(old)
            client = Client(address, nonce, cookie, identity, now)
            self.clients[cookie] = client
        client.last = now
        if kind == LEAVE:
            self.remove(cookie); return []
        if kind == JOIN:
            attempt = u32(data, 92)
            if attempt < client.join_attempt: return []
            client.join_attempt = attempt
        if kind == REGISTER:
            if not client.room:
                locked, code = data[84], data[85:89]
                if locked > 1 or (locked and (len(code) != 4 or any(c < 48 or c > 57 for c in code))):
                    return self.error(client, ERR_FORMAT)
                if len(self.rooms) >= MAX_ROOMS: return [(address, bytes(self.response(client, ERROR)))]
                number = secrets.randbits(32) or 1
                while number in self.rooms: number = secrets.randbits(32) or 1
                client.room = number; client.role = 0
                self.rooms[number] = Room(number, cookie, passcode_hash=self.passcode_hash(number, code) if locked else b'')
            room = self.rooms.get(client.room)
            if not room or room.host != cookie: return [(address, bytes(self.response(client, ERROR)))]
            return [(address, bytes(self.response(client, REGISTERED)))] + self.matches(room)
        if kind == LIST:
            data = self.response(client, ROOMS)
            rooms = [room.number for room in self.rooms.values()
                     if not room.guest and room.host != cookie and
                     self.clients[room.host].identity == identity][:8]
            data[6] = len(rooms)
            for i, number in enumerate(rooms):
                put(data, 52 + 4 * i, number)
                data[84 + i] = bool(self.rooms[number].passcode_hash)
            return [(address, bytes(data))]
        if kind == JOIN:
            number = u32(data, 12); room = self.rooms.get(number)
            host = self.clients.get(room.host) if room else None
            if (not room or not host or host.identity != identity or room.host == cookie or
                (room.guest and room.guest != cookie) or (client.room and client.room != number)):
                return [(address, bytes(self.response(client, ERROR)))]
            # Retry by the same admitted guest is idempotent. Other clients
            # receive no peer endpoint or relay token before verification.
            if room.passcode_hash and room.guest != cookie:
                if not self.allow(('passcode', address[0]), 5, now, period=10.0):
                    return self.error(client, ERR_RATE)
                code = data[85:89]
                if (data[84] != 1 or any(c < 48 or c > 57 for c in code) or
                    not hmac.compare_digest(room.passcode_hash, self.passcode_hash(number, code))):
                    return self.error(client, ERR_PASSCODE)
            client.room = number; client.role = 1
            if not room.guest:
                room.guest = cookie; room.token = secrets.token_bytes(16)
            return self.matches(room)
        if kind == KEEP:
            room = self.rooms.get(client.room)
            if not room: return [(address, bytes(self.response(client, ERROR)))]
            return self.matches(room) or [(address, bytes(self.response(client, REGISTERED)))]
        return []

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bind', default='0.0.0.0')
    parser.add_argument('--port', type=int, default=PORT)
    args = parser.parse_args()
    directory = Directory()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((args.bind, args.port)); sock.settimeout(1.0)
        print(f'Contra directory/relay listening on {args.bind}:{args.port}', flush=True)
        while True:
            try: data, address = sock.recvfrom(2048)
            except socket.timeout:
                directory.prune(directory.clock()); continue
            for destination, response in directory.handle(data, address):
                try: sock.sendto(response, destination)
                except OSError: pass

if __name__ == '__main__': main()
