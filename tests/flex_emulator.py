"""A minimal FlexRadio: SmartSDR TCP commands and VITA-49 DAX IQ over UDP."""
import socket
import struct
import threading

HANDLE = "1A2B3C4D"
PAN = "0x40000000"


def vita(stream, count, payload, packet_type=1):
    words = 7 + len(payload) // 4
    header = (packet_type << 28 | 1 << 27 | 1 << 22 | 1 << 20 |
              (count & 0xF) << 16 | words)
    return struct.pack(">7I", header, stream, 0x00001C2D, 0x534C03E3, 0, 0, 0) + payload


class FlexRadio:
    def __init__(self, model="FLEX-6600"):
        self.model = model
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.listener.settimeout(10)
        self.port = self.listener.getsockname()[1]
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.client = None
        self.udp_port = None
        self.commands = []
        self.streams = {}
        self.counts = {}
        self.started = threading.Event()

    def send(self, line):
        self.client.sendall((line + "\n").encode())

    def status(self, text):
        self.send(f"S{HANDLE}|{text}")

    def serve_commands(self):
        """Accept one client and answer its commands until it disconnects."""
        self.client, _ = self.listener.accept()
        self.send("V1.4.0.0")
        self.send(f"H{HANDLE}")
        buffer = b""
        while True:
            try:
                chunk = self.client.recv(4096)
            except OSError:
                return
            if not chunk:
                return
            buffer += chunk
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                sequence, command = line.decode().strip()[1:].split("|", 1)
                self.commands.append(command)
                self.answer(sequence, command)

    def answer(self, sequence, command):
        words = command.split()
        reply = ""
        if command == "info":
            reply = f'model="{self.model}",chassis_serial="1234-5678",name="Shack"'
        elif words[:2] == ["client", "udpport"]:
            self.udp_port = int(words[2])
        elif words[:3] == ["display", "panafall", "create"]:
            reply = PAN
        elif words[:2] == ["stream", "create"]:
            channel = int(dict(w.split("=") for w in words[2:])["daxiq_channel"])
            stream = 0x20000000 + channel
            self.streams[channel] = stream
            reply = f"0x{stream:08X}"
        self.send(f"R{sequence}|0|{reply}")
        if words[:3] == ["display", "pan", "set"]:
            self.status(f"display pan {words[3]} " + " ".join(words[4:]))
        elif words[:2] == ["stream", "set"]:
            self.status(f"stream {words[2]} type=dax_iq " + " ".join(words[3:]))
            self.started.set()

    def iq(self, stream, floats):
        count = self.counts.get(stream, 0)
        self.counts[stream] = count + 1
        payload = struct.pack(f">{len(floats)}f", *floats)
        self.udp.sendto(vita(stream, count, payload), ("127.0.0.1", self.udp_port))

    def announce(self, port=4992):
        text = (f"discovery_protocol_version=3.0.0.2 model={self.model} "
                f"serial=1234-5678 nickname=Shack ip=127.0.0.1 port={self.port}")
        payload = text.encode().ljust((len(text) + 4) // 4 * 4, b"\0")
        self.udp.sendto(vita(0x800, 0, payload, packet_type=3), ("127.0.0.1", port))

    def close(self):
        if self.client:
            self.client.close()
        self.listener.close()
        self.udp.close()
