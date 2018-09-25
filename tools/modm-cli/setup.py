#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright (c) 2018, Niklas Hauser
#
# This file is part of the modm project.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

from setuptools import setup, find_packages
from modm.__init__ import __version__

setup(
    name = "modm-cli",
    version = __version__,
    python_requires=">=3.5.0",
    entry_points={
        "console_scripts": [
            "modm = modm.main:main",
        ],
    },

    packages = find_packages(exclude=["test"]),

    # include_package_data = True,

	# Make sure all files are unzipped during installation
	#zip_safe = False,

    install_requires = ["lbuild", "lxml", "colorful", "pyelftools", "pathlib"],

    extras_require = {
        "test": ["testfixtures", "coverage"],
    },

    # Metadata
    author = "Niklas Hauser",
    author_email = "niklas@salkinium.com",
    description = "modm command line interface.",
    license = "MPLv2",
    keywords = "modm cli",
    url = "https://github.com/modm-io/modm-cli",
    classifiers = [
        "Development Status :: 1 - Planning",
        "Environment :: Console",
        "Intended Audience :: Developers",
        "License :: OSI Approved :: Mozilla Public License 2.0 (MPL 2.0)",
        "Natural Language :: English",
        "Operating System :: OS Independent",
        "Programming Language :: Python",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.5",
        "Programming Language :: Python :: 3.6",
        "Topic :: Software Development",
        "Topic :: Software Development :: Embedded Systems",
    ],
)
