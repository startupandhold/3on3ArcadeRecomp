#!/usr/bin/env python3
"""Reads and edits EA "TDB" databases (db/nhlng.db): big-endian, bit-packed
records, with scrambled four-letter table and field names.

Usage: ea_tdb.py DB [TABLE [FIELD=VALUE ...]]
Without TABLE, lists the tables; with TABLE, prints the records matching
every FIELD=VALUE filter.

Layout: a 24-byte header, an index of (name, offset) pairs, then the tables.
Each table has a 40-byte header, 16-byte field descriptors and fixed-size
records. Fields are packed most-significant bit first from the start of the
record; strings (type 0) are NUL-padded byte arrays.

Checksums are CRC-32/MPEG-2: the file header covers bytes 0-0x13; each table
header covers its own bytes 4-35; each table's first word covers the previous
table's fields and records (the first table's covers the index); the last
four bytes of the file cover the last table's fields and records.
"""
import struct
import sys

_CRC_TABLE = []
for _i in range(256):
    _c = _i << 24
    for _ in range(8):
        _c = ((_c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if _c & 0x80000000 else (_c << 1) & 0xFFFFFFFF
    _CRC_TABLE.append(_c)


def crc32_mpeg2(data):
    c = 0xFFFFFFFF
    for b in data:
        c = ((c << 8) & 0xFFFFFFFF) ^ _CRC_TABLE[((c >> 24) ^ b) & 0xFF]
    return c


class Field:
    def __init__(self, name, type_, bit_offset, bits):
        self.name, self.type, self.bit_offset, self.bits = name, type_, bit_offset, bits


class Table:
    def __init__(self, db, name, offset):
        d = db.data
        self.db, self.name, self.offset = db, name, offset
        self.record_size = struct.unpack_from(">I", d, offset + 8)[0]
        self.capacity, self.count = struct.unpack_from(">HH", d, offset + 20)
        self.fields = {}
        p = offset + 40
        for _ in range(d[offset + 28]):
            type_, bit_offset, name_, bits = struct.unpack_from(">II4sI", d, p)
            self.fields[name_.decode("latin1")] = Field(name_.decode("latin1"), type_, bit_offset, bits)
            p += 16
        self.data_offset = p

    def end(self):
        return self.data_offset + self.record_size * self.capacity

    def get(self, row, name):
        f = self.fields[name]
        start = self.data_offset + row * self.record_size
        d = self.db.data
        if f.type == 0:
            raw = bytes(d[start + f.bit_offset // 8:start + (f.bit_offset + f.bits) // 8])
            return raw.split(b"\0")[0].decode("utf-8", "replace")
        record = int.from_bytes(d[start:start + self.record_size], "big")
        value = (record >> (self.record_size * 8 - f.bit_offset - f.bits)) & ((1 << f.bits) - 1)
        if f.type == 2 and value >> (f.bits - 1):
            value -= 1 << f.bits
        return value

    def set(self, row, name, value):
        f = self.fields[name]
        start = self.data_offset + row * self.record_size
        d = self.db.data
        if f.type == 0:
            raw = value.encode("utf-8")
            size = f.bits // 8
            if len(raw) >= size:
                raise ValueError(f"{self.name}.{name}: {value!r} is longer than {size - 1} bytes")
            at = start + f.bit_offset // 8
            d[at:at + size] = raw + bytes(size - len(raw))
            return
        if not 0 <= value < (1 << f.bits):
            if f.type == 2 and -(1 << (f.bits - 1)) <= value < 0:
                value += 1 << f.bits
            else:
                raise ValueError(f"{self.name}.{name}: {value} does not fit in {f.bits} bits")
        total = self.record_size * 8
        shift = total - f.bit_offset - f.bits
        record = int.from_bytes(d[start:start + self.record_size], "big")
        record = (record & ~(((1 << f.bits) - 1) << shift)) | (value << shift)
        d[start:start + self.record_size] = record.to_bytes(self.record_size, "big")

    def row(self, row):
        return {name: self.get(row, name) for name in self.fields}

    def rows(self):
        return [self.row(r) for r in range(self.count)]


class Database:
    def __init__(self, data):
        self.data = bytearray(data)
        if self.data[:2] != b"DB":
            raise ValueError("not a TDB database")
        count = struct.unpack_from(">I", self.data, 0x10)[0]
        self.index_end = 0x18 + 8 * count
        self.tables = {}
        self.order = []
        for i in range(count):
            name, offset = struct.unpack_from(">4sI", self.data, 0x18 + 8 * i)
            table = Table(self, name.decode("latin1"), self.index_end + offset)
            self.tables[table.name] = table
            self.order.append(table)

    @classmethod
    def load(cls, path):
        with open(path, "rb") as f:
            return cls(f.read())

    def __getitem__(self, name):
        return self.tables[name]

    def update_checksums(self):
        d = self.data
        struct.pack_into(">I", d, 0x14, crc32_mpeg2(d[:0x14]))
        previous = d[0x18:self.index_end]
        for table in self.order:
            struct.pack_into(">I", d, table.offset, crc32_mpeg2(previous))
            struct.pack_into(">I", d, table.offset + 36, crc32_mpeg2(d[table.offset + 4:table.offset + 36]))
            previous = d[table.offset + 40:table.end()]
        struct.pack_into(">I", d, len(d) - 4, crc32_mpeg2(previous))

    def save(self, path):
        self.update_checksums()
        with open(path, "wb") as f:
            f.write(self.data)


def main():
    db = Database.load(sys.argv[1])
    if len(sys.argv) == 2:
        for t in db.order:
            fields = " ".join(f"{f.name}:{f.type}/{f.bits}" for f in t.fields.values())
            print(f"{t.name} records {t.count}/{t.capacity} size {t.record_size}  {fields}")
        return
    table = db[sys.argv[2]]
    filters = [arg.split("=", 1) for arg in sys.argv[3:]]
    for r in range(table.count):
        row = table.row(r)
        if all(str(row[k]) == v for k, v in filters):
            print(r, row)


if __name__ == "__main__":
    main()
