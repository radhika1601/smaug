#!/usr/bin/env python3
"""Checks that programs compute the right result under MPC.

Two suites are checked. "benchmarks" covers every benchmark in SPEC.
"ops" covers the operation tests in tests/ops/, one program per operation
kind, all with the function op(a, b, out, N) and metadata tests/ops/op.ll.json.
For each program, a test program is generated from its source. Its main seeds rand() per party, records the inputs each
party passes to the benchmark function, and records the function's outputs.
The program is built through each pass pipeline and run as two parties. The
same source is also built natively, without the passes, and run on the
combined inputs. A trial passes when party 1's output equals the native
output, or when party 1 XOR party 2 does (outputs left as XOR shares).

Two input modes are run. In "z" party 2's inputs are zero, so the expected
result does not depend on how inputs are shared. In "r" both parties use
random inputs, and the native build gets party 1 XOR party 2.
"""
import argparse
import json
import os
import platform
import re
import shutil
import socket
import subprocess as sp
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BENCH = f'{ROOT}/benchmarks'
BUILD = f'{HERE}/build'

# name: (call statement in main, input arrays, output arrays). Arrays are
# (expression, length expression) using the variable names in main.
N, L, LEN, BINS = 'N', 'L', 'length', 'bins'
SPEC = {
    'biometric': ('int v1 = biometric(a, b, N*D, D);',
                  [('a', 'N*D'), ('b', 'D')], [('&v1', '1')]),
    'convex_hull': ('convex_hull(X, Y, N, resX1, resY1);',
                    [('X', N), ('Y', N), ('resX1', N), ('resY1', N)],
                    [('resX1', N), ('resY1', N)]),
    'count10': ('int32_t a = count10(X, N);', [('X', N)], [('&a', '1')]),
    'count102': ('int32_t a = count102(X, N);', [('X', N)], [('&a', '1')]),
    'db_variance': ('int res = db_variance(A, length);', [('A', LEN)],
                    [('&res', '1')]),
    'histogram': ('res1 = histogram(X, Y, N, res1, bins);',
                  [('X', N), ('Y', N)], [('res1', BINS)]),
    'inner_product': ('int res = inner_product(A, B, length);',
                      [('A', LEN), ('B', LEN)], [('&res', '1')]),
    'kmeans_iteration': (
        'kmeans(X, Y, clusterX, clusterY, resclusterX, resclusterY, L, N);',
        [('X', L), ('Y', L), ('clusterX', N), ('clusterY', N),
         ('resclusterX', N), ('resclusterY', N)],
        [('resclusterX', N), ('resclusterY', N)]),
    'longest102': ('int32_t a = longest102(X, N);', [('X', N)], [('&a', '1')]),
    'max_dist_between_syms': ('int32_t a = max_dist_between_syms(X, N, 0);',
                              [('X', N)], [('&a', '1')]),
    'minimal_points': ('minimal_points(X, Y, N, resX, resY);',
                       [('X', N), ('Y', N), ('resX', N), ('resY', N)],
                       [('resX', N), ('resY', N)]),
    'mnistRelu': ('mnist_relu(X, N, resX);', [('X', N), ('resX', N)],
                  [('resX', N)]),
    'psi': ('psi(X, Y, N, resX);', [('X', N), ('Y', N), ('resX', N)],
            [('resX', N)]),
}

# Same pipelines as the run targets in benchmarks/Makefile, plus the
# pipelines without mpc-loop-vectorize:
# name: (gc mode, runtime library, opt -passes stages).
PIPELINES = {
    'gc': ('true', 'gc-opt', ['smaug-pipeline']),
    'no-gc': ('false', 'mpc', ['smaug-pipeline']),
    'loop-flatten': ('true', 'gc-opt',
                     ['no-link-loop-flatten-pipeline', 'smaug-link']),
    'loop-flatten-no-gc': ('false', 'mpc',
                           ['no-link-loop-flatten-pipeline', 'smaug-link']),
    'gc-novec': ('true', 'gc-opt', ['smaug-pipeline-novec']),
    'no-gc-novec': ('false', 'mpc', ['smaug-pipeline-novec']),
}
DEFAULT_PIPELINES = 'gc,no-gc,loop-flatten,loop-flatten-no-gc'
# Lowerings selectable with --mpc-lowering (see passes/src/Options.cpp).
LOWERINGS = ('legacy', 'new')


OPS = f'{HERE}/ops'


def tests(suites):
    """name: (source, metadata, call, inputs, outputs) for the chosen suites."""
    t = {}
    if 'benchmarks' in suites:
        for name, (call, ins, outs) in SPEC.items():
            t[name] = (f'{BENCH}/{name}.cpp', f'{BENCH}/metadata/{name}.ll.json',
                       call, ins, outs)
    if 'ops' in suites:
        for f in sorted(os.listdir(OPS)):
            if f.endswith('.cpp'):
                t['ops/' + f[:-4]] = (
                    f'{OPS}/{f}', f'{OPS}/op.ll.json', 'op(a, b, out, N);',
                    [('a', N), ('b', N), ('out', N)], [('out', N)])
    return t


def out(cmd):
    return sp.check_output(cmd, text=True).strip()


def toolchain():
    darwin = platform.system() == 'Darwin'
    llvm = os.environ.get('LLVM_PREFIX')
    if not llvm:
        if darwin:
            llvm = ('/opt/homebrew/opt/llvm@18'
                    if os.path.isdir('/opt/homebrew/opt/llvm@18')
                    else '/usr/local/opt/llvm@18')
        else:
            llvm = '/usr/lib/llvm-18'
    prefix = os.environ.get('SMAUG_PREFIX') or (
        f'{ROOT}/.deps' if os.path.isdir(f'{ROOT}/.deps/lib') else '/usr/local')
    sysroot, link = [], [f'-L{prefix}/lib', f'-Wl,-rpath,{prefix}/lib']
    if darwin:
        sysroot = ['-isysroot', out(['xcrun', '--show-sdk-path'])]
        ssl = os.environ.get('OPENSSL_PREFIX') or out(
            ['brew', '--prefix', 'openssl@3'])
        link += sysroot + [f'-L{ssl}/lib']
    if platform.machine() == 'x86_64':
        link += ['-maes', '-mssse3']
    return {
        'clang': f'{llvm}/bin/clang++', 'opt': f'{llvm}/bin/opt',
        'prefix': prefix, 'sysroot': sysroot, 'link': link,
        'plugin': os.environ.get('PASS_PLUGIN',
                                 f'{ROOT}/passes/build/libpasses.so'),
    }


def generate(name, test):
    """Writes the test program for one test and returns its path."""
    src, _, call, ins, outs = test
    s = open(src).read()
    s = '#include <cstdio>\n#include <cstring>\n#include "harness.h"\n' + s
    # Seed per party so each party's inputs are reproducible.
    s, k = re.subn(r'srand\(\(unsigned\)time\(&t\)\);',
                   'srand(atoi(argv[4]) * 10 + atoi(argv[1]));', s)
    if k != 1 or s.count(call) != 1:
        sys.exit(f'{name}: main does not match the expected shape')
    # argv: party port N seed mode [inputs file]. With an inputs file the
    # program is the native reference: it loads the combined inputs.
    pre = ['  {', '    int party_ = atoi(argv[1]);', '    if (argc > 6) {']
    pre += [f'      hload(argv[6], "{a}", {a}, {n});' for a, n in ins]
    pre += ['    } else {', "      if (party_ == 2 && argv[5][0] == 'z') {"]
    pre += [f'        memset({a}, 0, sizeof(int32_t) * ({n}));' for a, n in ins]
    pre += ['      }', '      char f_[64];',
            '      snprintf(f_, 64, "in_p%d.txt", party_);']
    pre += [f'      hdump(f_, "{a}", {a}, {n});' for a, n in ins]
    pre += ['    }', '  }']
    post = ['  {', '    char f_[64];', '    if (argc > 6)',
            '      snprintf(f_, 64, "out_ref.txt");', '    else',
            '      snprintf(f_, 64, "out_p%d.txt", atoi(argv[1]));']
    post += [f'    hdump(f_, "{a}", {a}, {n});' for a, n in outs]
    # AND gates used so far; the comparison ignores names starting with #.
    post += ['    int gates_ = MPC::getNumGates();',
             '    hdump(f_, "#gates", &gates_, 1);', '  }']
    s = s.replace(call, '\n'.join(pre) + '\n  ' + call + '\n' + '\n'.join(post))
    os.makedirs(f'{BUILD}/src', exist_ok=True)
    path = f'{BUILD}/src/{name.replace("/", "_")}.cpp'
    open(path, 'w').write(s)
    return path


def run(cmd):
    return sp.run(cmd, stdout=sp.PIPE, stderr=sp.STDOUT, text=True)


def build(tc, name, src, metadata, pipe, lowering):
    """Returns (executable, None) or (None, (status, message)), where status
    is 'unsupported' for an mpc-lower diagnostic and 'build' otherwise."""
    gc, lib, stages = PIPELINES[pipe]
    d = f'{BUILD}/{lowering}/{pipe}'
    os.makedirs(d, exist_ok=True)
    stem = name.replace('/', '_')
    ll, exe = f'{d}/{stem}.ll', f'{d}/{stem}'
    plugin = ['--interleave-loops=false', f'-load-pass-plugin={tc["plugin"]}',
              f'--metadata-path={metadata}', f'--gc={gc}',
              f'--mpc-lowering={lowering}']
    steps = [[tc['clang'], f'-I{tc["prefix"]}/include', f'-I{HERE}/harness',
              '-O0', '-Xclang', '-disable-O0-optnone', '-S', '-emit-llvm',
              '-std=c++17'] + tc['sysroot'] + [src, '-o', ll],
             ['python3', f'{ROOT}/passes/remove_target_triple.py', ll]]
    steps += [[tc['opt']] + plugin + [f'-passes={p}', ll, '-o', ll, '-S']
              for p in stages]
    steps += [[tc['clang']] + tc['link'] + [
        '-lssl', '-lcrypto', '-lemp-tool', f'-l{lib}',
        '-Wno-deprecated-declarations', ll, f'{BUILD}/harness.o',
        '-std=c++17', '-o', exe]]
    for s in steps:
        r = run(s)
        if r.returncode:
            lines = r.stdout.splitlines()
            unsupported = [l for l in lines if 'mpc-lower: error' in l]
            if unsupported:
                return None, ('unsupported', unsupported[0].strip()[:220])
            # Report the most informative line: a fatal error or assertion
            # before a generic error line, and never a backtrace frame.
            msg = None
            for key in ('LLVM ERROR', 'fatal error', 'Assertion', 'error:',
                        'Segmentation'):
                hits = [l for l in lines if key in l and
                        not l.lstrip().startswith('#')]
                if hits:
                    msg = hits[0]
                    break
            return None, ('build', f'{os.path.basename(s[0])} failed: '
                          f'{(msg or r.stdout[-200:]).strip()[:220]}')
    return exe, None


def build_native(tc, name, src):
    exe = f'{BUILD}/native/{name.replace("/", "_")}'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    r = run([tc['clang'], '-O0', '-std=c++17', f'-I{tc["prefix"]}/include',
             f'-I{HERE}/harness'] + tc['sysroot'] + [
        src, f'{HERE}/harness/mpc_stub.cpp', f'{HERE}/harness/harness.cpp',
        '-o', exe])
    if r.returncode:
        sys.exit(f'native build of {name} failed:\n{r.stdout}')
    return exe


def parse(path):
    if not os.path.exists(path):
        return None
    vals = {}
    for line in open(path):
        t = line.split()
        vals[t[0]] = [int(x) for x in t[2:]]
    return vals


def free_port(port, span=4):
    """The first port at or after port with span free ports from it on.
    Concurrent runs may otherwise pick the same ports, and their parties
    then connect to each other."""
    while True:
        try:
            for p in range(port, port + span):
                with socket.socket() as s:
                    s.bind(('127.0.0.1', p))
            return port
        except OSError:
            port += span


def trial(exe, native, name, pipe, lowering, n, mode, seed, port):
    """Returns (status, detail, gates); status is pass, wrong, crash,
    timeout or missing."""
    d = f'{BUILD}/run/{lowering}/{pipe}/{name.replace("/", "_")}/{mode}{seed}'
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(f'{d}/data')  # emp-aby stores pre-OT data here
    args = [str(n), str(seed), mode]
    p1 = sp.Popen([exe, '1', str(port)] + args, cwd=d,
                  stdout=open(f'{d}/p1.log', 'w'), stderr=sp.STDOUT)
    time.sleep(0.3)
    try:
        r2 = sp.run([exe, '2', str(port)] + args, cwd=d,
                    stdout=open(f'{d}/p2.log', 'w'), stderr=sp.STDOUT,
                    timeout=600).returncode
        r1 = p1.wait(timeout=60)
    except sp.TimeoutExpired:
        p1.kill()
        return 'timeout', '', None
    if r1 or r2:
        return 'crash', f'exit codes party1={r1} party2={r2}', None
    i1, i2 = parse(f'{d}/in_p1.txt'), parse(f'{d}/in_p2.txt')
    o1, o2 = parse(f'{d}/out_p1.txt'), parse(f'{d}/out_p2.txt')
    if not (i1 and i2 and o1 and o2):
        return 'missing', 'missing input or output dump', None
    gates = o1.get('#gates', [None])[0]
    with open(f'{d}/inputs.txt', 'w') as f:
        for k in i1:
            vals = ' '.join(str(a ^ b) for a, b in zip(i1[k], i2[k]))
            f.write(f'{k} {len(i1[k])} {vals}\n')
    sp.run([native, '1', '0'] + args + [f'{d}/inputs.txt'], cwd=d,
           stdout=sp.DEVNULL, stderr=sp.DEVNULL)
    ref = parse(f'{d}/out_ref.txt')
    if not ref:
        return 'missing', 'native reference produced no output', gates
    errors = []
    for k, want in ref.items():
        if k.startswith('#'):
            continue
        a, b = o1[k], o2[k]
        shared = [x ^ y for x, y in zip(a, b)]
        if a == want or shared == want:
            continue
        bad = [i for i in range(len(want)) if a[i] != want[i]
               and shared[i] != want[i]]
        i = bad[0]
        errors.append(f'{k}: {len(bad)}/{len(want)} wrong, e.g. [{i}] '
                      f'expected {want[i]}, party1 {a[i]}, '
                      f'party1^party2 {shared[i]}')
    return ('wrong' if errors else 'pass'), '; '.join(errors), gates


def main():
    global BUILD
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--suites', default='benchmarks,ops',
                    help='comma-separated suites: benchmarks, ops')
    ap.add_argument('--tests', default='',
                    help='comma-separated test names, e.g. count10,ops/xor '
                         '(default: all tests in the suites)')
    ap.add_argument('--pipelines', default=DEFAULT_PIPELINES,
                    help='comma-separated pipelines: ' + ', '.join(PIPELINES))
    ap.add_argument('--lowering', default='legacy',
                    help='comma-separated lowerings: ' + ', '.join(LOWERINGS))
    ap.add_argument('--save-gates', default='',
                    help='write the gate count of each passing test and '
                         'pipeline (first trial) to this JSON file')
    ap.add_argument('-n', type=int, default=16, help='input size (argv[3])')
    ap.add_argument('--seeds', default='1,2', help='comma-separated seeds')
    ap.add_argument('--port', type=int, default=14100, help='first port')
    ap.add_argument('--build-dir', default=BUILD,
                    help='directory for build and run outputs')
    a = ap.parse_args()
    BUILD = os.path.abspath(a.build_dir)
    table = tests(a.suites.split(','))
    names = [x for x in a.tests.split(',') if x] or list(table)
    pipes = [x for x in a.pipelines.split(',') if x]
    lowerings = [x for x in a.lowering.split(',') if x]
    for x in names:
        if x not in table:
            sys.exit(f'unknown test {x}')
    for x in pipes:
        if x not in PIPELINES:
            sys.exit(f'unknown pipeline {x}')
    for x in lowerings:
        if x not in LOWERINGS:
            sys.exit(f'unknown lowering {x}')

    tc = toolchain()
    os.makedirs(BUILD, exist_ok=True)
    r = run([tc['clang'], '-O1', '-c'] + tc['sysroot'] +
            [f'{HERE}/harness/harness.cpp', '-o', f'{BUILD}/harness.o'])
    if r.returncode:
        sys.exit(r.stdout)

    port, results = a.port, []

    def record(lowering, name, pipe, trial_name, status, detail, gates):
        results.append({'lowering': lowering, 'test': name, 'pipeline': pipe,
                        'trial': trial_name, 'status': status,
                        'detail': detail, 'gates': gates})
        prefix = f'{lowering} ' if len(lowerings) > 1 else ''
        label = f'{prefix}{name} {pipe}' + (f' {trial_name}' if trial_name
                                            else '')
        if status == 'pass':
            print(f'PASS: {label}', flush=True)
        else:
            print(f'FAIL: {label}: {status}: {detail}', flush=True)

    for name in names:
        src = generate(name, table[name])
        native = build_native(tc, name, src)
        for lowering in lowerings:
            for pipe in pipes:
                exe, err = build(tc, name, src, table[name][1], pipe, lowering)
                if not exe:
                    record(lowering, name, pipe, '', err[0], err[1], None)
                    continue
                for mode in ('z', 'r'):
                    for seed in a.seeds.split(','):
                        port = free_port(port + 1)
                        status, detail, gates = trial(
                            exe, native, name, pipe, lowering, a.n, mode,
                            int(seed), port)
                        record(lowering, name, pipe, f'{mode}{seed}', status,
                               detail, gates)

    with open(f'{BUILD}/results.json', 'w') as f:
        json.dump(results, f, indent=1)
    if a.save_gates:
        gates = {}
        for r in results:
            if r['status'] == 'pass' and r['trial'] == f'z{a.seeds.split(",")[0]}':
                gates.setdefault(r['lowering'], {})[
                    f'{r["test"]} {r["pipeline"]}'] = r['gates']
        with open(a.save_gates, 'w') as f:
            json.dump(gates, f, indent=1, sort_keys=True)

    print()
    for lowering in lowerings:
        counts = {}
        for r in results:
            if r['lowering'] == lowering:
                counts[r['status']] = counts.get(r['status'], 0) + 1
        summary = ', '.join(f'{v} {k}' for k, v in sorted(counts.items()))
        print(f'Results ({lowering}): {summary}')
    sys.exit(0 if all(r['status'] == 'pass' for r in results) else 1)


if __name__ == '__main__':
    main()
