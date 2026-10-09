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

Two results are computed: for normal operation, and including the calls that
are only made on failure, which are marked with a comment `stack-usage: fatal`
on their line of source code. In modm this is the call of `modm_abandon`.

Interrupts execute on the main stack and are preempted by interrupts of another
priority, so the main stack must fit the deepest handler of every priority
that the enabled interrupts may have. Both are read from the writes to the NVIC
registers. If these are not constant, every interrupt with a handler is assumed
to preempt all others. A fault is a failure that can occur at any time.

```sh
python3 -m modm_tools.stack_usage path/to/project.elf path/to/buildfolder
```

Indirect calls are resolved with the optimized GIMPLE that GCC dumps when
compiling with `-fdump-tree-optimized-lineno-asmname`, in this order:

1. Hints passed as `--hint "site=target"` or written as comment
   `stack-usage: calls target` on the source code line of the call. `site` is a
   regex searched in `path/to/file.cpp: source code line` of the call. The call
   may then reach all functions whose signature matches the regex `target`, all
   functions in the table between the linker symbols `name_start` and
   `name_end` if the target is `table:name`, or `nothing`. The target `fatal`
   marks any call as only made on failure.
2. Virtual calls reach the functions in the called slot of the vtables, that
   belong to a base or derived class of the called class.
3. Calls of a function pointer loaded from a linker table or from an object
   reach the functions stored in it.
4. All other calls of a function pointer reach all functions with the same
   signature, whose address is stored outside of vtables and linker tables.
5. Everything else is listed as unresolved and the result is a lower bound.

The function, argument and stack of a fiber are taken from the arguments of
its call to `modm_context_init`. A fiber that is not constructed from constants
is only known by its type and may execute any function called via rule 4.

The stack usage of functions written in assembly is taken from the call frame
information and their indirect calls reach the functions in their literal pool.
A symbol `__modm_stack_usage.function = bytes` overrides the stack usage for
naked functions, which have neither.

A result prefixed with `>` is only a lower bound due to unresolved indirect
calls, recursion, unbounded dynamic stack allocations or functions without
stack usage information (assembly, precompiled libraries). For a recursion
every function of its cycle is assumed to be called once.
"""

import re
import sys
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
_LOCATION = r"\[([^\]\[()\s]+:\d+:\d+)(?: discrim \d+)?\] ?"
_TOKEN = r"[A-Za-z_$][\w.$]*"


def _split(text):
    """Splits a list at its top level commas."""
    depth, parts = 0, [""]
    for char in text:
        depth += (char in "(<[") - (char in ")>]")
        if char == "," and depth <= 0: parts.append("")
        else: parts[-1] += char
    return [part.strip() for part in parts if part.strip() not in ("", "void")]


def _signature(result, parameters):
    """Returns the signature of a function type and its shape of pointers and references."""
    clean = lambda t: re.sub(r" const$", "", re.sub(r"\s+", " ", re.sub(r"<T[0-9a-f]+>", "", t)).strip())
    signature = clean(result) + "(" + ",".join(clean(p) for p in parameters) + ")"
    return signature, signature[:5].replace("void(", "v") + re.sub(r"[^*&(),]", "", signature)


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
        self.hints = [h.split("=", 1) for h in (hints or [])]
        self.sites = defaultdict(list)   # (symbol, call site) and call site -> callees
        self.signature = {}              # symbol -> (signature, parameters)
        self.mentioned = set()           # all names used other than being called
        self.fibers = {}                 # object -> (stack bottom, top, entry names, function names)
        self.direct = defaultdict(set)   # (title, called title) -> call sites
        self.fatal = set()               # (title, called title) that are only called on failure
        self.enabled = set()             # enabled interrupts, None if not constant
        self.priority = set()            # (symbol, interrupt, priority), None if not constant
        self.vectors = {}                # index in vector table -> titles

        files = sorted(Path(buildpath).rglob("*.ci"), key=lambda f: f.stat().st_mtime)
        # With LTO the whole call graph is generated last by the linker, any
        # other call graphs are leftovers of previous builds and vice versa
        lto = bool(files) and ".ltrans" in files[-1].name
        files = [f for f in files if (".ltrans" in f.name) == lto]
        for file in files:
            self._parse(file)
        for file in Path(buildpath).rglob("*.optimized"):
            if (".ltrans" in file.name) == lto: self._parse_dump(file)
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
                else:
                    self.calls[source].add(target)
                    if site: self.direct[(source, target)].add(site)

    def _parse_dump(self, file):
        """Finds the type of all indirect calls in the optimized GIMPLE."""
        for block in re.split(r"\n(?=;; Function )", file.read_text(errors="replace")):
            if not (head := re.match(r";; Function (.*) \((\S+), funcdef_no", block)): continue
            name, symbol = head.groups()
            header, _, body = block.partition("\n{\n")
            prototype = header.splitlines()[-1]
            # the parameter list is the last group of parentheses
            depth, start = 0, len(prototype)
            while start and (depth or start == len(prototype)):
                start -= 1
                depth += (prototype[start] == ")") - (prototype[start] == "(")
            parameters = _split(prototype[start + 1:-1])
            result = prototype[:start].rstrip()
            result = result[:-len(name)] if result.endswith(name) else result.rpartition(" ")[0]
            self.signature[symbol] = _signature(result, [re.sub(r"\s*[\w.$]+$", "", p) for p in parameters])
            types = {p.rsplit(" ", 1)[-1]: p.rsplit(" ", 1)[0] for p in parameters if " " in p}
            types.update((n, t) for t, n in re.findall(r"^  (\S.*?) (\S+);$", body.split("<bb", 1)[0], re.M))
            statements, definitions = [], {}
            for line in body.splitlines():
                location = re.search(_LOCATION, line)
                line = re.sub(_LOCATION, "", line).strip()
                if "# DEBUG" in line: continue
                if define := re.match(r"(?:# )?(" + _TOKEN + r") = (.*)", line):
                    definitions[define[1]] = define[2]
                statements.append((location and location[1], line))
            def trace(name):
                """Follows the definitions of a name back to where its value is from."""
                origin, text, todo, known = {name}, name, [name], dict(definitions)
                while todo and len(origin) < 100:
                    text += " " + known.get(todo[-1], "")
                    for token in re.findall(_TOKEN, known.pop(todo.pop(), "")):
                        origin.add(token)
                        todo.append(token)
                return origin, text

            for location, line in statements:
                if "modm_context_init (" in line:
                    # A fiber is constructed from its stack, entry function and argument
                    arguments = _split(line[line.index("modm_context_init (") + 19:line.rindex(")")])
                    traced = [trace(t) for a in arguments for t in re.findall(_TOKEN + "$", a) or [""]]
                    if len(traced) == 5 and (name := re.search(r"&(" + _TOKEN + r")[\] ]", arguments[0])):
                        # the offset into the object plus what is added to that address
                        offset = lambda a, t: sum(map(int, re.findall(
                            r"&" + re.escape(name[1]) + r" \+ (\d+)", a + t)[:1] +
                            re.findall(r"_\d+ \+ (\d+)\b", t)))
                        self.fibers[name[1]] = (offset(arguments[1], traced[1][1]), offset(arguments[2], traced[2][1]),
                                                traced[3][0], traced[4][0])
                if nvic := re.search(r"NVIC_Type \*\)\d+B\]\.(IPR?|ISER)\[([^\]]+)\] =(?:\{v\})? ([^;]+);", line):
                    # Interrupts are enabled and their priority is set via these registers
                    register, index, value = nvic.groups()
                    constant = index.isdigit() and value.isdigit()
                    number = lambda text: int(text) if text.isdigit() else None
                    if register != "ISER": self.priority.add((symbol, number(index), number(value)))
                    elif not constant: self.enabled.add(None)
                    else: self.enabled |= {int(index) * 32 + bit for bit in range(32) if int(value) >> bit & 1}
                # names that are not called directly may have their address taken
                self.mentioned.update(re.findall(r"(?<![\w.$])(" + _TOKEN + r")(?! \()", line))
                if virtual := re.search(r"OBJ_TYPE_REF\([^;]+;\((.*?)\)[^;]*?->(\d+)B\) \(", line):
                    callee = {"slot": int(virtual[2]), "class": virtual[1]}
                elif call := re.search(r"(?:^|= )(" + _TOKEN + r")(?:\(D\))? \(", line):
                    # the type of the SSA name is declared via its variable
                    kind = types.get(call[1]) or types.get(re.sub(r"_\d+$", "", call[1]), "")
                    if not (pointer := re.match(r"(.*?) ?\(\*[^)]*\) \((.*)\)$", kind)): continue
                    # the pointer may be loaded from a linker table or an object
                    callee = {"type": _signature(pointer[1], _split(pointer[2])), "origin": trace(call[1])[0]}
                else: continue
                self.sites[(symbol, location)].append(callee)
                self.sites[location].append(callee)

    def _parse_classes(self, dwarf):
        """Finds the base classes and member functions of all classes."""
        self.bases = defaultdict(set)   # class -> base classes by qualified name
        self.derived = defaultdict(set) # class -> derived classes
        self.diamond = set()            # classes with a virtual base class
        self.owner = {}                 # function symbol -> class
        classes = ("DW_TAG_class_type", "DW_TAG_structure_type")
        symbol = lambda die: die.attributes["DW_AT_linkage_name"].value.decode()

        def qualified(die):
            names = []
            while die is not None and die.tag != "DW_TAG_compile_unit":
                names.append(die.attributes["DW_AT_name"].value.decode()
                             if "DW_AT_name" in die.attributes else "")
                die = die.get_parent()
            return "::".join(reversed(names))

        def walk(parent):
            for die in parent.iter_children():
                if die.tag in classes:
                    name = qualified(die)
                    for member in die.iter_children():
                        if member.tag == "DW_TAG_inheritance":
                            base = member.get_DIE_from_attribute("DW_AT_type")
                            while base.tag not in classes and "DW_AT_type" in base.attributes:
                                base = base.get_DIE_from_attribute("DW_AT_type")
                            self.bases[name].add(qualified(base))
                            self.derived[qualified(base)].add(name)
                            if "DW_AT_virtuality" in member.attributes: self.diamond.add(name)
                        elif member.tag == "DW_TAG_subprogram" and "DW_AT_linkage_name" in member.attributes:
                            self.owner[symbol(member)] = name
                    walk(die)
                elif die.tag == "DW_TAG_namespace": walk(die)
                elif die.tag == "DW_TAG_subprogram" and "DW_AT_linkage_name" in die.attributes:
                    # constructors and destructors only have a symbol at their definition
                    declaration = die
                    for attribute in ("DW_AT_abstract_origin", "DW_AT_specification"):
                        if attribute in declaration.attributes:
                            declaration = declaration.get_DIE_from_attribute(attribute)
                    if (owner := declaration.get_parent()) is not None and owner.tag in classes:
                        self.owner[symbol(die)] = qualified(owner)

        for unit in dwarf.iter_CUs():
            walk(unit.get_top_DIE())

    def _virtual_call(self, callee, named):
        """Returns all functions a virtual call may reach."""
        def closure(classes, relatives):
            classes, todo = set(classes), list(classes)
            while todo:
                for relative in relatives.get(todo.pop(), set()) - classes:
                    classes.add(relative)
                    todo.append(relative)
            return classes

        # A pure virtual function is only reached while constructing the object
        slot = {t for t in self.slot[callee["slot"]] if not _bare(t).startswith("__cxa_")}
        # The class is not qualified and with LTO it is mangled
        name = re.findall(r"[A-Za-z_]\w*", re.sub(r"^N(.*)E$", r"\1", callee["class"]))[-1]
        classes = {c for c in self.bases.keys() | self.derived.keys() | set(self.owner.values())
                   if re.sub(r"<.*", "", c).split("::")[-1] == name}
        if not classes: return (slot & named) or slot
        # The function is implemented by a derived class or inherited from a base class
        bases = closure(classes, self.bases)
        classes = closure(classes, self.derived) | bases
        # With virtual inheritance it may also be implemented by a sibling class
        if classes & self.diamond: classes = closure(bases, self.derived)
        # A thunk adjusts the object and belongs to the class of its function
        owner = lambda t: self.owner.get(re.sub(r"^_ZT[hv][\dn_]+_", "_Z", _bare(t)))
        # functions without debug information cannot be excluded
        return {t for t in slot if owner(t) is None or owner(t) in classes}

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
            self.entry = functions.get(elffile.header["e_entry"] & ~1, [])
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
            self._parse_classes(elffile.get_dwarf_info())
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
                    # an indirect call can only reach the functions in the literal pool
                    pool = {name for word in re.findall(r"\.word\t0x([0-9a-f]+)", function)
                            for name in functions.get(int(word, 16) - 1, [])}
                    self.calls[title] |= pool
                    if not pool: self.indirect[title].add(title + " (disassembly)")
                # without call frame info only functions not using the stack are known
                if not re.search(r"\bsp\b|push", function):
                    self.stack.setdefault(title, 0)

            def pointers(index, address, size):
                """Yields `(address, index, title)` of all functions an object points to."""
                section = elffile.get_section(index)
                if section["sh_type"] == "SHT_NOBITS": return
                offset = address - section["sh_addr"]
                data = section.data()[offset:offset + size]
                for ii in range(0, len(data) - width + 1, width):
                    value = int.from_bytes(data[ii:ii + width], "little")
                    for name in functions.get(value & ~1, []):
                        for title in self.bare.get(name, []):
                            yield address + ii, ii // width, title

            self.slot = defaultdict(set)    # vtable slot -> titles
            self.object = defaultdict(set)  # object -> titles
            self.ranges = {}                # object or table -> (start, end)
            starts = {s.name[:-6]: s for s in symbols if s.name.endswith("_start")}
            for end in (s for s in symbols if s.name.endswith("_end")):
                start, size = starts.get(end.name[:-4]), 0
                if start: size = end["st_value"] - start["st_value"]
                if start and size >= 0 and isinstance(start["st_shndx"], int):
                    entries = list(pointers(start["st_shndx"], start["st_value"], size))
                    self.table[end.name[:-4]] = {t for _, _, t in entries}
                    if end.name.startswith("__vector_table") and entries:
                        self.vectors = defaultdict(set)
                        for _, index, title in entries: self.vectors[index].add(title)
                    self.ranges[end.name[:-4]] = (start["st_value"], end["st_value"])
            self._virtual = set()
            for symbol in symbols:
                if symbol["st_info"]["type"] != "STT_OBJECT" or not isinstance(symbol["st_shndx"], int):
                    continue
                slot, last = 0, None
                for _, index, title in pointers(symbol["st_shndx"], symbol["st_value"], symbol["st_size"]):
                    if symbol.name.startswith("_ZTV"):
                        # A vtable consists of one table per base class with virtual
                        # functions, each preceded by words that are not functions
                        if last is not None and index > last: slot = slot + 1 if index == last + 1 else 0
                        last = index
                        self.slot[slot].add(title)
                        self._virtual.add(title)
                    else: self.object[symbol.name].add(title)
                if symbol.name.startswith("_ZTV"):
                    self.ranges[symbol.name] = (symbol["st_value"], symbol["st_value"] + symbol["st_size"])
            # all functions whose address is stored in data
            self.stored = []
            for index, section in enumerate(elffile.iter_sections()):
                # allocated, but not executable
                if section["sh_type"] == "SHT_PROGBITS" and section["sh_flags"] & 0x2 and \
                   not section["sh_flags"] & 0x4:
                    self.stored += [(a, t) for a, _, t in
                                    pointers(index, section["sh_addr"], section["sh_size"])]

    def _pointer(self, callee):
        """Returns all functions a function pointer may point to or None if unknown."""
        targets, known = set(), False
        for origin in callee["origin"]:
            if origin.endswith("_start") and origin[:-6] in self.table:
                self.tables.add(origin[:-6])
                targets |= self.table[origin[:-6]]
                known = True
            targets |= self.object.get(origin, set())
        stored = known or targets
        if not stored:
            # only functions stored outside of vtables and tables that are iterated
            ranges = [r for name, r in self.ranges.items()
                      if name.startswith(("_ZTV", "__vector_table")) or name in self.tables]
            targets = {t for a, t in self.stored if not any(s <= a < e for s, e in ranges)}
            targets |= {t for name in self.mentioned & self.linked for t in self.bare[name]}
        # Types are compared as written, however a typedef differs from its type
        # and LTO loses some names, then only the shape of the types is compared.
        # ponytail: compare the types of the debug information if this is too coarse
        for index in (0, 1):
            if same := {t for t in targets
                        if self.signature.get(_bare(t), ("", ""))[index] == callee["type"][index]}:
                return same
        # an object may also store functions of unknown type
        return targets if stored else None

    def _code(self, site):
        """Returns `(file, column, source code line)` of a call site."""
        try:
            file, line, column = site.rsplit(":", 2)
            if file not in self._files:
                try: self._files[file] = Path(file).read_text(errors="replace").splitlines()
                except OSError: self._files[file] = []
            return file, int(column), self._files[file][int(line) - 1]
        except (ValueError, IndexError):
            return site, 1, ""

    def _hints(self, site):
        """Returns the hints for a call site."""
        file, _, code = self._code(site)
        return [target for hint, target in self.hints if re.search(hint, file + ": " + code)] + \
            re.findall(r"stack-usage: (?:calls )?(.+?)\s*(?:\*/)?$", code)

    def _targets(self, source, site):
        """Resolves an indirect call site to all functions it may call."""
        _, column, code = self._code(site)
        # the column points either at the callee or its opening parenthesis
        names = re.findall(r"[A-Za-z_]\w*", code[:column - 1])[-1:] + \
                re.findall(r"^[\W]*([A-Za-z_]\w*)", code[column - 1:])
        for target in self._hints(site):
            if target in ("", "nothing"): return set()
            if target == "fatal": continue
            if target.startswith("table:"):
                return self.table.get(target[len("table:"):], set())
            return {t for t in self.stack if re.search(target, self.label[t])}
        named = set().union(*(self.virtual[name] for name in names if name in self.virtual))
        callees = self.sites.get((_bare(source), site)) or self.sites.get(site)
        if not callees:
            # without GIMPLE virtual calls can only be matched by name
            if not named: self.unresolved.add(site)
            return named
        targets = set()
        for callee in callees:
            if "slot" in callee: targets |= self._virtual_call(callee, named)
            elif (pointer := self._pointer(callee)) is None: self.unresolved.add(site)
            else: targets |= pointer
        return targets

    def resolve(self):
        self._files = {}     # file -> lines of source code
        self.entries = set() # titles that call the function of a fiber
        self.tables = set()  # linker tables that are iterated
        for source, sites in self.indirect.items():
            for site in sites:
                if "fiber/task_impl.hpp" in site and _FIBER.match(self.label.get(source, "")):
                    self.entries.add(source)
                    continue
                targets = self._targets(source, site)
                self.calls[source] |= targets
                if "fatal" in self._hints(site):
                    self.fatal |= {(source, target) for target in targets}
        # A call is fatal, if the program does not continue normally after it
        self.fatal |= {edge for edge, sites in self.direct.items()
                       if any("fatal" in self._hints(site) for site in sites)}
        # A fiber constructed from a function may execute any function without arguments
        self.free = self._pointer({"type": _signature("void", []), "origin": set()}) or set()

    def _reset(self):
        # results with and without fatal calls
        self._cache, self._cycle, self._index = ({False: {}, True: {}} for _ in range(3))

    def fiber(self, title, targets):
        """Returns `(bytes, exact, chain)` without and with fatal calls of a
        fiber entry calling its function."""
        # LTO merges identical entry functions, thus only the root tells the
        # fibers apart and each one must be evaluated with its own function
        calls = self.calls
        self.calls = defaultdict(set, calls)
        for entry in self.entries:
            self.calls[entry] = calls[entry] | targets
        self._reset()
        result = self.depth(title, False), self.depth(title, True)
        self.calls = calls
        self._reset()
        return result

    def _called(self):
        return {d for targets in self.calls.values() for t in targets for d in self._define(t)}

    def _define(self, title):
        """Returns the titles of all definitions a call target may refer to."""
        # The linker may replace weak and inline functions with another definition
        if title in self.stack and _bare(title) not in self.globals: return [title]
        return self.bare.get(_bare(title), [])

    def _callees(self, title, fatal):
        """Returns the definitions of all functions a function calls."""
        return sorted({d for target in self.calls.get(title, [])
                       if fatal or (title, target) not in self.fatal for d in self._define(target)})

    def _find_cycles(self, title, fatal):
        """Assigns all functions reachable from a function to their recursion cycle."""
        index, low, stack = self._index[fatal], {}, []
        def visit(node):
            # Tarjan's strongly connected components algorithm
            index[node] = low[node] = len(index)
            stack.append(node)
            for callee in self._callees(node, fatal):
                if callee not in index:
                    visit(callee)
                    low[node] = min(low[node], low[callee])
                elif callee in stack: low[node] = min(low[node], index[callee])
            if low[node] == index[node]:
                cycle = tuple(sorted(stack[stack.index(node):]))
                del stack[stack.index(node):]
                for member in cycle: self._cycle[fatal][member] = cycle
        sys.setrecursionlimit(max(sys.getrecursionlimit(), 10000))
        visit(title)

    def depth(self, title, fatal=True):
        """Returns `(bytes, exact, deepest call chain)` of a function, optionally
        without the calls that are only made on failure."""
        if title not in self._cycle[fatal]: self._find_cycles(title, fatal)
        cycle = self._cycle[fatal][title]
        if cycle not in self._cache[fatal]:
            # The depth of a recursion is unknown, so as lower bound every
            # function of its cycle is assumed to be called once
            size, deepest = 0, (0, True, [])
            exact = len(cycle) == 1 and title not in self._callees(title, fatal)
            if not exact: self.recursive.add(cycle)
            for member in cycle:
                size += self.stack.get(member, 0)
                missing = {t for t in self.calls.get(member, []) if not self._define(t)}
                if member not in self.stack: missing.add(member)
                self.unknown |= missing
                if missing or member in self.unbounded or \
                   any(site in self.unresolved for site in self.indirect.get(member, [])):
                    exact = False
                for callee in self._callees(member, fatal):
                    if callee not in cycle:
                        deepest = max(deepest, self.depth(callee, fatal), key=lambda result: result[0])
            chain = ["(recursion of {} functions)".format(len(cycle))] if len(cycle) > 1 else []
            self._cache[fatal][cycle] = (size + deepest[0], exact and deepest[1], chain + deepest[2])
        size, exact, chain = self._cache[fatal][cycle]
        return size, exact, [title] + chain

    def roots(self):
        """Returns `[(bytes, exact, chain)]` of all functions without caller."""
        self.unknown, self.recursive = set(), set()
        self._reset()
        called = self._called()
        # functions removed by the linker are not roots
        return sorted((self.depth(t) for t in sorted(self.stack)
                       if t not in called and _bare(t) in self.linked),
                      key=lambda r: -r[0])

    def interrupts(self):
        """Returns `({level: titles}, titles)` of the handlers of all interrupts
        grouped by what cannot preempt each other, and of all faults."""
        handlers = {index: frozenset(t for t in titles if t in self.stack)
                    for index, titles in self.vectors.items() if index >= 2}
        if not handlers: return {}, set()
        # unused interrupts all have the same handler
        unused = max(handlers.values(), key=list(handlers.values()).count)
        interrupts = {index - 16 for index in handlers if index >= 16 and handlers[index] != unused}
        # An interrupt that is never enabled cannot occur
        if self.enabled and None not in self.enabled: interrupts = self.enabled & {i - 16 for i in handlers}
        faults = set().union(*(handlers.get(index, set()) for index in range(2, 7)))
        # The handlers of faults and unused interrupts only change priorities on failure
        failed = {_bare(t) for t in faults | unused}
        priorities, levels = defaultdict(set), defaultdict(set)
        for symbol, interrupt, priority in self.priority:
            if symbol not in failed: priorities[interrupt].add(priority)
        for interrupt in sorted(interrupts):
            # The priority after reset is 0, and a priority that is set for an
            # unknown interrupt may be set for every interrupt
            priority = (priorities.get(interrupt) or {0}) | priorities.get(None, set())
            # Only an interrupt of another priority preempts, so an interrupt
            # adds to the level of every priority it may have
            if None in priority: levels["interrupt {}".format(interrupt)] |= handlers[interrupt + 16]
            else:
                for value in priority: levels["priority {:3}".format(value)] |= handlers[interrupt + 16]
        # ponytail: the priority of system exceptions is not read from the SCB,
        # so each is assumed to preempt everything else
        for index, name in ((11, "SVCall"), (12, "DebugMonitor"), (14, "PendSV"), (15, "SysTick")):
            if handlers.get(index, unused) != unused: levels[name] |= handlers[index]
        return levels, faults


def format(elf, buildpath, hints=None, objdump="arm-none-eabi-objdump"):
    graph = CallGraph(elf, buildpath, hints, objdump)
    graph.resolve()
    roots = graph.roots()
    def names(titles):
        return [graph.label.get(t) or _demangle([_bare(t)])[0] for t in titles]
    def usage(normal, fatal, exact, size, text):
        return "{}{}{:5} {:6} of {:5} bytes {:3.0f}%  {}".format(
            "!" if fatal > size else " ", " " if exact else ">", normal, fatal, size, 100 * fatal / size, text)
    def deepest(titles, fatal):
        return max((graph.depth(t, fatal) for t in sorted(titles)), default=(0, True, [""]))

    # An interrupt pushes its frame onto the current stack, but executes on the main stack
    frame = graph.value.get("EXCEPTION_FRAME_SIZE", 0)
    output = ["Stack usage in bytes during normal operation and including calls on failure:\n"]
    for _, _, chain in roots:
        # The main stack starts at the entry point of the program
        if _bare(chain[0]) not in graph.entry: continue
        levels, faults = graph.interrupts()
        rows = [(deepest(chain[:1], False), deepest(chain[:1], True), 0, "main")]
        # Every level of interrupts that preempt each other adds its deepest handler
        rows += [(deepest(titles, False), deepest(titles, True), frame,
                  "{}: {}".format(level, names(deepest(titles, True)[2][:1])[0]))
                 for level, titles in sorted(levels.items())]
        # A fault is a failure and can occur anywhere
        if faults: rows.append(((-frame, True, []), deepest(faults, True), frame,
                                "fault: " + names(deepest(faults, True)[2][:1])[0]))
        output.append("Main stack:\n" + usage(
            sum(n[0] + entry for n, _, entry, _ in rows), sum(f[0] + entry for _, f, entry, _ in rows),
            all(n[1] and f[1] for n, f, _, _ in rows),
            graph.value["__main_stack_top"] - graph.value["__main_stack_bottom"], ""))
        output += ["  {:5} {:6}  {}".format(n[0] + entry, f[0] + entry, text) for n, f, entry, text in rows]
        if levels or faults: output.append("   Including {} bytes interrupt entry per level.".format(frame))
        output.append("")

    # The function and its argument are stored at the top of the fiber stack
    overhead, stacks, entries = 2 * graph.width + frame, {}, set()
    def stack(title, targets, size, text):
        normal, fatal = graph.fiber(title, targets)
        return (fatal[0], usage(normal[0] + overhead, fatal[0] + overhead, normal[1] and fatal[1], size, text))
    for name, (bottom, top, entry, function) in graph.fibers.items():
        titles = {t for e in entry & graph.linked for t in graph.bare[e] if t in graph.stack}
        targets = {t for f in function & graph.linked for t in graph.bare[f]}
        if not titles or top <= bottom: continue
        stacks[name] = max(stack(t, targets, top - bottom, _demangle([name])[0]) for t in sorted(titles))
        entries |= titles
    for _, _, chain in roots:
        # Fibers that are not constructed from a constant are only known by their type
        if chain[0] in entries or not (fiber := _FIBER.match(graph.label.get(chain[0], ""))): continue
        targets = graph.free if re.search(r"\([&*]\)", fiber[2]) else \
            {t for t in graph.stack if graph.label[t].startswith(fiber[2] + "::")}
        stacks[chain[0]] = stack(chain[0], targets, int(fiber[1]), fiber[2])
    if stacks:
        output.append("Fiber stacks:")
        output += [text for _, text in sorted(stacks.values(), reverse=True)]
        output.append("   Including {} bytes interrupt entry.\n".format(frame))

    output.append("Call graph roots:")
    for size, exact, chain in roots:
        normal = graph.depth(chain[0], False)
        output.append("{}{:5} {:6}  {}".format(" " if exact and normal[1] else ">", normal[0], size,
                                              names(chain[:1])[0]))
        output.append("                 " + " > ".join(n[:40] for n in names(chain[1:])))
    if graph.unresolved:
        output.append("\nUnresolved indirect calls:\n  " + "\n  ".join(sorted(graph.unresolved)))
    for cycle in sorted(graph.recursive):
        output.append("\nRecursion between:\n  " + "\n  ".join(names(cycle)))
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
