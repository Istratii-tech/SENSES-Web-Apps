#!/usr/bin/env python3
"""Prints the default BOOTCLASSPATH of the dalvikvm launcher.

    dexopt_bootclasspath.py dalvik/launcher/dalvikvm.cc

The build preoptimizes /system jars against the boot class path dalvikvm
runs with (mk/topic-dexopt.mk), so it reads the value from the launcher's
boot_class_path = "..." "..."; instead of keeping a second copy (jars an
image appends with ro.arcwasm.bootclasspath.append are not preoptimized).
"""
import re
import sys

source = open(sys.argv[1]).read()
match = re.search(r'boot_class_path\s*=\s*((?:"[^"]*"\s*)+);', source)
if not match:
    sys.exit('%s: no boot_class_path = "...";' % sys.argv[1])
print(''.join(re.findall(r'"([^"]*)"', match.group(1))))
