#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright (c) 2018, Niklas Hauser
#
# This file is part of the modm project.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
# -----------------------------------------------------------------------------

import os
import re
import sys
import graphviz as gv

from pathlib import Path
from jinja2 import Environment
from collections import defaultdict

def repopath(path):
    return Path.cwd().parents[1] / path
def relpath(path):
    return os.path.relpath(path, str(repopath(".")))

sys.path.append(str(repopath("ext/modm-devices/tools/generator")))
sys.path.append(str(repopath("ext/modm-devices/tools/device")))
from dfg.device_tree import DeviceTree
from modm.device_identifier import *
sys.path.append(str(repopath("../lbuild")))
import lbuild

builder = lbuild.api.Builder()

# class Option:
#     def __init__(self):
#         self.type = None
#         self.value = list()
#         self.values = list()
#         self.targets = list()
#         self.description = ""
#         self.dependencies = defaultdict(list)

#     def convert(self):
#         self.description = str(self.description)
#         self.targets = MultiDeviceIdentifierSet(self.targets)
#         self.value = sorted(list(set(self.value)))
#         self.values = sorted(list(set(self.values)))
#         self.dependencies = {
#             valin: sorted(list(set(("modm" + dep).replace("modmmodm", "modm") for dep in deps)))
#             for valin, deps in self.dependencies.items()
#         }

# class Module:
#     def __init__(self):
#         self.name = None
#         self.filename = None
#         self.parent = None
#         self.targets = list()
#         self.options = defaultdict(Option)
#         self.dependencies = defaultdict(list)
#         self.description = ""

#     def convert(self):
#         self.description = str(self.description)
#         self.targets = MultiDeviceIdentifierSet(self.targets)
#         for o in self.options.values():
#             o.convert()
#         self.dependencies = {
#             ("modm" + dep).replace("modmmodm", "modm"):
#             MultiDeviceIdentifierSet(targets)
#             for dep, targets in self.dependencies.items()
#         }


def get_modules():
    builder._load_repositories(repopath("repo.lb"))
    option = builder.parser.find_option(":target")

    targets = list(set(option.values) - set(d for d in repopath("test/all/ignored.txt").read_text().strip().splitlines() if "#" not in d))
    targets = sorted(targets)
    # targets = sorted(targets[:50])

    # Prime the repositories and get all module files
    mfiles = []
    option.value = targets[0]
    for repo in builder.parser._findall(builder.parser.Type.REPOSITORY):
        repo.prepare()
        mfiles.extend([(repo, file) for file in repo._module_files])

    num_modules = []
    num_options = []

    print("Querying for {} targets...".format(len(targets)))
    mtargets = []
    for target in targets:
        option.value = target
        target = option.value._identifier

        imodules = [m for (repo, mfile) in mfiles for m in lbuild.module.load_module_from_file(repo, mfile)]
        imodules = [m for m in imodules if m.available]
        imodules = sorted(imodules, key=lambda m: m.fullname.count(":"))

        num_modules.append(len(imodules))
        print("{}: {}".format(target, len(imodules)))

        modules = {"modm": DeviceTree("modm", target)}
        modules["modm"].addSortKey(lambda mo: (mo.name, mo["filename"]))
        for init in imodules:
            m = modules[init.parent].addChild(init.fullname.split(":")[-1])
            modules[init.fullname] = m
            m.addSortKey(lambda mo: (mo.name, m["filename"], mo.get("value", ""), mo.get("option", "")))
            m.setAttribute("filename", relpath(init.filename))
            m.setAttribute("_description", init.description)
            # for d in (init._dependencies + [init.parent]):
            for d in init._dependencies:
                dep = m.addChild("dependency")
                dep.setValue(("modm" + d).replace("modmmodm", "modm"))

            num_options.append(len(init._options))
            for o in init._options:
                op = m.addChild(o.fullname.split(":")[-1])
                op.addSortKey(lambda oo: (oo.name, oo.get("value", "")))
                op.setAttribute("option", type(o).__name__.replace("Option", "").lower())
                op.setAttribute("_description", o._description)
                for value in lbuild.utils.listrify(o._input):
                    opval = op.addChild("value")
                    opval.setValue(value)
                for value in o.values:
                    opvals = op.addChild("values")
                    opvals.setValue(value)
                if o._dependency_handler:
                    for valin in o.values:
                        valout = o._dependency_handler(o._in(valin))
                        if valout:
                            opdep = op.addChild("dependency")
                            opdep.setValue("{}->{}".format(valin, ("modm" + valout).replace("modmmodm", "modm")))

        mtargets.append(modules["modm"])

    print("Merging module tree...")
    mfinal = mtargets[0]
    for mtarg in mtargets[1:]:
        mfinal.merge(mtarg)
    print("Sorting module tree...")
    mfinal._sortTree()

    print("Printing module tree...")
    print(mfinal.toString())
    exit(1)

    return mfinal, (num_modules, num_options)


def split_description(descr):
    lines = descr.strip(" \n").splitlines() + [""]
    return lines[0].replace("#", "").strip(), "\n".join(lines[1:])

def extract(text, key):
    return re.search(r"# {0}\n(.*?)\n# /{0}".format(key), text, flags=re.DOTALL | re.MULTILINE).group(1)

def replace(text, key, content):
    return re.sub(r"# {0}.*?# /{0}".format(key), "# {0}\n{1}\n# /{0}".format(key, content), text, flags=re.DOTALL | re.MULTILINE)

def url_name(name):
    for c in ":., ({": name = name.replace(c, "-");
    for c in "})":     name = name.replace(c, "");
    return name

def ref_name(name):
    return name.replace(":", "_").replace(".", "_")

def node_name(name):
    return name.replace(":", ":\n")

def get_platform_families(mids):
    dids = defaultdict(set)
    for mid in mids:
        for did in mid:
            dids[did.platform].add(did.family)
    return dids

all_dids = None
def target_diff(targets, compare=None):
    global all_dids, all_targets
    if all_dids is None:
        all_dids = get_platform_families(all_targets)
    if compare is None:
        compare = all_targets

    if compare == targets:
        return ""

    if len(targets) == 1:
        return list(targets.mids.values())[0][0].string

    output = []
    for p, fs in get_platform_families(targets).items():
        if fs != all_dids[p]:
            if len(fs) == 1:
                p += list(fs)[0]
            else:
                p += "{" + ",".join(sorted(fs)) + "}"
        output.append(p)
    return ", ".join(sorted(set(output)))



lbuild.logger.configure_logger(2)
lbuild.format.plain = True
module_tree, omodules, mlens = get_modules()

print(f"Total: {sum(mlens[0])} modules and {sum(mlens[1])} options generated.\n"
      f"Between {min(mlens[0])} and {max(mlens[0])} modules per target.\n"
      f"Between {min(mlens[1])} and {max(mlens[1])} options per module.")

modules = []
for fullname in sorted(omodules.keys()):
    for filename, module in omodules[fullname].items():
        title, descr = split_description(module.description)
        deps = {d: target_diff(targets, compare=module.targets) for d, targets in module.dependencies.items()}
        tdiff = target_diff(module.targets)
        refname = fullname
        if len(tdiff) and len(module.targets) != 1 and len(omodules[fullname]) > 1:
            refname += f" ({tdiff})"
        mprops = {"name": fullname,
                  "filename": filename,
                  "title": title,
                  "description": descr,
                  "targets": tdiff,
                  "ref": refname,
                  "dependencies": deps,
                  "options": []}
        for name, o in module.options.items():
            deps.update({d: f"{name}.{ins}" for ins, ds in o.dependencies.items() for d in ds})
            title, descr = split_description(o.description)
            oprops = {"name": name,
                      "title": title,
                      "description": descr,
                      "targets": target_diff(o.targets, compare=module.targets),
                      "dependencies": sorted(d for ld in o.dependencies.values() for d in ld),
                      "type": o.type,
                      "ovalue": o.value,
                      "ovalues": o.values}
            mprops["options"].append(oprops)
        modules.append(mprops)

modtable = []
for m in modules:
    # Compute and generate dependency SVG
    m["dependents"] = []
    for d in modules:
        if m["name"] in d["dependencies"].keys():
            m["dependents"].append(d["name"])

    graph = gv.Digraph(name=m["name"],
                       format="svg",
                       graph_attr={"rankdir": "BT"},
                       node_attr={"style": "filled,solid", "shape": "box"})
    graph.node(ref_name(m["name"]), node_name(m["name"]), style="filled,bold")

    for mod in sorted(list(m["dependencies"].keys()) + m["dependents"]):
        graph.node(ref_name(mod), node_name(mod), URL=("../" + url_name(mod)))
    for mod in sorted(m["dependencies"].keys()):
        graph.edge(ref_name(m["name"]), ref_name(mod))
    for mod in sorted(m["dependents"]):
        graph.edge(ref_name(mod), ref_name(m["name"]))

    m["graph"] = graph.pipe().decode("utf-8")

    # Render the actual documentation
    rendered = Environment().from_string(repopath("docs/module.md.in").read_text()).render({"module": m})
    repopath("docs/src/reference/module/{}.md".format(url_name(m["ref"]))).write_text(rendered)
    modtable.append("      - \"{}\": \"reference/module/{}.md\"".format(m["ref"], url_name(m["ref"])))

# Update the
config_path = Path(repopath("docs/mkdocs.yml"))
modtable = "\n".join(modtable)
config = config_path.read_text()
if extract(config, "moduletable") != modtable:
    config = replace(config, "moduletable", modtable)
    config_path.write_text(config)