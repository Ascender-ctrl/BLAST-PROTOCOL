#!/usr/bin/env python3
"""
A fake sender that tests receiver cases the real sender rarely hits:
missing records, a repeated FILE_HDR, junk and duplicate packets,
very long missing lists, unsafe filenames, and the closing timer
when every DISCONNECT is lost.

Usage: python3 closing_timer_test.py <path-to-receiver> <port>
"""
import os, random, socket, struct, subprocess, sys, tempfile, time

HDR = "<BIH"                      # packet header, same layout as protocol.h
FILE_HDR, ACK, DATA, POLL, MISS, DISC = 1, 2, 3, 4, 5, 6
LINGER = 3.0                      # receiver's closing timer, in seconds

RECEIVER, PORT = sys.argv[1], int(sys.argv[2])
failures = 0


def check(name, cond, detail=""):
    global failures
    print(("PASS " if cond else "FAIL ") + name + (f"  ({detail})" if detail else ""))
    if not cond:
        failures += 1


def file_hdr(size, rs, m, name):
    body = struct.pack("<QII256s", size, rs, m, name.encode())
    return struct.pack(HDR, FILE_HDR, 0, len(body)) + body


def data_pkt(blast, recs):
    body = struct.pack("<B", len(recs))
    for rid, d in recs:
        body += struct.pack("<IH", rid, len(d)) + d.ljust(1024, b"\0")
    return struct.pack(HDR, DATA, blast, len(body)) + body


def ctrl(t, blast):
    return struct.pack(HDR, t, blast, 0)


class Session:
    def __init__(self, port):
        self.out = tempfile.mkdtemp()
        self.proc = subprocess.Popen([RECEIVER, str(port), self.out],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.proc.stdout.readline()       # wait until the receiver is listening
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.settimeout(0.5)
        self.dst = ("127.0.0.1", port)

    def send(self, b):
        self.s.sendto(b, self.dst)

    def recv(self, bufsize=2048):
        try:
            d, _ = self.s.recvfrom(bufsize)
            return d
        except socket.timeout:
            return None

    def poll(self, blast):
        self.send(ctrl(POLL, blast))
        d = self.recv(65536)
        if d is None:
            return None, None
        t, b, _ = struct.unpack_from(HDR, d)
        n, = struct.unpack_from("<H", d, 7)
        ids = list(struct.unpack_from(f"<{n}I", d, 9))
        return (t, b, ids), len(d)

    def finish(self, timeout=15):
        try:
            out, _ = self.proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            out, _ = self.proc.communicate()
            return None, out
        return self.proc.returncode, out


def records_of(data, rs, m, blast):
    first = blast * m
    out = []
    for i in range(m):
        off = (first + i) * rs
        if off >= len(data):
            break
        out.append((i, data[off:off + rs]))
    return out


def send_records(sess, blast, recs):
    for i in range(0, len(recs), 16):
        sess.send(data_pkt(blast, recs[i:i + 16]))
        time.sleep(0.0005)


# Test A: a full transfer with problems added along the way
rs, m = 512, 64
payload = os.urandom(rs * m + 300)          # 2 blasts, short last record
A = Session(PORT)

A.send(file_hdr(len(payload), rs, m, "a.bin"))
r = A.recv()
check("handshake: FILE_HDR_ACK received", r is not None and r[0] == ACK)
A.send(file_hdr(len(payload), rs, m, "a.bin"))          # resend as if the ACK was lost
r = A.recv()
check("duplicate FILE_HDR is re-ACKed", r is not None and r[0] == ACK)

b0 = records_of(payload, rs, m, 0)
send_records(A, 0, [x for x in b0 if x[0] % 2 == 0])
resp, _ = A.poll(0)
odd = [i for i in range(m) if i % 2 == 1]
check("selective repeat: REC_MISS lists exactly the odd records",
      resp is not None and resp[0] == MISS and resp[2] == odd,
      f"got {len(resp[2]) if resp else 'nothing'} ids")

# bad packets the receiver should ignore
A.send(b"\x03")                                         # too short
A.send(data_pkt(0, [(m + 5, b"x" * rs)]))               # bad record ID
A.send(data_pkt(0, [(1, b"short")]))                    # wrong length
A.send(data_pkt(7, [(0, b"y" * rs)]))                   # wrong blast
A.send(ctrl(99, 0))                                     # unknown type
stranger = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
stranger.sendto(data_pkt(0, [(1, b"Z" * rs)]), A.dst)   # from another sender
send_records(A, 0, b0)                                  # resend all (evens are duplicates)

resp, _ = A.poll(0)
check("blast 0 complete after resend", resp is not None and resp[2] == [])
send_records(A, 0, b0[:4])                              # old blast's data arrives late
b1 = records_of(payload, rs, m, 1)
send_records(A, 1, b1)
resp, _ = A.poll(1)
check("last blast (short tail record) complete", resp is not None and resp[2] == [])

time.sleep(LINGER - 1.2)                                # poll again as if the reply was lost
resp, _ = A.poll(1)
check("IS_BLAST_OVER retried during linger is re-answered",
      resp is not None and resp[0] == MISS and resp[1] == 1 and resp[2] == [])

t0 = time.time()                                        # never send DISCONNECT
code, out = A.finish()
elapsed = time.time() - t0
got = open(os.path.join(A.out, "a.bin"), "rb").read() if code == 0 else b""
check("closing timer exits cleanly with every DISCONNECT lost",
      code == 0 and LINGER - 0.5 <= elapsed <= LINGER + 1.5, f"exit={code}, {elapsed:.1f}s")
check("file intact despite duplicates/junk/stale/stranger packets", got == payload)
check("receiver reports no DISCONNECT seen", "closing timer expired" in out)

# Test B: a blast with 600 missing records, then a normal DISCONNECT
rs, m = 256, 600
payload = os.urandom(rs * m)
B = Session(PORT + 1)
B.send(file_hdr(len(payload), rs, m, "b.bin"))
B.recv()
resp, size = B.poll(0)                                  # nothing sent yet
check("REC_MISS capped to fit sender's 2048-byte buffer",
      resp is not None and size <= 2048 and resp[2] == list(range(len(resp[2]))),
      f"{len(resp[2]) if resp else 0} ids, {size} bytes")
send_records(B, 0, records_of(payload, rs, m, 0)[:550])
resp, _ = B.poll(0)
check("remaining ids reported on next poll", resp is not None and resp[2] == list(range(550, 600)))
send_records(B, 0, records_of(payload, rs, m, 0)[550:])
resp, _ = B.poll(0)
B.send(ctrl(DISC, 1))
t0 = time.time()
code, out = B.finish()
got = open(os.path.join(B.out, "b.bin"), "rb").read() if code == 0 else b""
check("DISCONNECT -> linger -> clean exit, file intact",
      code == 0 and got == payload and "DISCONNECT Seen  : yes" in out, f"{time.time() - t0:.1f}s")

# Test C: unsafe filename
C = Session(PORT + 2)
C.send(file_hdr(10, 256, 10, "../evil.bin"))
r = C.recv()
check("unsafe filename '../evil.bin' rejected (no ACK)", r is None)
C.proc.terminate()
C.finish()

sys.exit(1 if failures else 0)
