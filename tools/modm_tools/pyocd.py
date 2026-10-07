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

r"""
### pyOCD

Wraps [pyOCD](https://pyocd.io) to program, reset and debug the target.

```sh
python3 -m modm_tools.pyocd -t stm32f469nihx path/to/project.elf
```

You can also reset the target:

```sh
python3 -m modm_tools.pyocd -t stm32f469nihx --reset
```

pyOCD only has builtin support for a few targets, all others require a CMSIS
Device Family Pack. This tool therefore checks if pyOCD knows the target and
otherwise downloads the right pack via `pyocd pack install {target}`.

You can set the `MODM_PYOCD_BINARY` environment variable to point this script to
a specific `pyocd` binary:

```sh
export MODM_PYOCD_BINARY=/path/to/other/pyocd
```

(\* *only ARM Cortex-M targets*)
"""

import os
import signal
import platform
import subprocess

from .backend import DebugBackend

# -----------------------------------------------------------------------------
class PyOcdBackend(DebugBackend):
    def __init__(self, target):
        super().__init__(":3333")
        self.target = target
        self.process = None

    def start(self):
        self.process = call(self.target, blocking=False, silent=True)

    def stop(self):
        if self.process is not None:
            if "Windows" in platform.platform():
                os.kill(self.process.pid, signal.CTRL_BREAK_EVENT)
            else:
                os.killpg(os.getpgid(self.process.pid), signal.SIGINT)
            self.process.wait()
            self.process = None


def _binary():
    return os.environ.get("MODM_PYOCD_BINARY", "pyocd")


def _has_target(target):
    output = subprocess.run([_binary(), "list", "--targets"], capture_output=True, text=True).stdout
    return any(line.split()[:1] == [target] for line in output.splitlines())


def install_pack(target):
    """Downloads the CMSIS pack for the target unless pyOCD already knows it."""
    if _has_target(target): return
    # The pack index may be missing or older than the device
    for update in (False, True):
        if update: subprocess.call([_binary(), "pack", "update"])
        subprocess.call([_binary(), "pack", "install", target])
        if _has_target(target): return
    raise ValueError(f"pyOCD does not know target '{target}'!")


def _call(subcommand, target, *arguments, verbose=True):
    install_pack(target)
    # -W: fail instead of waiting forever for a debug probe
    command = [_binary(), subcommand, "-W", "-t", target, *arguments]
    if verbose: print(" ".join(command))
    return subprocess.call(command, cwd=os.getcwd())


def call(target, blocking=True, silent=False, verbose=False):
    if blocking:
        return _call("gdbserver", target, "--persist", verbose=verbose)

    install_pack(target)
    command = [_binary(), "gdbserver", "-W", "-t", target]
    if silent: command.append("-qq")
    # We have to start pyOCD in its own session ID, so that Ctrl-C in GDB does
    # not kill pyOCD.
    kwargs = {"cwd": os.getcwd()}
    if "Windows" in platform.platform():
        kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        kwargs["start_new_session"] = True
    return subprocess.Popen(command, **kwargs)


# -----------------------------------------------------------------------------
def rtt(target, channel=0):
    return _call("rtt", target, "--up-channel-id", str(channel),
                 "--down-channel-id", str(channel))


def program(target, source):
    return _call("load", target, source)


def reset(target):
    return _call("reset", target)


# -----------------------------------------------------------------------------
def add_subparser(subparser):
    parser = subparser.add_parser("pyocd", help="Use pyOCD as Backend.")
    parser.add_argument(
            "-t", "--target",
            dest="target",
            required=True,
            help="Connect to this target.")
    parser.set_defaults(backend=lambda args: PyOcdBackend(args.target))
    return parser


# -----------------------------------------------------------------------------
if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(
        description="Program ELF file or reset device via pyOCD")
    parser.add_argument(
            dest="source",
            nargs="?",
            metavar="ELF")
    parser.add_argument(
            "-t", "--target",
            dest="target",
            required=True,
            help="Connect to this target.")
    parser.add_argument(
            "-r", "--reset",
            dest="reset",
            default=False,
            action="store_true",
            help="Reset device.")

    args = parser.parse_args()
    if args.reset:
        exit(reset(args.target))
    else:
        exit(program(args.target, os.path.abspath(args.source)))
