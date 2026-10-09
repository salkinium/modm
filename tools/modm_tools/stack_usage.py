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
### Worst-Case Stack Usage

Computes the worst-case stack usage of the main stack, of every fiber and of
every other call graph root from the call graphs that GCC emits when compiling
with `-fcallgraph-info=su,da` and compares it with the available stack size.

```sh
python3 -m modm_tools.stack_usage path/to/project.elf path/to/buildfolder
```

Indirect calls are resolved in this order:

1. Hints passed as `--hint "site=target"`: `site` is a regex searched in
   `path/to/file.cpp: source code line` of the call. The call may then reach
   all functions whose symbol or signature matches the regex `target`, or all
   functions pointed to by the table between the linker symbols `name_start`
   and `name_end` if the target is `table:name`.
   An empty target declares that the call reaches nothing.
2. Virtual calls reach all functions of the same name found in any vtable.
3. Everything else is listed as unresolved and the result is a lower bound.

A fiber constructed from a function instead of a lambda may execute any function
without arguments that is not called by anything else, since only the lambda is
part of its type.

The stack usage of functions written in assembly is taken from the call frame
information. A symbol `__modm_stack_usage.function = bytes` overrides it for
naked functions, which have neither.

A result prefixed with `>` is only a lower bound due to unresolved indirect
calls, recursion, unbounded dynamic stack allocations or functions without
stack usage information (assembly, precompiled libraries).
"""

import re
import subprocess
from collections import defaultdict
from pathlib import Path
from elftools.elf.elffile import ELFFile

_STRING = r'"((?:[^"\\]|\\.)*)"'
_NODE = re.compile(r'^node: \{ title: ' + _STRING + ' label: ' + _STRING)
_EDGE = re.compile(r'^edge: \{ sourcename: ' + _STRING + ' targetname: ' + _STRING +
                   '(?: label: ' + _STRING + ')?')
_BYTES = re.compile(r'\\n(\d+) bytes \(([a-z, ]+)\)')
_INDIRECT = "__indirect_call"
_FRAME = "__modm_stack_usage."
# The entry function of a fiber: its stack size and the type of its function
_FIBER = re.compile(r"modm::fiber::Task::Task<(\d+)u, (.+)>\(modm::fiber::Stack<\1u>&, ")
# Indirect calls inside modm that cannot be resolved via vtables
_HINTS = [
    r"startup\.c.*\(\*entry\)\(\)=table:__init_array",
    r"assert\.cpp.*\(\*handler\)\(\*info\)=table:__assertion_table",
    r"printf\.c.*gadget->function\(=IOStream::out_char",
    r"iostream\.hpp.*return format\(\*this\)=^modm::\w+\(modm::IOStream&\)$",
    r"inplace_function\.hpp.*vtable_ptr_->\w+_ptr\(=inplace_function_detail::vtable",
    # Sets up the main stack and jumps to __modm_startup, which is the root
    r"Reset_Handler \(disassembly\)=",
    r"new_delete\.cpp.*get_new_handler\(\)\(\)=",
]


def _bare(title):
    # Functions local to a translation unit are prefixed with its file name
    return title.rsplit(":", 1)[-1]


def _demangle(symbols):
    try:
        return subprocess.run(["c++filt", "-n"], input="\n".join(symbols), text=True,
                              capture_output=True, check=True).stdout.splitlines()
    except (OSError, subprocess.CalledProcessError):
        return symbols


class CallGraph:
    def __init__(self, elf, buildpath, hints=None, objdump="arm-none-eabi-objdump"):
        self.stack = {}                  # title -> bytes, missing if unknown
        self.unbounded = set()           # titles with unbounded dynamic stack
        self.label = {}                  # title -> demangled name
        self.calls = defaultdict(set)    # title -> called titles
        self.indirect = defaultdict(set) # title -> call sites "file:line:col"
        self.bare = defaultdict(set)     # bare symbol -> defining titles
        self.unresolved = set()          # call sites without any target
        self.hints = [h.split("=", 1) for h in (hints or []) + _HINTS]

        files = sorted(Path(buildpath).rglob("*.ci"), key=lambda f: f.stat().st_mtime)
        # With LTO the whole call graph is generated last by the linker, any
        # other call graphs are leftovers of previous builds and vice versa
        lto = bool(files) and ".ltrans" in files[-1].name
        files = [f for f in files if (".ltrans" in f.name) == lto]
        for file in files:
            self._parse(file)
        self._parse_elf(elf, objdump)
        # The labels are unqualified with LTO and ambiguous for lambdas
        titles = list(self.stack)
        self.label.update(zip(titles, _demangle([_bare(t) for t in titles])))
        for title in self._virtual:
            # function name in front of the last, possibly nested, argument list
            if name := re.search(r"(~?\w+)\((?:[^()]|\((?:[^()]|\([^()]*\))*\))*\)"
                                 r"(?: const)?(?: \(\.[\w.]+\))?$", self.label[title]):
                self.virtual[name[1]].add(title)

    def _parse(self, file):
        for line in file.read_text(errors="replace").splitlines():
            if match := _NODE.match(line):
                title, label = match.groups()
                if size := _BYTES.search(label):
                    # Inline functions may differ per translation unit
                    self.stack[title] = max(self.stack.get(title, 0), int(size[1]))
                    self.bare[_bare(title)].add(title)
                    if "dynamic" in size[2] and "bounded" not in size[2]:
                        self.unbounded.add(title)
            elif match := _EDGE.match(line):
                source, target, site = match.groups()
                if target == _INDIRECT: self.indirect[source].add(site)
                else: self.calls[source].add(target)

    def _parse_elf(self, elf, objdump):
        """Finds all functions referenced by vtables and linker tables."""
        self.virtual = defaultdict(set) # function name -> titles
        self.table = {}                 # table name -> titles
        with open(elf, "rb") as file:
            elffile = ELFFile(file)
            symbols = list(elffile.get_section_by_name(".symtab").iter_symbols())
            functions = defaultdict(list) # address -> aliased symbols
            for symbol in symbols:
                if symbol["st_info"]["type"] == "STT_FUNC":
                    functions[symbol["st_value"] & ~1].append(symbol.name)
            self.linked = {name for names in functions.values() for name in names}
            self.globals = {s.name for s in symbols if s["st_info"]["bind"] != "STB_LOCAL"}
            width = self.width = elffile.elfclass // 8

            # Assembly and precompiled libraries have no call graph info, but
            # usually call frame info and their calls are found by disassembly
            missing = {} # symbol -> title
            for names in functions.values():
                titles = set().union(*(self.bare.get(name, []) for name in names))
                if not titles:
                    missing.update({name: names[0] for name in names})
                for name in names:
                    self.bare[name] = titles or {names[0]}
            self.value = {s.name: s["st_value"] for s in symbols}
            for name, size in self.value.items():
                if name.startswith(_FRAME):
                    for title in self.bare.get(name[len(_FRAME):], []):
                        self.stack[title] = max(self.stack.get(title, 0), size)
            for entry in elffile.get_dwarf_info().CFI_entries():
                names = functions.get(entry.header.get("initial_location", -1) & ~1, [""])
                if title := missing.get(names[0]):
                    self.stack[title] = max(row["cfa"].offset or 0
                                            for row in entry.get_decoded().table)
            try:
                listing = subprocess.run([objdump, "-d", elf], capture_output=True,
                                         text=True, check=True).stdout
            except (OSError, subprocess.CalledProcessError):
                listing = ""
            for function in re.split(r"\n\n", listing):
                if not (match := re.match(r"[0-9a-f]+ <(.+)>:\n", function)): continue
                if not (title := missing.get(match[1])): continue
                self.calls[title] |= set(re.findall(r"\tbl?[a-z.]*\t[0-9a-f]+ <([^+>]+)>", function))
                if re.search(r"\tblx\tr|\tbx\t(?!lr)", function):
                    self.indirect[title].add(title + " (disassembly)")
                # without call frame info only functions not using the stack are known
                if not re.search(r"\bsp\b|push", function):
                    self.stack.setdefault(title, 0)

            def pointers(symbol, size):
                section = elffile.get_section(symbol["st_shndx"])
                offset = symbol["st_value"] - section["sh_addr"]
                data = section.data()[offset:offset + size]
                for ii in range(0, len(data), width):
                    address = int.from_bytes(data[ii:ii + width], "little") & ~1
                    for name in functions.get(address, []):
                        yield from self.bare.get(name, [])

            starts = {s.name[:-6]: s for s in symbols if s.name.endswith("_start")}
            for end in (s for s in symbols if s.name.endswith("_end")):
                start, size = starts.get(end.name[:-4]), 0
                if start: size = end["st_value"] - start["st_value"]
                if size > 0 and isinstance(start["st_shndx"], int):
                    self.table[end.name[:-4]] = set(pointers(start, size))
            self._virtual = {title for vtable in symbols if vtable.name.startswith("_ZTV")
                             for title in pointers(vtable, vtable["st_size"])}

    def _targets(self, site):
        """Resolves an indirect call site to all functions it may call."""
        file = site
        try:
            file, line, col = site.rsplit(":", 2)
            source = Path(file).read_text(errors="replace").splitlines()[int(line) - 1]
            # the column points either at the callee or its opening parenthesis
            names = re.findall(r"[A-Za-z_]\w*", source[:int(col) - 1])[-1:] + \
                    re.findall(r"^[\W]*([A-Za-z_]\w*)", source[int(col) - 1:])
        except (OSError, ValueError, IndexError):
            source, names = "", []
        for hint, target in self.hints:
            if re.search(hint, file + ": " + source):
                if not target: return set()
                if target.startswith("table:"):
                    return self.table.get(target[len("table:"):], set())
                return {t for t in self.stack if re.search(target, self.label[t])}
        for name in names:
            if name in self.virtual:
                return self.virtual[name]
        self.unresolved.add(site)
        return set()

    def resolve(self):
        self.entries = set() # titles that call the function of a fiber
        for source, sites in self.indirect.items():
            for site in sites:
                if "fiber/task_impl.hpp" in site and _FIBER.match(self.label.get(source, "")):
                    self.entries.add(source)
                    continue
                # ponytail: virtual calls are matched by name only, so calling
                # an overload looks like recursion. Match the signature too if
                # overloads across classes cause false recursion.
                self.calls[source] |= {t for t in self._targets(site)
                                       if _bare(t) != _bare(source)}

        # A fiber may execute any function that nothing else calls
        called = self._called()
        self.free = {t for t in self.stack if t not in called and _bare(t) in self.linked
                     and re.fullmatch(r"[\w:]+\(\)", self.label[t])}

    def fiber(self, title, function):
        """Returns `(bytes, exact, chain)` of a fiber entry calling its function."""
        targets = self.free if re.search(r"\([&*]\)", function) else \
            {t for t in self.stack if self.label[t].startswith(function + "::")}
        # LTO merges identical entry functions, thus only the root tells the
        # fibers apart and each one must be evaluated with its own function
        calls, self._cache = self.calls, {}
        self.calls = defaultdict(set, calls)
        for entry in self.entries:
            self.calls[entry] = calls[entry] | targets
        result = self.depth(title)
        self.calls, self._cache = calls, {}
        return result

    def _called(self):
        return {d for targets in self.calls.values() for t in targets for d in self._define(t)}

    def _define(self, title):
        """Returns the titles of all definitions a call target may refer to."""
        # The linker may replace weak and inline functions with another definition
        if title in self.stack and _bare(title) not in self.globals: return [title]
        return self.bare.get(_bare(title), [])

    def depth(self, title, path=()):
        """Returns `(bytes, exact, deepest call chain)` of a function."""
        if title in path:
            self.recursive.add(path[path.index(title):])
            return (0, False, [title + " (recursion)"])
        if title in self._cache: return self._cache[title]
        deepest, exact, chain = 0, title not in self.unbounded, []
        if title not in self.stack:
            self.unknown.add(title)
            exact = False
        for target in self.calls.get(title, []):
            definitions = self._define(target)
            if not definitions:
                self.unknown.add(target)
                exact = False
            for definition in definitions:
                d, e, c = self.depth(definition, path + (title,))
                exact &= e
                if d >= deepest: deepest, chain = d, c
        if any(site in self.unresolved for site in self.indirect.get(title, [])):
            exact = False
        result = (self.stack.get(title, 0) + deepest, exact, [title] + chain)
        # ponytail: results below a recursion are not cached, memoize per
        # strongly connected component if this gets slow on large projects
        if not any(c.endswith("(recursion)") for c in chain):
            self._cache[title] = result
        return result

    def roots(self):
        """Returns `[(bytes, exact, chain)]` of all functions without caller."""
        self.unknown, self.recursive, self._cache = set(), set(), {}
        called = self._called()
        # functions removed by the linker are not roots
        return sorted((self.depth(t) for t in self.stack
                       if t not in called and _bare(t) in self.linked),
                      key=lambda r: -r[0])


def format(elf, buildpath, hints=None, objdump="arm-none-eabi-objdump"):
    graph = CallGraph(elf, buildpath, hints, objdump)
    graph.resolve()
    roots = graph.roots()
    def names(titles):
        return [graph.label.get(t) or _demangle([_bare(t)])[0] for t in titles]
    def usage(need, exact, size, text):
        return "{}{}{:5} of {:5} bytes {:3.0f}%  {}".format(
            "!" if need > size else " ", " " if exact else ">", need, size, 100 * need / size, text)

    # An interrupt pushes its frame onto the current stack, but executes on the main stack
    frame = graph.value.get("EXCEPTION_FRAME_SIZE", 0)
    output = []
    for size, exact, chain in roots:
        if _bare(chain[0]) != "__modm_startup": continue
        handlers = {t for name, table in graph.table.items() if name.startswith("__vector_table")
                    for t in table if t in graph.stack}
        hsize, hexact, hchain = max((graph.depth(t) for t in handlers), default=(0, True, [""]))
        output.append("Main stack:\n" + usage(
            size + frame + hsize, exact and hexact,
            graph.value["__main_stack_top"] - graph.value["__main_stack_bottom"],
            "{} main + {} interrupt entry + {} {}".format(size, frame, hsize, names(hchain[:1])[0])))
        output.append("   Assuming that interrupts do not preempt each other.\n")

    fibers = [(r, fiber) for r in roots if (fiber := _FIBER.match(graph.label.get(r[2][0], "")))]
    if fibers: output.append("Fiber stacks:")
    stacks = []
    for (_, _, chain), fiber in fibers:
        size, exact, _ = graph.fiber(chain[0], fiber[2])
        # The function and its argument are stored at the top of the fiber stack
        size += 2 * graph.width + frame
        stacks.append((size, usage(size, exact, int(fiber[1]), fiber[2])))
    output += [text for _, text in sorted(stacks, reverse=True)]
    if fibers:
        output.append("   Including {} bytes interrupt entry, excluding captured variables of lambdas.\n"
                      .format(frame))

    output.append("Call graph roots:")
    for size, exact, chain in roots:
        output.append("{}{:5}  {}".format(" " if exact else ">", size, names(chain[:1])[0]))
        output.append("          " + " > ".join(n[:40] for n in names(chain[1:])))
    if graph.unresolved:
        output.append("\nUnresolved indirect calls:\n  " + "\n  ".join(sorted(graph.unresolved)))
    for cycle in sorted(graph.recursive):
        output.append("\nRecursion:\n  " + "\n  > ".join(names(cycle)))
    if graph.unknown:
        output.append("\nFunctions without stack usage information:\n  " +
                      "\n  ".join(sorted(names(graph.unknown))))
    return "\n".join(output)


# -----------------------------------------------------------------------------
if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Worst-case stack usage analysis.")
    parser.add_argument(dest="source", metavar="ELF")
    parser.add_argument(dest="buildpath", help="Folder containing the .ci files.")
    parser.add_argument("--hint", action="append", metavar="SITE=TARGET",
                        help="Indirect calls in source lines matching SITE may reach "
                             "functions matching TARGET.")
    parser.add_argument("--objdump", default="arm-none-eabi-objdump")

    args = parser.parse_args()
    print(format(args.source, args.buildpath, args.hint, args.objdump))
