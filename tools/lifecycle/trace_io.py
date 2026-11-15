import struct
from pathlib import Path

HEADER = struct.Struct("<10Q")
MAGIC = 0x325254535550474D


def read_header(path, *, allow_loss=False):
    path = Path(path)
    with path.open("rb") as stream:
        raw = stream.read(HEADER.size)
    if len(raw) != HEADER.size:
        raise ValueError(f"Incomplete trace header: {path}")
    values = dict(zip(("magic", "version", "record_bytes", "session", "frequency", "begin", "end", "records", "dropped", "failures"), HEADER.unpack(raw)))
    if values["magic"] != MAGIC or values["version"] != 1 or not 0 < values["record_bytes"] <= 32768:
        raise ValueError(f"Unsupported trace format: {path}")
    if not values["frequency"] or values["end"] <= values["begin"]:
        raise ValueError(f"Unsealed trace: {path}")
    if path.stat().st_size != HEADER.size + values["records"] * values["record_bytes"]:
        raise ValueError(f"Truncated or trailing trace data: {path}")
    if (values["dropped"] or values["failures"]) and not allow_loss:
        raise ValueError(f"Trace loss/failure: {path}: {values['dropped']}/{values['failures']}")
    return values


def records(path, header=None):
    header = header or read_header(path)
    with Path(path).open("rb") as stream:
        stream.seek(HEADER.size)
        for _ in range(header["records"]):
            raw = stream.read(header["record_bytes"])
            if len(raw) != header["record_bytes"]:
                raise ValueError("Trace changed while reading")
            yield raw
