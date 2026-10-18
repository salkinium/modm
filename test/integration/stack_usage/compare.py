#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright (c) 2026, Niklas Hauser
#
# This file is part of the modm project.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
# -----------------------------------------------------------------------------

"""
Compares the static stack usage analysis with the stack usage measured on a
board. The project is compiled for one configuration of its `project.xml`,
programmed, and the lines `stack NAME BYTES` are read from the serial port
until the test result is printed.

```sh
python3 compare.py project.xml --config modm:nucleo-h563zi --port /dev/cu.usbmodem1 \\
                   --program "pyocd flash -u 0027001C3434511534313937 {elf}"
```

The `{elf}` and `{hex}` in the program command are replaced by the firmware.
Other output formats are read with `--pattern` and `--name`, the largest
value of a stack is compared, since a restarted fiber may report several times.
If the port is part of the program command, the port is opened afterwards,
which resets an Arduino-like board.
"""

import argparse, os, re, select, subprocess, sys, termios, threading, time, tty
from pathlib import Path

repo = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(repo / "tools/scripts"))
import examples_compile


def build(project, config, extra=""):
    """Generates and compiles the configuration, returns its folder and ELF file."""
    generators = [g for g in examples_compile.prepare(project) if config in (None, g[1])]
    if not generators: raise SystemExit(f"Unknown configuration {config} of {project}")
    _, _, options, build_options, project_xml, _ = generators[0]
    folder = project_xml.parent
    for command in (f"lbuild {options} {extra} build {build_options} --no-log", "scons -j8 build"):
        if (result := subprocess.run(command, shell=True, cwd=folder, capture_output=True, text=True)).returncode:
            raise SystemExit(result.stdout[-3000:] + result.stderr[-3000:])
    return folder, next((folder / "build/scons-release").glob("*.elf"))


def measure(port, baud, program, timeout):
    """Programs the board and returns all lines of its output."""
    lines, done = [], threading.Event()
    def read():
        fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(fd)
        attributes = termios.tcgetattr(fd)
        attributes[4] = attributes[5] = getattr(termios, f"B{baud}")
        termios.tcsetattr(fd, termios.TCSANOW, attributes)
        termios.tcflush(fd, termios.TCIFLUSH)
        buffer, end = b"", time.time() + timeout
        while time.time() < end and not done.is_set():
            if select.select([fd], [], [], 0.1)[0]:
                buffer += os.read(fd, 4096)
                *complete, buffer = buffer.split(b"\n")
                lines.extend(line.decode(errors="replace").strip() for line in complete)
                if any("INTEGRATION TEST" in line for line in lines): break
        os.close(fd)
        done.set()

    reader = threading.Thread(target=read)
    # The port must be free while programming via it
    if port not in program: reader.start(); time.sleep(0.2)
    if (result := subprocess.run(program, shell=True, capture_output=True, text=True)).returncode:
        done.set()
        raise SystemExit(result.stdout[-3000:] + result.stderr[-3000:])
    if port in program: reader.start()
    reader.join()
    return lines


def main():
    parser = argparse.ArgumentParser(description="Compares static and measured stack usage.")
    parser.add_argument("project", type=Path, nargs="?", default=Path(__file__).parent / "project.xml")
    parser.add_argument("--config", help="The configuration of the project, e.g. modm:nucleo-h563zi.")
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--program", required=True, help="Command that programs {elf} or {hex}.")
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--lbuild", default="", help="Additional lbuild options, e.g. '-D modm:build:lto=yes'.")
    parser.add_argument("--pattern", default=r"stack (\S+) (\d+)$",
                        help="Regex of a line with the name and bytes of a measured stack.")
    parser.add_argument("--name", action="append", default=[], metavar="MEASURED=STATIC",
                        help="Name of the measured stack in the analysis.")
    args = parser.parse_args()

    folder, elf = build(args.project.resolve(), args.config, args.lbuild)
    sys.path.insert(0, str(folder / "modm"))
    from modm_tools import stack_usage
    os.chdir(folder)
    avr = "avr" in subprocess.run(["file", elf], capture_output=True, text=True).stdout.lower()
    result = stack_usage.analyze(elf, folder / "build", objdump="avr-objdump" if avr else "arm-none-eabi-objdump")

    lines = measure(args.port, args.baud, args.program.format(elf=elf, hex=elf.with_suffix(".hex")), args.timeout)
    names, measured = dict(n.split("=", 1) for n in args.name), {}
    for line in lines:
        if match := re.search(args.pattern, line):
            name = names.get(match[1], match[1])
            measured[name] = max(measured.get(name, 0), int(match[2]))
    if not measured: raise SystemExit("No measurements:\n" + "\n".join(lines[-20:]))

    stacks = dict(result["fibers"], **({"main": result["main"]} if result["main"] else {}))
    print(f"\n{args.config or folder.name}: {'passed' if any('PASSED' in l for l in lines) else 'FAILED'}")
    print(f"{'stack':20} {'measured':>8} {'normal':>8} {'fatal':>8} {'size':>6}  normal - measured")
    for name in sorted(measured, key=lambda n: (n == "main", n)):
        if not (stack := stacks.get(name)):
            print(f"{name:20} {measured[name]:8}  not found by the analysis")
            continue
        difference = stack["normal"] - measured[name]
        print(f"{name:20} {measured[name]:8} {('' if stack['exact'] else '>') + str(stack['normal']):>8} "
              f"{stack['fatal']:8} {stack['size']:6}  {difference:+5}"
              f"{'  UNDERESTIMATED' if difference < 0 else ''}")


if __name__ == "__main__":
    main()
