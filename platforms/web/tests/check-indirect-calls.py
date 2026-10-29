#!/usr/bin/env python3
"""Lists function-pointer casts that would trap when called in WebAssembly.

    platforms/web/tests/check-indirect-calls.py [build/web/compile_commands.json]

WebAssembly checks the signature of every indirect call. A function called through a pointer of a
different type traps ("function signature mismatch"), where native ABIs usually let it pass. What
counts is the lowered wasm signature: all pointers (and int, bool, enums, ...) are i32, so casting
int (*)(daFoo_c*) to int (*)(void*) is fine, but a different parameter count, a void vs. non-void
return, or int vs. float/double/64-bit is not.

This re-runs each game and aurora source in the web build's compile database syntax-only with
-Wcast-function-type-strict and reports the casts whose wasm signatures differ. Configure the build
with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON first. Casts laundered through void* are not visible here
and only show up at run time.
"""
import collections
import concurrent.futures
import json
import os
import re
import shlex
import subprocess
import sys

DB = sys.argv[1] if len(sys.argv) > 1 else 'build/web/compile_commands.json'
ROOT = os.path.realpath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
WARNING = re.compile(r"^(\S+?):(\d+):\d+: warning: cast from '([^']+)'(?: \(aka '([^']+)'\))? "
                     r"to '([^']+)'(?: \(aka '([^']+)'\))?")


def syntax_only(entry):
    args, skip = [], False
    for a in shlex.split(entry['command']):
        if skip:
            skip = False
        elif a == '-o':
            skip = True
        elif a != '-c':
            args.append(a)
    args += ['-fsyntax-only', '-Wcast-function-type-strict', '-Wno-error', '-fno-color-diagnostics']
    r = subprocess.run(args, cwd=entry['directory'], capture_output=True, text=True)
    return [l for l in r.stderr.splitlines() if 'Wcast-function-type' in l]


def split_params(s):
    depth, cur, out = 0, '', []
    for ch in s:
        depth += ch in '(<[' and 1 or 0
        depth -= ch in ')>]' and 1 or 0
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    return out + [cur.strip()] if cur.strip() else out


def lower(t):
    t = re.sub(r'\b(const|volatile|struct|class|enum|unsigned|signed)\b', ' ', t)
    t = re.sub(r'\s+', ' ', t).strip()
    if t in ('void', ''):
        return ''
    if '*' in t or '&' in t or '(' in t:
        return 'i'
    if t in ('float', 'f32'):
        return 'f'
    if t in ('double', 'f64', 'long double'):
        return 'd'
    if t in ('long long', 'long long int', 's64', 'u64', 'int64_t', 'uint64_t', 'OSTime'):
        return 'I'
    return 'i'


def wasm_signature(fn):
    m = re.match(r'^(.*?)\s*\((\w+::)?\*\)\((.*)\)( const)?(?: noexcept)?$', fn)
    if not m:
        return None
    params = [] if m.group(3).strip() in ('', 'void') else split_params(m.group(3))
    return lower(m.group(1)), ('i' if m.group(2) else '') + ''.join(
        'V' if p == '...' else lower(p) for p in params)


def main():
    entries = [e for e in json.load(open(DB))
               if any(os.path.realpath(e['file']).startswith(os.path.join(ROOT, d) + os.sep)
                      for d in ('src', 'libs', 'extern/aurora/lib'))]
    warnings = set()
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        for lines in pool.map(syntax_only, entries):
            warnings.update(lines)
    mismatches = collections.defaultdict(set)
    for line in warnings:
        m = WARNING.search(line)
        if not m:
            continue
        frm, to = m.group(4) or m.group(3), m.group(6) or m.group(5)
        a, b = wasm_signature(frm), wasm_signature(to)
        if a is None or b is None or a != b:
            mismatches[(frm, to)].add(f'{m.group(1)}:{m.group(2)}')
    for (frm, to), sites in sorted(mismatches.items()):
        print(f'{frm}  ->  {to}')
        for site in sorted(sites):
            print(f'    {site}')
    print(f'{sum(len(s) for s in mismatches.values())} cast(s) with a different wasm signature '
          f'({len(entries)} sources checked)')


if __name__ == '__main__':
    main()
