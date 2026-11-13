"""JSON in and out for the estimator's files.

Reading is strict (no duplicate keys, no NaN or Infinity, a size limit) and every
problem raises InputError. Data files named by a request are checked for place,
size and SHA-256. Writing is byte-stable: fixed key order as given, two-space
indent, six decimals, LF and a final newline, and the file appears atomically.
"""
import hashlib
import json
import math
import os
import tempfile

import numpy as np

from .errors import InputError

MAX_JSON_BYTES = 1 << 20
MAX_INT_DIGITS = 18       # a longer integer is an input error, not a number to compute with
FLOAT_DIGITS = 6


def _reject_constant(label):
    def reject(name):
        raise InputError(f"{label} contains {name}")
    return reject


def _no_duplicates(label):
    def build(pairs):
        out = {}
        for k, v in pairs:
            if k in out:
                raise InputError(f"{label} repeats the key {k!r}")
            out[k] = v
        return out
    return build


def _parse_int(label):
    def parse(text):
        if len(text) > MAX_INT_DIGITS:
            raise InputError(f"{label} holds an integer with too many digits")
        return int(text)
    return parse


def loads_strict(raw, label):
    """Parse UTF-8 JSON bytes.

    Duplicate keys, NaN, Infinity, very long integers and very deep nesting are
    InputError, like malformed JSON.
    """
    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=_no_duplicates(label),
                          parse_constant=_reject_constant(label), parse_int=_parse_int(label))
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        raise InputError(f"{label} is not valid JSON: {e}") from e
    except RecursionError as e:
        raise InputError(f"{label} is nested too deeply") from e


def read_json_file(path, label, max_bytes=MAX_JSON_BYTES):
    """Read a JSON file of at most max_bytes. Returns (document, raw bytes)."""
    try:
        if os.path.getsize(path) > max_bytes:
            raise InputError(f"{label} file is too large")
        with open(path, "rb") as f:
            raw = f.read()
    except OSError as e:
        raise InputError(f"cannot read the {label}: {e.strerror or type(e).__name__}") from e
    return loads_strict(raw, label), raw


def sha256_of_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def is_sha256(value):
    return (isinstance(value, str) and len(value) == 64
            and all(c in "0123456789abcdef" for c in value))


def data_file(base_dir, spec, label, expected_size):
    """Resolve, size-check and hash-check a data file named in a request."""
    if not isinstance(spec.get("file"), str) or not spec["file"]:
        raise InputError(f"{label}.file must be a file name")
    name = spec["file"]
    if os.path.isabs(name) or os.path.splitdrive(name)[0]:
        raise InputError(f"{label}.file must be relative to the request")
    root = os.path.realpath(base_dir)
    path = os.path.realpath(os.path.join(root, name))
    try:
        inside = os.path.commonpath([root, path]) == root
    except ValueError:
        inside = False
    if not inside:
        raise InputError(f"{label}.file leaves the request folder")
    if not os.path.isfile(path):
        raise InputError(f"{label}.file does not exist")
    size = os.path.getsize(path)
    if size != expected_size:
        raise InputError(f"{label}.file has {size} bytes, expected {expected_size}")
    digest = spec.get("sha256")
    if not is_sha256(digest):
        raise InputError(f"{label}.sha256 must be 64 lowercase hex digits")
    if sha256_of_file(path) != digest:
        raise InputError(f"{label}.file does not match its sha256")
    return path


def write_atomic(path, text):
    """Write text (UTF-8, LF) to path through a temporary file in the same folder."""
    folder = os.path.dirname(os.path.abspath(path))
    fd, tmp = tempfile.mkstemp(dir=folder, prefix=".a0seam-", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise


def _number(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, np.integer)):
        return str(int(v))
    f = float(v)
    if not math.isfinite(f):
        raise ValueError("document holds a non-finite number")
    s = f"{f:.{FLOAT_DIGITS}f}"
    return s[1:] if s.startswith("-") and float(s) == 0.0 else s


def _is_scalar_list(v):
    return isinstance(v, list) and all(not isinstance(i, (dict, list)) for i in v)


def _emit(v, indent, out):
    pad = "  " * indent
    if v is None:
        out.append("null")
    elif isinstance(v, str):
        out.append(json.dumps(v, ensure_ascii=False))
    elif isinstance(v, dict):
        if not v:
            out.append("{}")
            return
        out.append("{\n")
        for i, (key, val) in enumerate(v.items()):
            out.append(f"{pad}  {json.dumps(key, ensure_ascii=False)}: ")
            _emit(val, indent + 1, out)
            out.append(",\n" if i < len(v) - 1 else "\n")
        out.append(pad + "}")
    elif isinstance(v, list) and _is_scalar_list(v):
        out.append("[" + ", ".join(_inline(i) for i in v) + "]")
    elif isinstance(v, list):
        out.append("[\n")
        for i, val in enumerate(v):
            out.append(pad + "  ")
            _emit(val, indent + 1, out)
            out.append(",\n" if i < len(v) - 1 else "\n")
        out.append(pad + "]")
    else:
        out.append(_number(v))


def _inline(v):
    out = []
    _emit(v, 0, out)
    return "".join(out)


def dumps(document):
    """Serialize with the given key order, two-space indent, 6 decimals and a final newline."""
    out = []
    _emit(document, 0, out)
    return "".join(out) + "\n"
