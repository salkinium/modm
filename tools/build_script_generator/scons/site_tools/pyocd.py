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

from SCons.Script import *

from modm_tools import gdb, pyocd

# -----------------------------------------------------------------------------
def _config(env):
	device = env.subst(env["MODM_PYOCD_TARGET"])
	gconfig  = env.SubstList("MODM_GDBINIT_PYOCD", "MODM_GDBINIT")
	gcmds = env.SubstList("MODM_GDB_COMMANDS")
	return (device, gconfig, gcmds)

# -----------------------------------------------------------------------------
def debug_pyocd(env, source):
	def call_debug_pyocd(target, source, env):
		device, gconfig, gcmds = _config(env)
		backend = pyocd.PyOcdBackend(device)
		gdb.call(backend, source=source[0], config=gconfig,
				 commands=gcmds, ui=ARGUMENTS.get("ui", "tui"))
	return env.AlwaysBuildAction(call_debug_pyocd, "$DEBUG_PYOCD_COMSTR", source)

def coredump_pyocd(env):
	def call_coredump_pyocd(target, source, env):
		device, gconfig, gcmds = _config(env)
		backend = pyocd.PyOcdBackend(device)
		gcmds += ["modm_coredump", "modm_build_id", "quit"]
		gdb.call(backend, config=gconfig, commands=gcmds)
	return env.AlwaysBuildAction(call_coredump_pyocd, "$COREDUMP_PYOCD_COMSTR")

# -----------------------------------------------------------------------------
def program_pyocd(env, source):
	def call_program_pyocd(target, source, env):
		return pyocd.program(_config(env)[0], str(source[0]))
	return env.AlwaysBuildAction(call_program_pyocd, "$PROGRAM_PYOCD_COMSTR", source)

def reset_pyocd(env):
	def call_reset_pyocd(target, source, env):
		return pyocd.reset(_config(env)[0])
	return env.AlwaysBuildAction(call_reset_pyocd, "$RESET_PYOCD_COMSTR")

def run_pyocd(env):
	def call_run_pyocd(target, source, env):
		pyocd.call(_config(env)[0], verbose=True)
	return env.AlwaysBuildAction(call_run_pyocd, "$RUN_PYOCD_COMSTR")

# -----------------------------------------------------------------------------
def log_pyocd_rtt(env):
	def run_pyocd_rtt(target, source, env):
		pyocd.rtt(_config(env)[0], int(ARGUMENTS.get("channel", 0)))
	return env.AlwaysBuildAction(run_pyocd_rtt, "$RTT_PYOCD_COMSTR")

# -----------------------------------------------------------------------------
def generate(env, **kw):
	env.AddMethod(program_pyocd, "ProgramPyOcd")
	env.AddMethod(debug_pyocd, "DebugPyOcd")
	env.AddMethod(coredump_pyocd, "CoredumpPyOcd")
	env.AddMethod(reset_pyocd, "ResetPyOcd")
	env.AddMethod(run_pyocd, "PyOcd")

	env.AddMethod(log_pyocd_rtt, "LogRttPyOcd")

def exists(env):
	return env.Detect("pyocd")
