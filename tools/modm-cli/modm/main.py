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

import sys
import argparse

__version__ = '0.1.0'

def main():
    parser = argparse.ArgumentParser(description='modm command line interface')
    args = parser.parse_args(sys.argv[1:])

if __name__ == '__main__':
    main()
