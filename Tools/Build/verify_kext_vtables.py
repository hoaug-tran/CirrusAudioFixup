#!/usr/bin/env python3
"""Reject IOKit metaclass symbols that OpenCore cannot pair with vtables."""
import argparse
from pathlib import Path
import re
import subprocess


def verify_symbols(symbols: set[str]) -> int:
    count = 0
    for symbol in sorted(symbols):
        match = re.fullmatch(r"__ZN(.+)10superClassE", symbol)
        if not match:
            continue
        count += 1
        body = match.group(1)
        for expected in ("__ZTV" + body, "__ZTVN" + body + "9MetaClassE"):
            if expected not in symbols:
                raise ValueError(
                    f"{symbol}: missing OpenCore-compatible vtable {expected}; "
                    "declare the concrete IOKit class in the global namespace"
                )
    if not count:
        raise ValueError("No IOKit superclass symbols found; use an unstripped kext binary")
    return count


def verify(binary: Path) -> None:
    symbols = set(subprocess.check_output(["nm", "-j", str(binary)], text=True).splitlines())
    count = verify_symbols(symbols)
    print(f"PASS OpenCore vtable symbol pairs: {count} IOKit classes ({binary})")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    try:
        verify(args.binary)
    except (ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"error: {error}\n")
