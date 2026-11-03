#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Copy the aggregate emulator without replacing a macOS signed inode."""
import pathlib
import shutil
import sys

destination = pathlib.Path(sys.argv[2])
destination.unlink(missing_ok=True)
shutil.copy2(sys.argv[1], destination)
