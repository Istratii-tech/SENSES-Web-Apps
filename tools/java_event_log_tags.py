#!/usr/bin/env python3
# Generates the Java class for an .logtags file, as AOSP 4.4.4's
# build/tools/java-event-log-tags.py does (Python 2 there; same output).
# Tags without a number ("?") get 999999, as for tags missing from the
# merged file in the AOSP build.
#
# Usage: java_event_log_tags.py INPUT.logtags OUTPUT.java
import os
import re
import sys

KEYWORDS = frozenset("""abstract continue for new switch assert default goto
package synchronized boolean do if private this break double implements
protected throw byte else import public throws case enum instanceof return
transient catch extends int short try char final interface static void class
finally long strictfp volatile const float native super while""".split())
JAVA_TYPES = ["ERROR", "int", "long", "String", "Object[]"]


def java_name(name):
    out = name[0].lower() + re.sub(r"[^A-Za-z0-9]", "", name.title())[1:]
    return out + "_" if out in KEYWORDS else out


def main(src, dst):
    options, tags = {}, []
    with open(src) as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line or line[0] == "#":
                continue
            parts = re.split(r"\s+", line, maxsplit=2)
            if parts[0] == "option":
                options[parts[1]] = parts[2:]
                continue
            num = 999999 if parts[0] == "?" else int(parts[0])
            desc = parts[2] if len(parts) == 3 else None
            if desc and not re.match(r"\(.*\)\s*$", desc):
                sys.exit("%s:%d: tag %s has unparseable description" % (src, n, parts[1]))
            tags.append((num, parts[1], desc))
    if "java_package" not in options:
        sys.exit("%s: java_package option not specified" % src)
    hide = options.get("javadoc_hide", ["true"])[0].lower() in ("true", "t", "1", "on", "yes", "y")
    cls = os.path.splitext(os.path.basename(src))[0]
    out = ["/* This file is auto-generated.  DO NOT MODIFY.\n * Source file: %s\n */\n\n" % src,
           "package %s;\n\n" % options["java_package"][0]]
    if hide:
        out.append("/**\n * @hide\n */\n")
    out.append("public class %s {\n  private %s() { }  // don't instantiate\n" % (cls, cls))
    for num, name, desc in tags:
        out.append("\n  /** %d %s%s */\n" % (num, name, " " + desc if desc else ""))
        out.append("  public static final int %s = %d;\n" % (name.upper(), num))
    for num, name, desc in tags:
        args = [a.strip("() ").split("|") for a in desc.split(",")] if desc else []
        decl = ", ".join(JAVA_TYPES[int(a[1])] + " " + java_name(a[0]) for a in args)
        call = "".join(", " + java_name(a[0]) for a in args)
        out.append("\n  public static void %s(%s) {" % (java_name("write_" + name), decl))
        out.append("\n    android.util.EventLog.writeEvent(%s%s);\n  }\n" % (name.upper(), call))
    out.append("}\n")
    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    with open(dst, "w") as f:
        f.write("".join(out))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: java_event_log_tags.py INPUT.logtags OUTPUT.java")
    main(sys.argv[1], sys.argv[2])
