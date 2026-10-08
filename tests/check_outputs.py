#!/usr/bin/env python3
"""Checks that programs compute the right result under MPC.

Three suites are checked. "benchmarks" covers every benchmark in SPEC.
"ops" covers the operation tests in tests/ops/, one program per operation
kind, all with the function op(a, b, out, N) and metadata tests/ops/op.ll.json.
"scalar" covers tests/scalar/, where op(a, b) takes two secret int32_t values
and returns an int32_t. They use tests/scalar/op.ll.json, or <name>.ll.json
next to the test when it exists.

The MPC protocol is set up once per program, and setup dominates a GMW run.
A benchmark program runs all its trials. The ops and scalar tests are each
compiled through the pipeline on their own, then linked into one program per
suite and pipeline that runs all of them. If that program fails to link,
crashes or times out, each test is linked and run on its own instead.
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
import threading
from concurrent.futures import ThreadPoolExecutor
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
SCALAR = f'{HERE}/scalar'


def tests(suites):
    """name: (source, metadata, call, inputs, outputs, combined) for the
    chosen suites. Tests of a combined suite are linked into one program per
    pipeline (see run_combined); the others are a program each."""
    t = {}
    if 'benchmarks' in suites:
        for name, (call, ins, outs) in SPEC.items():
            t[name] = (f'{BENCH}/{name}.cpp', f'{BENCH}/metadata/{name}.ll.json',
                       call, ins, outs, False)
    if 'ops' in suites:
        for f in sorted(os.listdir(OPS)):
            if f.endswith('.cpp'):
                t['ops/' + f[:-4]] = (
                    f'{OPS}/{f}', f'{OPS}/op.ll.json', 'op(a, b, out, N);',
                    [('a', N), ('b', N), ('out', N)], [('out', N)], True)
    if 'scalar' in suites:
        for f in sorted(os.listdir(SCALAR)):
            if f.endswith('.cpp'):
                own = f'{SCALAR}/{f[:-4]}.ll.json'
                t['scalar/' + f[:-4]] = (
                    f'{SCALAR}/{f}',
                    own if os.path.exists(own) else f'{SCALAR}/op.ll.json',
                    'int32_t res = op(a, b);', [('&a', '1'), ('&b', '1')],
                    [('&res', '1')], True)
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


# Runs the trials named in argv[4] in one process, so the MPC protocol is set
# up once. argv: party port N trials [inputs dir], e.g. "1 14100 16 z1,r1".
# The native reference gets an inputs dir holding <trial>_inputs.txt.
MULTI_TRIAL_MAIN = r'''
int main(int argc, char **argv) {
  MPC::setup(atoi(argv[1]), atoi(argv[2]));
  char list[256];
  snprintf(list, sizeof list, "%s", argv[4]);
  for (char *t = list; *t;) {
    char *end = strchr(t, ',');
    if (end)
      *end = 0;
    char seed[16], mode[2] = {t[0], 0}, inputs[512];
    snprintf(seed, sizeof seed, "%s", t + 1);
    char *args[8] = {argv[0], argv[1], argv[2], argv[3], seed, mode, nullptr,
                     nullptr};
    int n = 6;
    if (argc > 5) {
      snprintf(inputs, sizeof inputs, "%s/%s_inputs.txt", argv[5], t);
      args[n++] = inputs;
    }
    smaug_trial_ = t;
    smaug_trial_main(n, args);
    if (!end)
      break;
    t = end + 1;
  }
  MPC::finish();
}
'''

# The driver of a combined program: runs the tests named in argv[5] (ids, as
# from test_id), each with the trials named in argv[4], after one setup.
# argv: party port N trials tests [inputs dir]. Files are named after
# "<id>.<trial>".
DRIVER = r'''#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mpc/mpc.h"

const char *smaug_trial_ = "";
%(decls)s
static struct {
  const char *name;
  void (*run)(int, char **);
} tests_[] = {
%(table)s
};

int main(int argc, char **argv) {
  MPC::setup(atoi(argv[1]), atoi(argv[2]));
  char names[8192];
  snprintf(names, sizeof names, "%%s", argv[5]);
  for (char *name = strtok(names, ","); name; name = strtok(nullptr, ",")) {
    void (*run)(int, char **) = nullptr;
    for (auto &t : tests_)
      if (!strcmp(t.name, name))
        run = t.run;
    if (!run) {
      fprintf(stderr, "unknown test %%s\n", name);
      return 2;
    }
    printf("running %%s\n", name);
    fflush(stdout);
    char list[256];
    snprintf(list, sizeof list, "%%s", argv[4]);
    for (char *t = list; *t;) {
      char *end = strchr(t, ',');
      if (end)
        *end = 0;
      char seed[16], mode[2] = {t[0], 0}, tag[320], inputs[1024];
      snprintf(seed, sizeof seed, "%%s", t + 1);
      snprintf(tag, sizeof tag, "%%s.%%s", name, t);
      char *args[8] = {argv[0], argv[1], argv[2], argv[3], seed, mode, nullptr,
                       nullptr};
      int n = 6;
      if (argc > 6) {
        snprintf(inputs, sizeof inputs, "%%s/%%s_inputs.txt", argv[6], tag);
        args[n++] = inputs;
      }
      smaug_trial_ = tag;
      run(n, args);
      if (!end)
        break;
      t = end + 1;
    }
  }
  MPC::finish();
}
'''


def test_id(name):
    return name.replace('/', '_')


def driver_source(ids, path):
    """Writes the driver of a combined program running the tests in ids."""
    decls = '\n'.join(f'void smaug_trial_{i}(int argc, char **argv);'
                      for i in ids)
    table = '\n'.join(f'    {{"{i}", smaug_trial_{i}}},' for i in ids)
    with open(path, 'w') as f:
        f.write(DRIVER % {'decls': decls, 'table': table})
    return path


def generate(name, test):
    """Writes the test program for one test and returns (source, metadata).
    A test of a combined suite becomes a library: its op is renamed
    smaug_op_<id>, with a metadata file to match, and its main becomes
    smaug_trial_<id>, run by the driver."""
    src, metadata, call, ins, outs, combined = test
    s = open(src).read()
    s = '#include <cstdio>\n#include <cstring>\n#include "harness.h"\n' + s
    # Seed per party so each party's inputs are reproducible.
    s, k = re.subn(r'srand\(\(unsigned\)time\(&t\)\);',
                   'srand(atoi(argv[4]) * 10 + atoi(argv[1]));', s)
    # The original main runs one trial. The protocol is set up once, in the
    # main added below, so its own setup and finish calls go.
    s, m = re.subn(r'int main\(int argc, char\s*\*\s*\*\s*argv\)',
                   'void smaug_trial_main(int argc, char **argv)', s)
    s, k2 = re.subn(r'MPC::setup\(atoi\(argv\[1\]\), atoi\(argv\[2\]\)\);',
                    '', s)
    s = s.replace('MPC::finish();', '')
    if k != 1 or m != 1 or k2 != 1 or s.count(call) != 1:
        sys.exit(f'{name}: main does not match the expected shape')
    # Trial argv: party port N seed mode [inputs file]. With an inputs file
    # the program is the native reference: it loads the combined inputs.
    pre = ['  {', '    int party_ = atoi(argv[1]);', '    if (argc > 6) {']
    pre += [f'      hload(argv[6], "{a}", {a}, {n});' for a, n in ins]
    pre += ['    } else {', "      if (party_ == 2 && argv[5][0] == 'z') {"]
    pre += [f'        memset({a}, 0, sizeof(int32_t) * ({n}));' for a, n in ins]
    pre += ['      }', '      char f_[64];',
            '      snprintf(f_, 64, "%s_in_p%d.txt", smaug_trial_, party_);']
    pre += [f'      hdump(f_, "{a}", {a}, {n});' for a, n in ins]
    pre += ['    }', '  }']
    post = ['  {', '    char f_[64];', '    if (argc > 6)',
            '      snprintf(f_, 64, "%s_out_ref.txt", smaug_trial_);', '    else',
            '      snprintf(f_, 64, "%s_out_p%d.txt", smaug_trial_, atoi(argv[1]));']
    post += [f'    hdump(f_, "{a}", {a}, {n});' for a, n in outs]
    # AND gates used so far; the comparison ignores names starting with #.
    post += ['    int gates_ = MPC::getNumGates();',
             '    hdump(f_, "#gates", &gates_, 1);', '  }']
    s = s.replace(call, '\n'.join(pre) + '\n  ' + call + '\n' + '\n'.join(post))
    os.makedirs(f'{BUILD}/src', exist_ok=True)
    tid = test_id(name)
    if combined:
        fn = f'smaug_op_{tid}'
        s = re.sub(r'\bop\(', fn + '(', s)
        s = s.replace('void smaug_trial_main(', f'void smaug_trial_{tid}(')
        s = s.replace('#include "harness.h"\n', '#include "harness.h"\n'
                      'extern const char *smaug_trial_;\n', 1)
        # The metadata of op, under the renamed function's mangled name.
        meta = json.load(open(metadata))
        renamed = {}
        for key, val in meta.items():
            if key.startswith('_Z2op'):
                renamed[f'_Z{len(fn)}{fn}' + key[len('_Z2op'):]] = val
        metadata = f'{BUILD}/src/{tid}.ll.json'
        with open(metadata, 'w') as f:
            json.dump(renamed, f, indent=1)
    else:
        s = s.replace('#include "harness.h"\n', '#include "harness.h"\n'
                      'static const char *smaug_trial_ = "";\n', 1)
        s += MULTI_TRIAL_MAIN
    path = f'{BUILD}/src/{tid}.cpp'
    open(path, 'w').write(s)
    return path, metadata


def run(cmd):
    return sp.run(cmd, stdout=sp.PIPE, stderr=sp.STDOUT, text=True)


def build(tc, name, src, metadata, pipe, lowering, link=True):
    """Returns (executable, None) or (None, (status, message)), where status
    is 'unsupported' for an mpc-lower diagnostic and 'build' otherwise.
    Without link, returns the transformed .ll instead of an executable."""
    gc, lib, stages = PIPELINES[pipe]
    d = f'{BUILD}/{lowering}/{pipe}'
    os.makedirs(d, exist_ok=True)
    stem = test_id(name)
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
    if link:
        steps += [link_command(tc, lib, [ll], exe)]
    for s in steps:
        r = run(s)
        if r.returncode:
            return None, build_error(s, r)
    return (exe if link else ll), None


def link_command(tc, lib, inputs, exe):
    return [tc['clang']] + tc['link'] + [
        f'-I{tc["prefix"]}/include', '-lssl', '-lcrypto', '-lemp-tool',
        f'-l{lib}', '-Wno-deprecated-declarations'] + inputs + [
        f'{BUILD}/harness.o', '-std=c++17', '-o', exe]


def build_error(step, r):
    """(status, message) for a build step that failed with result r."""
    lines = r.stdout.splitlines()
    unsupported = [l for l in lines if 'mpc-lower: error' in l]
    if unsupported:
        return ('unsupported', unsupported[0].strip()[:220])
    # Report the most informative line: a fatal error or assertion before a
    # generic error line, and never a backtrace frame.
    msg = None
    for key in ('LLVM ERROR', 'fatal error', 'Assertion', 'error:',
                'Segmentation'):
        hits = [l for l in lines if key in l and not l.lstrip().startswith('#')]
        if hits:
            msg = hits[0]
            break
    return ('build', f'{os.path.basename(step[0])} failed: '
            f'{(msg or r.stdout[-200:]).strip()[:220]}')


def build_native(tc, name, src, combined):
    """The native reference: an executable, or for a test of a combined
    suite an object file that native_combined links."""
    out = f'{BUILD}/native/{test_id(name)}' + ('.o' if combined else '')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if combined:
        cmd = [src, '-c']
    else:
        cmd = [src, f'{HERE}/harness/mpc_stub.cpp',
               f'{HERE}/harness/harness.cpp']
    r = run([tc['clang'], '-O0', '-std=c++17', f'-I{tc["prefix"]}/include',
             f'-I{HERE}/harness'] + tc['sysroot'] + cmd + ['-o', out])
    if r.returncode:
        sys.exit(f'native build of {name} failed:\n{r.stdout}')
    return out


def native_combined(tc, suite, names, objects):
    """The native reference of a combined suite: every test in it."""
    exe = f'{BUILD}/native/{suite}'
    driver = driver_source([test_id(n) for n in names], f'{exe}_driver.cpp')
    r = run([tc['clang'], '-O0', '-std=c++17', f'-I{tc["prefix"]}/include',
             f'-I{HERE}/harness'] + tc['sysroot'] + [
        driver] + objects + [f'{HERE}/harness/mpc_stub.cpp',
                             f'{HERE}/harness/harness.cpp', '-o', exe])
    if r.returncode:
        sys.exit(f'native build of {suite} failed:\n{r.stdout}')
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


def trials(exe, native, d, n, names, port, ids=None):
    """Runs the trials in names (e.g. ['z1', 'r1']) in one process pair in
    directory d and returns {tag: (status, detail, gates)}; status is pass,
    wrong, crash, timeout or missing. For a combined program, ids names its
    tests and a tag is "<id>.<trial>"; otherwise a tag is the trial."""
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(f'{d}/data')  # emp-aby stores pre-OT data here
    args = [str(n), ','.join(names)]
    if ids is not None:
        args.append(','.join(ids))
        names = [f'{i}.{t}' for i in ids for t in names]
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
        return {t: ('timeout', '', None) for t in names}
    if r1 or r2:
        return {t: ('crash', f'exit codes party1={r1} party2={r2}', None)
                for t in names}
    dumps = {}
    for t in names:
        i1, i2 = parse(f'{d}/{t}_in_p1.txt'), parse(f'{d}/{t}_in_p2.txt')
        o1, o2 = parse(f'{d}/{t}_out_p1.txt'), parse(f'{d}/{t}_out_p2.txt')
        if not (i1 and i2 and o1 and o2):
            continue
        dumps[t] = (o1, o2)
        with open(f'{d}/{t}_inputs.txt', 'w') as f:
            for k in i1:
                vals = ' '.join(str(a ^ b) for a, b in zip(i1[k], i2[k]))
                f.write(f'{k} {len(i1[k])} {vals}\n')
    sp.run([native, '1', '0'] + args + [d], cwd=d,
           stdout=sp.DEVNULL, stderr=sp.DEVNULL)
    out = {}
    for t in names:
        if t not in dumps:
            out[t] = ('missing', 'missing input or output dump', None)
            continue
        o1, o2 = dumps[t]
        gates = o1.get('#gates', [None])[0]
        ref = parse(f'{d}/{t}_out_ref.txt')
        if not ref:
            out[t] = ('missing', 'native reference produced no output', gates)
            continue
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
        out[t] = (('wrong' if errors else 'pass'), '; '.join(errors), gates)
    return out

def main():
    global BUILD
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--suites', default='benchmarks,ops,scalar',
                    help='comma-separated suites: benchmarks, ops, scalar')
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
    ap.add_argument('-j', '--jobs', type=int,
                    default=max(1, (os.cpu_count() or 2) // 2),
                    help='test/pipeline builds and runs to do at once '
                         '(default: half the CPUs; a GMW trial uses about '
                         'two cores per party)')
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

    results = []
    lock = threading.Lock()
    next_port = [a.port]

    def take_port():
        # Each trial gets its own range of ports; free_port skips ports that
        # other programs hold.
        with lock:
            port = free_port(next_port[0] + 1)
            next_port[0] = port + 4
            return port

    def record(lowering, name, pipe, trial_name, status, detail, gates):
        prefix = f'{lowering} ' if len(lowerings) > 1 else ''
        label = f'{prefix}{name} {pipe}' + (f' {trial_name}' if trial_name
                                            else '')
        with lock:
            results.append({'lowering': lowering, 'test': name,
                            'pipeline': pipe, 'trial': trial_name,
                            'status': status, 'detail': detail,
                            'gates': gates})
            if status == 'pass':
                print(f'PASS: {label}', flush=True)
            else:
                print(f'FAIL: {label}: {status}: {detail}', flush=True)

    trial_names = [f'{mode}{seed}' for mode in ('z', 'r')
                   for seed in a.seeds.split(',')]
    standalone = [n for n in names if not table[n][5]]
    suites = {}
    for n in names:
        if table[n][5]:
            suites.setdefault(n.split('/')[0], []).append(n)

    # The test program, its metadata and its native reference are made once
    # per test, by the first build that needs them.
    prepared, prep_locks = {}, {name: threading.Lock() for name in names}

    def prepare(name):
        with prep_locks[name]:
            if name not in prepared:
                src, meta = generate(name, table[name])
                prepared[name] = (src, meta,
                                  build_native(tc, name, src, table[name][5]))
            return prepared[name]

    # Phase 1: every build. A test of a combined suite builds to a .ll.
    built = {}

    def build_unit(name, lowering, pipe):
        src, meta, _ = prepare(name)
        path, err = build(tc, name, src, meta, pipe, lowering,
                          link=not table[name][5])
        if err:
            record(lowering, name, pipe, '', err[0], err[1], None)
        with lock:
            built[(name, lowering, pipe)] = path

    # Phase 2: the runs.
    def run_standalone(name, lowering, pipe):
        exe = built[(name, lowering, pipe)]
        if not exe:
            return
        d = f'{BUILD}/run/{lowering}/{pipe}/{test_id(name)}'
        res = trials(exe, prepare(name)[2], d, a.n, trial_names, take_port())
        for t in trial_names:
            record(lowering, name, pipe, t, *res[t])

    natives, native_lock = {}, threading.Lock()

    def native_suite(suite):
        with native_lock:
            if suite not in natives:
                ns = suites[suite]
                natives[suite] = native_combined(
                    tc, suite, ns, [prepare(n)[2] for n in ns])
            return natives[suite]

    def run_group(suite, lowering, pipe, group, tag):
        """Links the tests in group into one program and runs them. Returns
        {name: {trial: result}}, or None if linking failed or the program
        crashed or timed out, so that the caller can narrow it down."""
        lib = PIPELINES[pipe][1]
        d = f'{BUILD}/{lowering}/{pipe}'
        ids = [test_id(n) for n in group]
        exe = f'{d}/{suite}_{tag}'
        driver = driver_source(ids, f'{exe}_driver.cpp')
        r = run(link_command(tc, lib, [driver] + [
            built[(n, lowering, pipe)] for n in group], exe))
        if r.returncode:
            return None, build_error(link_command(tc, lib, [], exe), r)
        res = trials(exe, native_suite(suite),
                     f'{BUILD}/run/{lowering}/{pipe}/{suite}_{tag}', a.n,
                     trial_names, take_port(), ids)
        if any(v[0] in ('crash', 'timeout') for v in res.values()):
            return None, (next(v for v in res.values()
                               if v[0] in ('crash', 'timeout'))[:2])
        return {n: {t: res[f'{test_id(n)}.{t}'] for t in trial_names}
                for n in group}, None

    def run_combined(suite, lowering, pipe):
        group = [n for n in suites[suite] if built[(n, lowering, pipe)]]
        if not group:
            return
        out, _ = run_group(suite, lowering, pipe, group, 'combined')
        if out is None:
            # Run each test on its own to find the one that failed.
            out = {}
            for n in group:
                one, err = run_group(suite, lowering, pipe, [n],
                                     'single_' + test_id(n))
                if one is None:
                    for t in trial_names:
                        record(lowering, n, pipe, t, err[0], err[1], None)
                else:
                    out.update(one)
        for n, res in out.items():
            for t in trial_names:
                record(lowering, n, pipe, t, *res[t])

    with ThreadPoolExecutor(max_workers=max(1, a.jobs)) as pool:
        for f in [pool.submit(build_unit, n, l, p)
                  for n in names for l in lowerings for p in pipes]:
            f.result()  # re-raise errors from the workers
        runs = [pool.submit(run_standalone, n, l, p)
                for n in standalone for l in lowerings for p in pipes]
        runs += [pool.submit(run_combined, su, l, p)
                 for su in suites for l in lowerings for p in pipes]
        for f in runs:
            f.result()
    results.sort(key=lambda r: (r['lowering'], r['test'], r['pipeline'],
                                r['trial']))

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
