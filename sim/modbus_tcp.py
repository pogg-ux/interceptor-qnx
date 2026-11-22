"""
Minimal Modbus TCP server and client. Standard library only.

Supports function codes:
  1 Read Coils, 2 Read Discrete Inputs, 3 Read Holding Registers,
  4 Read Input Registers, 5 Write Single Coil, 6 Write Single Register,
  15 Write Multiple Coils, 16 Write Multiple Registers.

License: MIT
"""
import asyncio
import socket
import struct


class DataBank:
    """Plain lists that hold the four Modbus tables."""

    def __init__(self, n_di=64, n_coils=64, n_ir=64, n_hr=64):
        self.di = [0] * n_di        # discrete inputs  (device -> master)
        self.coils = [0] * n_coils  # coils            (master -> device)
        self.ir = [0] * n_ir        # input registers  (device -> master)
        self.hr = [0] * n_hr        # holding registers (master -> device)


def _pack_bits(bits):
    out = bytearray((len(bits) + 7) // 8)
    for i, b in enumerate(bits):
        if b:
            out[i // 8] |= 1 << (i % 8)
    return bytes(out)


def _unpack_bits(data, count):
    return [(data[i // 8] >> (i % 8)) & 1 for i in range(count)]


def handle_pdu(bank, pdu):
    """Process one request PDU against the bank and return the response PDU."""
    fc = pdu[0]

    def exc(code):
        return bytes([fc | 0x80, code])

    try:
        if fc in (1, 2):
            addr, count = struct.unpack(">HH", pdu[1:5])
            table = bank.coils if fc == 1 else bank.di
            if addr + count > len(table):
                return exc(2)
            data = _pack_bits(table[addr:addr + count])
            return bytes([fc, len(data)]) + data
        if fc in (3, 4):
            addr, count = struct.unpack(">HH", pdu[1:5])
            table = bank.hr if fc == 3 else bank.ir
            if addr + count > len(table):
                return exc(2)
            vals = [v & 0xFFFF for v in table[addr:addr + count]]
            return bytes([fc, 2 * count]) + struct.pack(">%dH" % count, *vals)
        if fc == 5:
            addr, val = struct.unpack(">HH", pdu[1:5])
            if addr >= len(bank.coils):
                return exc(2)
            bank.coils[addr] = 1 if val == 0xFF00 else 0
            return pdu[:5]
        if fc == 6:
            addr, val = struct.unpack(">HH", pdu[1:5])
            if addr >= len(bank.hr):
                return exc(2)
            bank.hr[addr] = val
            return pdu[:5]
        if fc == 15:
            addr, count, _nbytes = struct.unpack(">HHB", pdu[1:6])
            if addr + count > len(bank.coils):
                return exc(2)
            bank.coils[addr:addr + count] = _unpack_bits(pdu[6:], count)
            return pdu[:5]
        if fc == 16:
            addr, count, _nbytes = struct.unpack(">HHB", pdu[1:6])
            if addr + count > len(bank.hr):
                return exc(2)
            bank.hr[addr:addr + count] = list(struct.unpack(">%dH" % count, pdu[6:6 + 2 * count]))
            return pdu[:5]
    except struct.error:
        return exc(3)
    return exc(1)


class ModbusTCPServer:
    def __init__(self, bank, host="0.0.0.0", port=5021, on_request=None):
        self.bank = bank
        self.host = host
        self.port = port
        self.on_request = on_request  # optional callback(fc)

    async def _client(self, reader, writer):
        try:
            while True:
                header = await reader.readexactly(7)
                tid, pid, length, uid = struct.unpack(">HHHB", header)
                pdu = await reader.readexactly(length - 1)
                resp = handle_pdu(self.bank, pdu)
                if self.on_request:
                    self.on_request(pdu[0])
                writer.write(struct.pack(">HHHB", tid, pid, len(resp) + 1, uid) + resp)
                await writer.drain()
        except (asyncio.IncompleteReadError, ConnectionResetError):
            pass
        finally:
            writer.close()

    async def serve(self):
        server = await asyncio.start_server(self._client, self.host, self.port)
        async with server:
            await server.serve_forever()


class ModbusTCPClient:
    """Tiny blocking client, used by the HMI and test tools."""

    def __init__(self, host="127.0.0.1", port=502, unit=1, timeout=2.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.unit = unit
        self.tid = 0

    def _call(self, pdu):
        self.tid = (self.tid + 1) & 0xFFFF
        self.sock.sendall(struct.pack(">HHHB", self.tid, 0, len(pdu) + 1, self.unit) + pdu)
        header = self._recv(7)
        _tid, _pid, length, _uid = struct.unpack(">HHHB", header)
        resp = self._recv(length - 1)
        if resp[0] & 0x80:
            raise IOError("Modbus exception %d on function %d" % (resp[1], resp[0] & 0x7F))
        return resp

    def _recv(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("connection closed")
            buf += chunk
        return buf

    def read_holding(self, addr, count):
        r = self._call(struct.pack(">BHH", 3, addr, count))
        return list(struct.unpack(">%dH" % count, r[2:2 + 2 * count]))

    def read_input_regs(self, addr, count):
        r = self._call(struct.pack(">BHH", 4, addr, count))
        return list(struct.unpack(">%dH" % count, r[2:2 + 2 * count]))

    def read_coils(self, addr, count):
        r = self._call(struct.pack(">BHH", 1, addr, count))
        return _unpack_bits(r[2:], count)

    def read_discrete(self, addr, count):
        r = self._call(struct.pack(">BHH", 2, addr, count))
        return _unpack_bits(r[2:], count)

    def write_register(self, addr, value):
        self._call(struct.pack(">BHH", 6, addr, value & 0xFFFF))

    def close(self):
        self.sock.close()
