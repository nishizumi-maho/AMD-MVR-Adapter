"""Log iRacing's own performance telemetry (iRacing SDK shared memory) to a CSV file.

Usage: python -I irsdk_logger.py <output.csv> <seconds>

The iRacing SDK publishes a memory-mapped file ("Local\\IRSDKMemMapFileName") with a header, a list of variable
headers, and up to 4 rotating data buffers. We read the most recent buffer at ~4 Hz.
"""

import csv
import mmap
import struct
import sys
import time

MEMMAP_NAME = "Local\\IRSDKMemMapFileName"
MEMMAP_SIZE = 1164 * 1024
STATUS_CONNECTED = 1

# Variables of interest (name -> column). Missing ones are skipped.
WANTED = ["FrameRate", "CpuUsageFG", "CpuUsageBG", "GpuUsage", "IsOnTrack", "Speed"]

# irsdk_VarType -> struct format
VAR_FORMATS = {0: "c", 1: "?", 2: "i", 3: "I", 4: "f", 5: "d"}


def open_shared_memory():
    try:
        return mmap.mmap(-1, MEMMAP_SIZE, tagname=MEMMAP_NAME, access=mmap.ACCESS_READ)
    except OSError:
        return None


def read_header(mem):
    # int ver, status, tickRate, sessionInfoUpdate, sessionInfoLen, sessionInfoOffset, numVars, varHeaderOffset,
    # numBuf, bufLen, pad[2]; then varBuf[4] of {int tickCount, bufOffset, pad[2]}
    fields = struct.unpack_from("<10i2i", mem, 0)
    header = {
        "status": fields[1],
        "tickRate": fields[2],
        "numVars": fields[6],
        "varHeaderOffset": fields[7],
        "numBuf": fields[8],
    }
    buffers = [struct.unpack_from("<2i", mem, 48 + i * 16) for i in range(4)]
    header["buffers"] = buffers[: header["numBuf"]]
    return header


def read_var_headers(mem, header):
    # int type, offset, count; bool countAsTime; char pad[3]; char name[32], desc[64], unit[32] -> 144 bytes
    variables = {}
    for i in range(header["numVars"]):
        base = header["varHeaderOffset"] + i * 144
        var_type, offset, count = struct.unpack_from("<3i", mem, base)
        name = mem[base + 16 : base + 48].split(b"\0", 1)[0].decode("ascii", "replace")
        if name in WANTED and var_type in VAR_FORMATS:
            variables[name] = (VAR_FORMATS[var_type], offset)
    return variables


def latest_buffer_offset(header):
    tick, offset = max(header["buffers"], key=lambda b: b[0])
    return offset


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    output_path, seconds = sys.argv[1], float(sys.argv[2])

    mem = open_shared_memory()
    if mem is None or read_header(mem)["status"] & STATUS_CONNECTED == 0:
        print("irsdk: iRacing is not running (or not on track yet)")
        return 1

    header = read_header(mem)
    variables = read_var_headers(mem, header)
    missing = [name for name in WANTED if name not in variables]
    if missing:
        print("irsdk: variables not found:", ", ".join(missing))
    columns = [name for name in WANTED if name in variables]

    with open(output_path, "w", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(["t"] + columns)
        start = time.perf_counter()
        while (elapsed := time.perf_counter() - start) < seconds:
            header = read_header(mem)
            base = latest_buffer_offset(header)
            row = [f"{elapsed:.3f}"]
            for name in columns:
                fmt, offset = variables[name]
                row.append(struct.unpack_from("<" + fmt, mem, base + offset)[0])
            writer.writerow(row)
            time.sleep(0.25)
    print(f"irsdk: wrote {output_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
