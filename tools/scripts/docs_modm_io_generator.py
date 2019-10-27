#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright (c) 2018, Niklas Hauser
# Copyright (c) 2018-2019, Raphael Lehmann
#
# This file is part of the modm project.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import os
import sys
import shutil
import lbuild
import zipfile
import tempfile
import argparse
import datetime
import multiprocessing

from pathlib import Path
from jinja2 import Environment
from collections import defaultdict

def repopath(path):
    return Path(__file__).absolute().parents[2] / path
def relpath(path):
    return os.path.relpath(path, str(repopath(".")))

sys.path.append(str(repopath("ext/modm-devices/tools/device")))
from modm_devices.device_identifier import *




def get_targets():
    builder = lbuild.api.Builder()
    builder._load_repositories(repopath("repo.lb"))
    option = builder.parser.find_option(":target")
    ignored = list(filter(lambda d: "#" not in d, repopath("test/all/ignored.txt").read_text().strip().splitlines()))
    raw_targets = sorted(d for d in option.values if not any(d.startswith(i) for i in ignored))
    minimal_targets = defaultdict(list)

    for target in raw_targets:
        option.value = target
        target = option.value._identifier
        short_id = target.copy()

        if target.platform == "avr":
            short_id.naming_schema = short_id.naming_schema \
                .replace("{type}-{speed}{package}", "") \
                .replace("-{speed}{package}", "")

        elif target.platform == "stm32":
            short_id.naming_schema = "{platform}{family}{name}"

        elif target.platform == "hosted":
            short_id.naming_schema = "{platform}"

        short_id.set("platform", target.platform) # invalidate caches
        minimal_targets[short_id.string].append(target)


    # for key, values in minimal_targets.items():
    #     minimal_targets[key] = sorted(values, key=lambda d: d.string)[-1]
    print(len(minimal_targets.keys()))
    print(minimal_targets.values())
    print(minimal_targets)


get_targets()
exit(1)

parser = argparse.ArgumentParser()
parser.add_argument("--test", "-t", action='store_true', help="Test mode: generate only a few targets")
parser.add_argument("--jobs", "-j", type=int, default=2, help="Number of parallel doxygen processes")
parser.add_argument("--local-temp", "-l", action='store_true', help="Create temporary directory inside current working directory")
group = parser.add_mutually_exclusive_group(required=True)
group.add_argument("--compress", "-c", action='store_true', help="Compress output into gzip archive")
group.add_argument("--output", "-o", type=str, help="Output directory (absolute path)")
parser.add_argument("--overwrite", "-f", action='store_true', help="Overwrite existing data in output directory (Removes all files from output directory.)")
args = parser.parse_args()

device_list_hosted = [
 "hosted-darwin",
 "hosted-linux",
 "hosted-windows",
]

device_list_avr = [
 "at90can128",
# "at90pwm316",
# "at90pwm81",
# "at90usb1287",
 "atmega8",
 "atmega16",
 "atmega32",
 "atmega48",
 "atmega64",
 "atmega128a",
 "atmega2560",
 "attiny4",
 "attiny5",
 "attiny9",
 "attiny10",
# "attiny11",
# "attiny12",
 "attiny13",
# "attiny15",
# "attiny1634",
 "attiny20",
# "attiny2313a",
 "attiny24a",
 "attiny25",
# "attiny261a",
 "attiny40",
# "attiny4313",
# "attiny44",
 "attiny45",
# "attiny461a",
 "attiny48",
# "attiny828",
# "attiny84",
 "attiny85",
 "attiny861",
# "attiny87",
 "attiny88",
]

device_list_stm = ['stm32f030rct', 'stm32f031k6u', 'stm32f038k6u', 'stm32f042t6y', 'stm32f048t6y', 'stm32f051t8y', 'stm32f058t8y', 'stm32f070rbt', 'stm32f071vbt', 'stm32f072vbt', 'stm32f078vbt', 'stm32f091vct', 'stm32f098vct', 'stm32f100zet', 'stm32f101zgt', 'stm32f102rbt', 'stm32f103zgt', 'stm32f105vct', 'stm32f107vct', 'stm32f205zgt', 'stm32f207zgt', 'stm32f215zgt', 'stm32f217zgt', 'stm32f301r8t', 'stm32f302zet', 'stm32f303zet', 'stm32f318k8u', 'stm32f328c8t', 'stm32f334r8t', 'stm32f358vct', 'stm32f373vct', 'stm32f378vct', 'stm32f398vet', 'stm32f401vet', 'stm32f405zgt', 'stm32f407zgt', 'stm32f410tby', 'stm32f411vet', 'stm32f412zgt', 'stm32f413zht', 'stm32f415zgt', 'stm32f417zgt', 'stm32f423zht', 'stm32f427zit', 'stm32f429ziy', 'stm32f437zit', 'stm32f439ziy', 'stm32f446zet', 'stm32f469zit', 'stm32f479zit', 'stm32f722zet', 'stm32f723zet', 'stm32f730z8t', 'stm32f732zet', 'stm32f733zet', 'stm32f745zgt', 'stm32f746zgy', 'stm32f750z8t', 'stm32f756zgy', 'stm32f765zit', 'stm32f767zit', 'stm32f768aiy', 'stm32f769nih', 'stm32f777zit', 'stm32f778aiy', 'stm32f779nih', 'stm32l412tby', 'stm32l422tby', 'stm32l431vct', 'stm32l432kcu', 'stm32l433vct', 'stm32l442kcu', 'stm32l443vct', 'stm32l451vet', 'stm32l452vet', 'stm32l462vet', 'stm32l471zgt', 'stm32l475vgt', 'stm32l476zgt', 'stm32l485jey', 'stm32l486zgt', 'stm32l496zgt', 'stm32l4a6zgt', 'stm32l4r5ziy', 'stm32l4r7zit', 'stm32l4r9ziy', 'stm32l4s5ziy', 'stm32l4s7zit', 'stm32l4s9ziy']

if args.test:
    # test list
    device_list = ["hosted-linux", "at90can128", "stm32f103rbt", "stm32f429ziy"]
else:
    device_list = device_list_hosted + device_list_avr + device_list_stm


def main():
    cwd = Path().cwd()
    if args.local_temp:
        temp_dir = str(cwd)
    else:
        temp_dir = None
    with tempfile.TemporaryDirectory(dir=temp_dir) as tempdir:
        path_tempdir = Path(tempdir)
        print("Temporary directory: {}".format(str(tempdir)))
        os.system("git clone --recurse-submodules --jobs 8 --depth 1 https://github.com/modm-io/modm.git {}".format(Path(tempdir) / "modm"))
        output_dir = (path_tempdir / "output")
        (output_dir / "develop/api").mkdir(parents=True)
        os.chdir(tempdir)
        print("Starting to generate documentation...")
        print("... for {} devices, estimated memory footprint is {} MB".format(len(device_list), (len(device_list)*70)+2000))
        with multiprocessing.Pool(args.jobs) as pool:
                # We can only pass one argument to pool.map
                devices = ["{}|{}".format(path_tempdir, d) for d in device_list]
                results = pool.map(create_target, devices)
        # output_dir.rename(cwd / 'modm-api-docs')
        template_overview(output_dir)
        if args.compress:
            print("Zipping docs ...")
            shutil.make_archive(str(cwd / 'modm-api-docs'), 'gztar', str(output_dir))
        else:
            final_output_dir = Path(args.output)
            if args.overwrite:
                for i in final_output_dir.iterdir():
                    print('Removing {}'.format(i))
                    if i.is_dir():
                        shutil.rmtree(i)
                    else:
                        os.remove(i)
            print('Moving {} -> {}'.format(output_dir, final_output_dir))
            #shutil.move(str(output_dir) + '/', str(final_output_dir))
            output_dir.rename(final_output_dir)
        return results.count(False)


def create_target(argument):
        tempdir, device = argument.split("|")
        try:
            tempdir = Path(tempdir)
            print("Generating docs for {} ...".format(device))
            lbuild_options = '-r modm/repo.lb -D "modm:target={0}" -p {0}/'.format(device)
            options = ''
            if device.startswith("stm32"):
                options += ' -m"modm:architecture:**" -m"modm:build:**" -m"modm:cmsis:**" -m"modm:communication:**" -m"modm:container:**" -m"modm:debug:**" -m"modm:docs:**" -m"modm:driver:**" -m"modm:fatfs:**" -m"modm:freertos:**" -m"modm:io:**" -m"modm:math:**" -m"modm:platform:**" -m"modm:processing:**" -m"modm:ros:**" -m"modm:tlsf:**" -m"modm:ui:**" -m"modm:unittest:**" -m"modm:utils"'
            elif device.startswith("at"):
                lbuild_options += ' -D"modm:platform:clock:f_cpu=16000000"'
                options += ' -m"modm:**"'
            else:
                options += ' -m"modm:**"'
            lbuild_command = 'lbuild {} build {}'.format(lbuild_options, options)
            print('Executing: ' + lbuild_command)
            os.system(lbuild_command)
            os.system('(cd {}/modm/docs/ && doxygen doxyfile.cfg > /dev/null)'.format(device))
            (tempdir / device / "modm/docs/html").rename(tempdir / 'output/develop/api' / device)
            return True
        except:
            print("Error generating documentation for device {}".format(device))
            return False


html_template = """
<!doctype html>
<html>
<head>
    <title>doc.modm.io API</title>
    <meta charset="utf-8">

<style type="text/css">
body {
    font-family: "Ubuntu", "Helvetica Neue", Helvetica, Arial, sans-serif;
    text-align: center;
    padding: 0;
    margin: 0;
}
header {
    background-color: #3f51b5;
    color: #fff;
    padding: 0px;
}
table {
    margin: auto;
}
td {
    padding: 0em 0.5em;
}

*{
    box-sizing: border-box;
}
.autocomplete {
    position: relative;
    display: inline-block;
}
input, select {
    border: 1px solid transparent;
    background-color: #f1f1f1;
    padding: 10px;
    font-size: 16px;
    width: 100%;
    border-width: 2px;
    border-color: #fff;
}
input[type=text] {
    background-color: #f1f1f1;
    width: 100%;
    text-transform: uppercase;
}
input[type=text]::placeholder {
    text-transform: none;
}
input[type=submit] {
    background-color: #3f51b5;
    color: #fff;
}
.autocomplete-items {
    position: absolute;
    border: 1px solid #d4d4d4;
    border-bottom: none;
    border-top: none;
    z-index: 99;
    top: 100%;
    left: 0;
    right: 0;
    text-transform: uppercase;
}
.autocomplete-items div {
    padding: 10px;
    cursor: pointer;
    background-color: #fff;
    border-bottom: 1px solid #d4d4d4;
}
.autocomplete-items div:hover {
    background-color: #e9e9e9;
}
.autocomplete-active {
    background-color: DodgerBlue !important;
    color: #ffffff;
}
</style>
</head>
<body>
<header>
<a id="h1" href="https://modm.io/">
<img style="height: 10em;" src="https://modm.io/images/logo.svg">
</a>
<h1>API documentation</h1>
</header>
<main>
<p id="numtargets">Documentation is available for {{ num_devices }} devices.</p>
<h3>Select your target:</h3>
<form autocomplete="off" action="javascript:showDocumentation()">
<table>
    <tr>
        <td>Version/Release</td>
        <td>Microcontroller</td>
        <td>&nbsp;</td>
    </tr>
    <tr>
        <td>
            <select id="releaseinput" name="releases">
                <option>develop</option>
            </select>
        </td>
        <td>
            <div class="autocomplete" style="width:300px;">
                <input id="targetinput" type="text" name="target" placeholder="Type e.g. 'F407'">
            </div>
        </td>
        <td>
            <input type="submit" value="Show Documentation">
        </td>
    </tr>
</table>
</form>
<br />
<small>For most microcontrollers-sub-families only the variant with largest pin-count and memory is available.</small>
<p>Last updated: {{ date }}</p>

<script type="text/javascript">
var devices = [
{% for d in devices %}
"{{ d }}",
{% endfor %}
];
var targetinput = document.getElementById("targetinput");
var currentFocus;
function showDocumentation() {
    if(!targetinput.value) {
        targetinput.style.transition = "border 5ms ease-out";
        targetinput.style.borderColor = "red";
        setTimeout(function(){
            targetinput.style.transition = "border 5000ms ease-out";
            targetinput.style.borderColor = "#fff";
        }, 5000);
        return;
    }
    if(!releaseinput.value) {
        releaseinput.style.transition = "border 5ms ease-out";
        releaseinput.style.borderColor = "red";
        setTimeout(function(){
            releaseinput.style.transition = "border 5000ms ease-out";
            releaseinput.style.borderColor = "#fff";
        }, 5000);
        return;
    }
    var url = "/" + releaseinput.value + "/api/" + targetinput.value + "/";
    location.href = url;
}
targetinput.addEventListener("input", function(event) {
    var a, b, i, val = this.value;
    closeAllLists();
    if (!val) { return false;}
    currentFocus = -1;
    a = document.createElement("DIV");
    a.setAttribute("id", this.id + "autocomplete-list");
    a.setAttribute("class", "autocomplete-items");
    this.parentNode.appendChild(a);
    for (i = 0; i < devices.length; i++) {
        var regex = RegExp(val.toLowerCase());
        if (devices[i].toLowerCase().match(regex) != null) {
            b = document.createElement("DIV");
            b.innerHTML = devices[i].replace(regex, function(str) { return "<strong>" + str + "</strong>" })
            b.innerHTML += "<input type='hidden' value='" + devices[i] + "'>";
            b.addEventListener("click", function(event) {
                targetinput.value = this.getElementsByTagName("input")[0].value;
                closeAllLists();
            });
            a.appendChild(b);
        }
    }
});
targetinput.addEventListener("keydown", function(event) {
    var list = document.getElementById(this.id + "autocomplete-list");
    if (list) {
        list = list.getElementsByTagName("div");
    }
    if (event.keyCode == 40) { // down key
        currentFocus++;
        makeItemActive(list);
    } else if (event.keyCode == 38) { // up key
        currentFocus--;
        makeItemActive(list);
    } else if (event.keyCode == 13) { // enter key
        event.preventDefault();
        if (currentFocus > -1) {
            /*and simulate a click on the "active" item:*/
            if (list) {
                list[currentFocus].click();
            }
        }
    }
});
function removeActiveFromList(list) {
    for (var i = 0; i < list.length; i++) {
        list[i].classList.remove("autocomplete-active");
    }
}
function makeItemActive(list) {
    if (!list) return false;
    removeActiveFromList(list);
    if (currentFocus >= list.length) {
        currentFocus = 0;
    }
    if (currentFocus < 0) {
        currentFocus = (list.length - 1);
    }
    list[currentFocus].classList.add("autocomplete-active");
}
function closeAllLists(element) {
    var items = document.getElementsByClassName("autocomplete-items");
    for (var i = 0; i < items.length; i++) {
        if (element != items[i] && element != targetinput) {
            items[i].parentNode.removeChild(items[i]);
        }
    }
}
document.addEventListener("click", function (element) {
    closeAllLists(element.target);
});
</script>
<noscript>
<h3>Select your target</h3>
<input type="text" id="input" placeholder="Type e.g. 'F407'" />
<ul>
{% for d in devices %}
 <li id="{{ d }}"><a href="/develop/api/{{ d }}/">{{ d }}</a></li>
{% endfor %}
</ul>
</noscript>
</main>
</body>
</html>
"""

robots_txt = """
User-agent: *
Disallow: /
"""

def template_overview(output_dir):
    html = Environment().from_string(html_template).render(
        devices=device_list,
        date=datetime.datetime.now().strftime("%d.%m.%Y, %H:%M"),
        num_devices=len(device_list))
    with open(str(output_dir) + "/index.html","w+") as f:
        f.write(html)
    with open(str(output_dir) + "/robots.txt","w+") as f:
        f.write(robots_txt)


if __name__ == "__main__":
    exit(main())
