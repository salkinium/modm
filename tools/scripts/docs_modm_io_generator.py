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

import argparse
import datetime
from jinja2 import Environment
import multiprocessing
import os
from pathlib import Path
import shutil
import tempfile
import zipfile

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
    device_list = ["at90pwm316"]
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
</head>
<body>
<header>
<h1><a id="h1" href="https://modm.io/">modm</a></h1>
<h2>API documentation</h2>
</header>
<h3>Select your target</h3>
<input type="text" id="input" placeholder="Type e.g. 'F407'" />
<ul>
{% for d in devices %}
 <li id="{{ d }}"><a href="develop/api/{{ d }}/">{{ d }}</a></li>
{% endfor %}
</ul>
<br />
<p>Last updated: {{ date }}</p>
<script type="text/javascript">
var debug = document.getElementById('debug');
var input = document.getElementById('input');
input.onkeyup = function () {
    var filter = RegExp(input.value.toLowerCase());
    var lis = document.getElementsByTagName('li');
    if(input.value.length > 0) {
        for (var i = 0; i < lis.length; i++) {
            var name = lis[i].id;
            if (name.match(filter) != null) {
                lis[i].style.display = 'list-item';
            }
            else {
                lis[i].style.display = 'none';
            }
        }
    }
    else {
        for (var i = 0; i < lis.length; i++) {
            lis[i].style.display = 'list-item';
        }
    }
}
</script>
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
        date=datetime.datetime.now().strftime("%d.%m.%Y, %H:%M"))
    with open(str(output_dir) + "/index.html","w+") as f:
        f.write(html)
    with open(str(output_dir) + "/robots.txt","w+") as f:
        f.write(robots_txt)


if __name__ == "__main__":
    exit(main())
