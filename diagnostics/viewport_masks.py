"""Summarize how the dumped vertex shaders write the per-view viewport masks and the position of view 3.

The diagnostic build writes the disassembly of the first shaders it processes to
%LOCALAPPDATA%\\AMD-MVR-Adapter\\shaders. This script groups the instructions that write the NV_VIEWPORT_MASK,
NV_VIEWPORT_MASK_2_SEMANTIC and NV_POSITION_VIEW_3_SEMANTIC outputs, to see at a glance whether the masks are
constant, depend on the number of views, or are computed per object (culling).

Usage: python -I viewport_masks.py [shaders_dir]
"""

import collections
import os
import re
import sys
from pathlib import Path

OUTPUTS = ("NV_VIEWPORT_MASK", "NV_VIEWPORT_MASK_2_SEMANTIC", "NV_POSITION_VIEW_3_SEMANTIC")


def output_registers(text):
    """Output register (o#) of each output of interest, from the output signature in the disassembly comments."""
    registers = {}
    for name, register in re.findall(r"^// (\S+)\s+\d+\s+\w+\s+(\d+)\s", text, re.M):
        if name in OUTPUTS:
            registers[name] = "o" + register
    return registers


def writes(text, register):
    """The instructions writing a register, with the temporary registers renamed so that equal code groups together."""
    body = text.split("dcl_temps", 1)[-1]
    return " | ".join(re.sub(r"\br\d+\b", "rN", line.strip())
                      for line in body.splitlines() if re.match(rf"\s*\w+ {register}\.", line))


def main():
    default = Path(os.environ.get("LOCALAPPDATA", ".")) / "AMD-MVR-Adapter" / "shaders"
    directory = Path(sys.argv[1]) if len(sys.argv) > 1 else default
    patterns = {name: collections.Counter() for name in OUTPUTS}
    count = 0
    for path in sorted(directory.glob("*.vs.asm")):
        if ".patched" in path.name:
            continue
        text = path.read_bytes().replace(b"\0", b"").decode("latin-1")
        registers = output_registers(text)
        if not registers:
            continue
        count += 1
        for name, register in registers.items():
            # Normalize the register number, which differs between shaders.
            patterns[name][writes(text, register).replace(register + ".", "oN.")] += 1

    print(f"{count} original vertex shaders with multi-view outputs in {directory}")
    for name in OUTPUTS:
        print(f"\n{name}:")
        for code, shaders in patterns[name].most_common():
            print(f"  {shaders:4d} shaders: {code}")
    return 0 if count else 1


if __name__ == "__main__":
    sys.exit(main())
