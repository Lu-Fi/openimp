#!/usr/bin/env python3
"""guest_console.py: tiny helper for the QEMU guest console (tcp chardev, one client at a time).
  guest_console.py PORT put LOCAL REMOTE        copy a text/script file in (base64 chunks)
  guest_console.py PORT run 'CMD' [TIMEOUT_S]   run CMD, print its output, wait for it to finish
  guest_console.py PORT stream 'CMD' [TIMEOUT_S] [END_MARKER]  run CMD and copy its output live until END_MARKER
  guest_console.py PORT waitsh [TIMEOUT_S]      wait until the console answers (shell prompt)"""
import base64, re, socket, sys, time

ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")


class Con:
    def __init__(self, port, tries=60):
        for _ in range(tries):
            try:
                self.s = socket.create_connection(("127.0.0.1", port))
                break
            except OSError:
                time.sleep(1)
        else:
            raise SystemExit("console %d not reachable" % port)
        self.s.settimeout(0.5)
        self.buf = b""

    def send(self, txt):
        self.s.sendall(txt.encode())

    def pump(self):
        try:
            d = self.s.recv(65536)
            if d:
                self.buf += d
                return d
        except socket.timeout:
            pass
        return b""

    def run(self, cmd, timeout=60, live=False, end=None, intr=True):
        mark = "END%d" % int(time.time() * 1000)
        self.buf = b""
        if intr:   # not during U-Boot's autoboot countdown: ^C there stops the boot
            self.send("\x03\n")
            time.sleep(0.3)
        self.buf = b""
        self.send("%s; echo %s$?\n" % (cmd, mark))
        t0 = time.time()
        pos = 0
        while time.time() - t0 < timeout:
            self.pump()
            txt = ANSI.sub("", self.buf.decode("latin1")).replace("\r", "")
            if live:
                sys.stdout.write(txt[pos:]); sys.stdout.flush(); pos = len(txt)
            if re.search(r"(^|\n)%s\d+" % mark, txt):
                break
            if end and end in txt[txt.find("\n") + 1:]:
                break
        txt = ANSI.sub("", self.buf.decode("latin1")).replace("\r", "")
        return txt


def main():
    port, op = int(sys.argv[1]), sys.argv[2]
    c = Con(port)
    if op == "waitsh":
        t0 = time.time(); to = float(sys.argv[3]) if len(sys.argv) > 3 else 300
        while time.time() - t0 < to:   # listen only until the shell prompt: any key stops U-Boot's autoboot
            c.pump()
            t = ANSI.sub("", c.buf.decode("latin1"))
            if "login:" in t or "root@" in t:
                break
        while time.time() - t0 < to:
            if "AEOK" in c.run("echo AE''OK; uname -s", 8) and "Linux" in c.buf.decode("latin1"):
                print("shell up after %ds" % (time.time() - t0)); return
        raise SystemExit("no shell")
    if op == "put":
        data = base64.b64encode(open(sys.argv[3], "rb").read()).decode()
        c.run("rm -f %s.b64" % sys.argv[4], 10)
        for i in range(0, len(data), 240):   # one chunk per command: the guest tty drops input sent faster than it reads
            c.run("echo %s >> %s.b64" % (data[i:i + 240], sys.argv[4]), 20)
        out = c.run("base64 -d %s.b64 > %s; rm -f %s.b64; chmod +x %s; wc -c < %s" % ((sys.argv[4],) * 5), 30)
        print(out.strip().splitlines()[-2:])
    elif op in ("run", "stream"):
        to = float(sys.argv[4]) if len(sys.argv) > 4 else 60
        end = sys.argv[5] if len(sys.argv) > 5 else None
        out = c.run(sys.argv[3], to, live=(op == "stream"), end=end)
        if op == "run":
            print(out)


main()
