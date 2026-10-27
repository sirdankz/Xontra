"""Run without building the game: python -m unittest discover -s . -p test_server.py"""
import secrets
import socket
import struct
import subprocess
import sys
import time
import unittest
from pathlib import Path
from contra_server import *
import contra_server as server_module
from unittest.mock import patch

class ServerTests(unittest.TestCase):
    def setUp(self):
        self.now = 1000.0
        self.server = Directory(clock=lambda: self.now, secret=b'test-secret-only')
        self.host = ('198.51.100.1', 6474)
        self.guest = ('203.0.113.2', 6474)

    def request(self, kind, nonce, cookie=b'\0'*16, room=0, identity=(123, 456, 12000), passcode=None, attempt=0):
        p = bytearray(CONTROL_SIZE); p[:4]=b'CDR3';p[4]=kind;p[5]=3
        put(p,8,nonce);put(p,12,room)
        for offset,value in zip((16,20,24),identity):put(p,offset,value)
        p[28:44]=cookie
        if passcode is not None: p[84]=1;p[85:89]=passcode.encode('ascii')
        put(p,92,attempt)
        return bytes(p)

    def authenticated(self, kind, address, nonce, room=0, identity=(123,456,12000), **options):
        p=self.request(kind,nonce,room=room,identity=identity,**options)
        response=self.server.handle(p,address)
        self.assertEqual(response[0][1][4],CHALLENGE)
        cookie=response[0][1][28:44]
        result=self.server.handle(self.request(kind,nonce,cookie,room,identity,**options),address)
        return cookie,result

    def pair(self):
        hc,replies=self.authenticated(REGISTER,self.host,1)
        room=u32(replies[0][1],12)
        gc,replies=self.authenticated(JOIN,self.guest,2,room)
        self.assertEqual([p[4] for _,p in replies],[MATCH,MATCH])
        return hc,gc,room,replies[0][1][64:80]

    def test_stateless_challenge_creates_no_room(self):
        r=self.server.handle(self.request(REGISTER,1),self.host)
        self.assertEqual(len(r[0][1]),CONTROL_SIZE)
        self.assertFalse(self.server.rooms);self.assertFalse(self.server.clients)

    def test_registration_retry_is_idempotent(self):
        cookie,r=self.authenticated(REGISTER,self.host,1);room=u32(r[0][1],12)
        r=self.server.handle(self.request(REGISTER,1,cookie),self.host)
        self.assertEqual(u32(r[0][1],12),room);self.assertEqual(len(self.server.rooms),1)

    def test_matching_builds_only(self):
        self.authenticated(REGISTER,self.host,1)
        _,r=self.authenticated(LIST,self.guest,2,identity=(123,999,12000))
        self.assertEqual(r[0][1][6],0)

    def test_room_list_and_join_retry(self):
        hc,r=self.authenticated(REGISTER,self.host,1);room=u32(r[0][1],12)
        gc,r=self.authenticated(LIST,self.guest,2)
        self.assertEqual(r[0][1][6],1);self.assertEqual(u32(r[0][1],52),room)
        r=self.server.handle(self.request(JOIN,2,gc,room),self.guest)
        token=r[0][1][64:80]
        r=self.server.handle(self.request(JOIN,2,gc,room),self.guest)
        self.assertEqual(r[0][1][64:80],token)
        self.assertEqual(socket.inet_ntoa(r[0][1][44:48]),self.guest[0])

    def test_third_player_cannot_join(self):
        _,_,room,_=self.pair()
        _,r=self.authenticated(JOIN,('192.0.2.3',6474),3,room)
        self.assertEqual(r[0][1][4],ERROR)

    def game(self,token):
        p=bytearray(GAME_SIZE);p[:4]=b'CNX2';p[4]=5
        put(p,16,123);put(p,20,456);put(p,116,12000);p[120:136]=token
        return bytes(p)

    def test_relay_destination_and_cookie_rewrite(self):
        hc,gc,_,token=self.pair();payload=self.game(token)
        r=self.server.handle(b'CRL2'+hc+payload,self.host)
        self.assertEqual(r,[(self.guest,b'CRL2'+gc+payload)])

    def test_relay_rejects_wrong_endpoint_cookie_token_or_build(self):
        hc,_,_,token=self.pair();payload=self.game(token)
        self.assertFalse(self.server.handle(b'CRL2'+hc+payload,self.guest))
        self.assertFalse(self.server.handle(b'CRL2'+b'x'*16+payload,self.host))
        self.assertFalse(self.server.handle(b'CRL2'+hc+self.game(b'x'*16),self.host))
        p=bytearray(payload);put(p,20,999)
        self.assertFalse(self.server.handle(b'CRL2'+hc+p,self.host))

    def test_leave_ends_room_for_both(self):
        hc,gc,_,token=self.pair()
        self.server.handle(self.request(LEAVE,2,gc),self.guest)
        self.assertFalse(self.server.rooms)
        self.assertFalse(self.server.handle(b'CRL2'+hc+self.game(token),self.host))
        r=self.server.handle(self.request(KEEP,1,hc),self.host)
        self.assertEqual(r[0][1][4],ERROR)

    def test_expiry_and_stale_cookie(self):
        hc,_,_,token=self.pair();self.now+=31;self.server.prune(self.now)
        self.assertFalse(self.server.rooms)
        self.assertFalse(self.server.handle(b'CRL2'+hc+self.game(token),self.host))
        self.now+=120
        r=self.server.handle(self.request(REGISTER,1,hc),self.host)
        self.assertEqual(r[0][1][4],CHALLENGE)

    def test_challenge_is_bound_to_source_and_nonce(self):
        r=self.server.handle(self.request(REGISTER,1),self.host);c=r[0][1][28:44]
        for address,nonce in [(self.guest,1),(self.host,2)]:
            r=self.server.handle(self.request(REGISTER,nonce,c),address)
            self.assertEqual(r[0][1][4],CHALLENGE)
        self.assertFalse(self.server.rooms)

    def test_malformed_sizes_and_unknown_packets(self):
        p=self.request(REGISTER,1)
        for data in [b'',p[:50],p+b'x',b'X'*96,b'CRL2'+b'x'*16,b'CNX2'+b'x'*140]:
            self.assertFalse(self.server.handle(data,self.host))

    def test_challenge_rate_limit_and_no_amplification(self):
        p=self.request(REGISTER,1)
        replies=[self.server.handle(p,self.host) for _ in range(100)]
        self.assertLessEqual(sum(bool(r) for r in replies),10)
        self.assertTrue(all(len(data)<=len(p) for r in replies for _,data in r))

    def test_relay_rate_limit(self):
        hc,_,_,token=self.pair();data=b'CRL2'+hc+self.game(token)
        self.assertEqual(sum(bool(self.server.handle(data,self.host)) for _ in range(260)),240)
        self.now+=1.01;self.assertTrue(self.server.handle(data,self.host))

    def test_room_capacity(self):
        self.authenticated(REGISTER,self.host,1)
        with patch.object(server_module,'MAX_ROOMS',1):
            _,r=self.authenticated(REGISTER,self.guest,2)
        self.assertEqual(r[0][1][4],ERROR)
        self.assertEqual(len(self.server.rooms),1)

    def test_rate_cache_is_bounded(self):
        for i in range(5000):self.server.allow(('synthetic',i),1,self.now)
        self.assertLessEqual(len(self.server.limits),4096)
        self.now+=3;self.server.prune(self.now)
        self.assertFalse(self.server.limits)

    def locked_room(self, code='0123'):
        hc,replies=self.authenticated(REGISTER,self.host,1,passcode=code)
        room=u32(replies[0][1],12)
        gc,replies=self.authenticated(LIST,self.guest,2)
        return hc,gc,room,replies[0][1]

    def test_passcode_room_is_public_without_exposing_code(self):
        _,_,room,listing=self.locked_room()
        self.assertEqual(listing[6],1);self.assertEqual(u32(listing,52),room)
        self.assertEqual(listing[84],1);self.assertEqual(listing[85:92],bytes(7))
        self.assertEqual(listing[44:52],bytes(8));self.assertEqual(listing[64:80],bytes(16))
        self.assertEqual(len(self.server.rooms[room].passcode_hash),32)

    def test_wrong_or_missing_code_does_not_admit_or_reveal_peer(self):
        _,gc,room,_=self.locked_room()
        for attempt,code in enumerate([None,'9999','A123'],1):
            replies=self.server.handle(self.request(JOIN,2,gc,room,passcode=code,attempt=attempt),self.guest)
            packet=replies[0][1]
            self.assertEqual((packet[4],packet[6]),(ERROR,ERR_PASSCODE))
            self.assertEqual(packet[44:52],bytes(8));self.assertEqual(packet[64:80],bytes(16))
            self.assertEqual(u32(packet,92),attempt)
            self.assertFalse(self.server.rooms[room].guest)
            self.assertEqual(self.server.clients[gc].room,0)
            self.assertFalse(self.server.handle(b'CRL2'+gc+self.game(bytes(16)),self.guest))

    def test_correct_code_with_leading_zero_matches(self):
        _,gc,room,_=self.locked_room()
        replies=self.server.handle(self.request(JOIN,2,gc,room,passcode='0123',attempt=1),self.guest)
        self.assertEqual([p[4] for _,p in replies],[MATCH,MATCH])
        self.assertEqual(u32(replies[1][1],92),1)
        self.assertEqual(self.server.rooms[room].guest,gc)

    def test_all_zero_passcode_still_requires_code(self):
        _,gc,room,_=self.locked_room('0000')
        result=self.server.handle(self.request(JOIN,2,gc,room,attempt=1),self.guest)
        self.assertEqual(result[0][1][6],ERR_PASSCODE)
        result=self.server.handle(self.request(JOIN,2,gc,room,passcode='0000',attempt=2),self.guest)
        self.assertEqual([p[4] for _,p in result],[MATCH,MATCH])

    def test_passcode_retries_by_admitted_guest_are_idempotent(self):
        _,gc,room,_=self.locked_room()
        first=self.server.handle(self.request(JOIN,2,gc,room,passcode='0123',attempt=1),self.guest)
        again=self.server.handle(self.request(JOIN,2,gc,room,attempt=1),self.guest)
        self.assertEqual(first,again)

    def test_passcode_attempt_limit_survives_prune_and_nonce_changes(self):
        _,gc,room,_=self.locked_room()
        for attempt in range(1,6):
            r=self.server.handle(self.request(JOIN,2,gc,room,passcode='9999',attempt=attempt),self.guest)
            self.assertEqual(r[0][1][6],ERR_PASSCODE)
        self.now+=3;self.server.prune(self.now)
        gc,_=self.authenticated(LIST,self.guest,99)
        r=self.server.handle(self.request(JOIN,99,gc,room,passcode='0123',attempt=1),self.guest)
        self.assertEqual(r[0][1][6],ERR_RATE)
        self.now+=7.01
        r=self.server.handle(self.request(JOIN,99,gc,room,passcode='0123',attempt=2),self.guest)
        self.assertEqual([p[4] for _,p in r],[MATCH,MATCH])

    def test_register_retry_cannot_change_room_code(self):
        hc,gc,room,_=self.locked_room()
        self.server.handle(self.request(REGISTER,1,hc,passcode='9999'),self.host)
        r=self.server.handle(self.request(JOIN,2,gc,room,passcode='9999',attempt=1),self.guest)
        self.assertEqual(r[0][1][6],ERR_PASSCODE)
        r=self.server.handle(self.request(JOIN,2,gc,room,passcode='0123',attempt=2),self.guest)
        self.assertEqual(r[0][1][4],MATCH)

    def test_malformed_passcode_cannot_create_room(self):
        hc,_=self.authenticated(LIST,self.host,1)
        for flag,code in [(1,b'A000'),(2,b'1234')]:
            request=bytearray(self.request(REGISTER,1,hc));request[84]=flag;request[85:89]=code
            r=self.server.handle(bytes(request),self.host)
            self.assertEqual(r[0][1][6],ERR_FORMAT)
            self.assertFalse(self.server.rooms)

    def test_old_protocol_is_rejected(self):
        request=bytearray(self.request(REGISTER,1));request[:4]=b'CDR2';request[5]=2
        self.assertFalse(self.server.handle(bytes(request),self.host))
        self.assertFalse(self.server.rooms)

    def test_stale_join_attempt_is_ignored(self):
        _,gc,room,_=self.locked_room()
        self.server.handle(self.request(JOIN,2,gc,room,passcode='9999',attempt=2),self.guest)
        self.assertFalse(self.server.handle(self.request(JOIN,2,gc,room,passcode='0123',attempt=1),self.guest))
        r=self.server.handle(self.request(JOIN,2,gc,room,passcode='0123',attempt=3),self.guest)
        self.assertEqual(r[1][1][4],MATCH);self.assertEqual(u32(r[1][1],92),3)

    def test_local_udp_process(self):
        # Exercise the actual process/socket path without any console build.
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as reserve:
            reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
        process=subprocess.Popen([sys.executable,str(Path(__file__).with_name('contra_server.py')),
                                  '--bind','127.0.0.1','--port',str(port)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as a,socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as b:
                a.settimeout(.2);b.settimeout(1);target=('127.0.0.1',port)
                deadline=time.monotonic()+5
                while True:
                    a.sendto(self.request(REGISTER,101,passcode='0123'),target)
                    try:r,_=a.recvfrom(512);break
                    except (socket.timeout, ConnectionResetError):
                        if time.monotonic()>deadline:raise
                cookie=r[28:44];a.sendto(self.request(REGISTER,101,cookie,passcode='0123'),target)
                r,_=a.recvfrom(512);room=u32(r,12)
                b.sendto(self.request(JOIN,102,room=room),target);r,_=b.recvfrom(512)
                bc=r[28:44];b.sendto(self.request(JOIN,102,bc,room,passcode='9999',attempt=1),target)
                denied,_=b.recvfrom(512);self.assertEqual((denied[4],denied[6]),(ERROR,ERR_PASSCODE))
                b.sendto(self.request(JOIN,102,bc,room,passcode='0123',attempt=2),target)
                match,_=a.recvfrom(512);other,_=b.recvfrom(512)
                self.assertEqual(match[4],MATCH);self.assertEqual(other[4],MATCH)
                payload=self.game(match[64:80]);a.sendto(b'CRL2'+cookie+payload,target)
                relayed,_=b.recvfrom(512);self.assertEqual(relayed,b'CRL2'+bc+payload)
        finally:
            process.terminate();process.communicate(timeout=5)

if __name__=='__main__':unittest.main()
