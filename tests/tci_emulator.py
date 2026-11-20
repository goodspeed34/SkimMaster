"""A minimal TCI server: WebSocket handshake, text commands and IQ frames."""
import base64
import hashlib
import socket
import struct

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class TciServer:
    def __init__(self, receivers=2, rate=48000, dds=(7000000, 7100000)):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.listener.settimeout(10)
        self.port = self.listener.getsockname()[1]
        self.state = [f"protocol:ExpertSDR3,1.8;", "device:SunSDR2PRO;",
                      f"trx_count:{receivers};", f"iq_samplerate:{rate};"]
        self.state += [f"dds:{rx},{hz};" for rx, hz in enumerate(dds[:receivers])]
        self.state.append("ready;")
        self.client = None
        self.received = []

    def accept(self):
        self.client, _ = self.listener.accept()
        self.client.settimeout(10)
        request = b""
        while b"\r\n\r\n" not in request:
            chunk = self.client.recv(4096)
            if not chunk:
                raise ConnectionError("client closed during handshake")
            request += chunk
        key = next(line.split(b":", 1)[1].strip()
                   for line in request.split(b"\r\n")
                   if line.lower().startswith(b"sec-websocket-key:"))
        accept = base64.b64encode(hashlib.sha1(key + GUID).digest())
        self.client.sendall(b"HTTP/1.1 101 Switching Protocols\r\n"
                            b"Upgrade: websocket\r\nConnection: Upgrade\r\n"
                            b"Sec-WebSocket-Accept: " + accept + b"\r\n\r\n")
        for command in self.state:
            self.text(command)

    def frame(self, opcode, payload):
        header = bytes([0x80 | opcode])
        if len(payload) < 126:
            header += bytes([len(payload)])
        elif len(payload) < 65536:
            header += bytes([126]) + struct.pack(">H", len(payload))
        else:
            header += bytes([127]) + struct.pack(">Q", len(payload))
        self.client.sendall(header + payload)

    def text(self, command):
        self.frame(1, command.encode())

    def iq(self, receiver, floats, rate=48000):
        """Send interleaved float32 I/Q as one IQ stream frame."""
        header = struct.pack("<16I", receiver, rate, 3, 0, 0, len(floats), 0,
                             *([0] * 9))
        self.frame(2, header + floats.tobytes())

    def _exact(self, count):
        data = b""
        while len(data) < count:
            chunk = self.client.recv(count - len(data))
            if not chunk:
                raise ConnectionError("client closed")
            data += chunk
        return data

    def read_command(self):
        """Read one client text frame (masked) and return its commands."""
        first, second = self._exact(2)
        size = second & 0x7F
        if size == 126:
            size = struct.unpack(">H", self._exact(2))[0]
        elif size == 127:
            size = struct.unpack(">Q", self._exact(8))[0]
        mask = self._exact(4) if second & 0x80 else bytes(4)
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(self._exact(size)))
        if first & 0x0F == 8:
            raise ConnectionError("client sent close")
        commands = [c for c in payload.decode().split(";") if c]
        self.received += commands
        return commands

    def wait_for(self, command):
        while command not in self.received:
            self.read_command()

    def close(self):
        if self.client:
            try:
                self.frame(8, struct.pack(">H", 1000))
                self.client.settimeout(2)
                while self.client.recv(4096):
                    pass
            except OSError:
                pass
            self.client.close()
        self.listener.close()
