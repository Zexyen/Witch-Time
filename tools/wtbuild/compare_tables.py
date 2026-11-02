#!/usr/bin/env python3
"""Developer check: `wtbuild tables` writes exactly the files runtime/gen/make_gen.py (with lifter/sdklift.py) writes,
on stand-in main and sdk modules (testdata/fake_main: no game or sdk code) or on your own dump.

    python3 tools/wtbuild/compare_tables.py                         stand-ins built with clang for AArch64
    python3 tools/wtbuild/compare_tables.py --exefs <dir>           your dump's exefs (needs main.elf and sdk; stays on your PC)

Each side runs in its own copy of the tree layout, so runtime/gen of this checkout is not touched. The stand-ins use a
roots file of their own (testdata/fake_main/roots.txt); your dump uses lifter/sdklift_roots.txt. Needs Pillow and lz4
(make_gen.py's own needs); the stand-ins also need clang and ld.lld with the AArch64 target.
"""
import filecmp
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
FAKE = os.path.join(HERE, 'testdata', 'fake_main')


def copy(rel, dst_root, src=None):
    dst = os.path.join(dst_root, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src or os.path.join(ROOT, rel), dst)


def main():
    args = sys.argv[1:]
    exe = os.path.join(HERE, 'target', 'release', 'wtbuild')
    exefs = None
    while args:
        a = args.pop(0)
        if a == '--exefs':
            exefs = os.path.abspath(args.pop(0))
        elif a == '--wtbuild':
            exe = os.path.abspath(args.pop(0))
    t = tempfile.mkdtemp(prefix='wttab_')
    try:
        if exefs is None:
            mk = [sys.executable, os.path.join(FAKE, 'make_fake.py')]
            main_nso, sdk_nso = os.path.join(t, 'main'), os.path.join(t, 'sdk')
            subprocess.run(mk + [main_nso, '300', '--seed', '1', '--extra', os.path.join(FAKE, 'tables_main.c'),
                                 '--ld', '-init=nninitStartup'], check=True)
            subprocess.run(mk + [sdk_nso, '300', '--seed', '2', '--extra', os.path.join(FAKE, 'tables_sdk.c'),
                                 '--extra', os.path.join(FAKE, 'fake_nvn.s'), '--extra', os.path.join(FAKE, 'fake_nvn_procs.s'), '--ld', '--no-relax'], check=True)
            roots = os.path.join(FAKE, 'roots.txt')
            main_elf = os.path.join(t, 'main.elf')
            subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'extract', 'nso2elf.py'), main_nso, main_elf],
                           check=True, stdout=subprocess.DEVNULL)
        else:
            main_elf, sdk_nso = os.path.join(exefs, 'main.elf'), os.path.join(exefs, 'sdk')
            roots = os.path.join(ROOT, 'lifter', 'sdklift_roots.txt')
        py, rs = os.path.join(t, 'py'), os.path.join(t, 'rs')
        for side in (py, rs):
            copy('targets/exefs/main.elf', side, main_elf)
            copy('targets/exefs/sdk', side, sdk_nso)
            copy('lifter/sdklift_install.txt', side)
            copy('lifter/sdklift_roots.txt', side, roots)
        for rel in ('runtime/gen/make_gen.py', 'tools/extract/nso2elf.py', 'lifter/recomp.py',
                    'lifter/regcache.py', 'lifter/sdklift.py', 'scripts/nvn_procs.py'):
            copy(rel, py)
        for n in os.listdir(os.path.join(ROOT, 'runtime', 'gen_static')):
            copy('runtime/gen_static/' + n, rs)
        os.makedirs(os.path.join(rs, 'runtime', 'gen'))
        env = {k: v for k, v in os.environ.items() if not k.startswith(('RECOMP_', 'RECOMPS_'))}
        a = subprocess.run([sys.executable, os.path.join(py, 'runtime', 'gen', 'make_gen.py')], env=env, capture_output=True, text=True)
        b = subprocess.run([exe, 'tables', rs], capture_output=True, text=True)
        print('make_gen.py:\n' + a.stdout + a.stderr + '\nwtbuild tables:\n' + b.stdout + b.stderr)
        if a.returncode or b.returncode:
            sys.exit('a side failed')
        ok = True
        pa, pb = os.path.join(py, 'runtime', 'gen'), os.path.join(rs, 'runtime', 'gen')
        na = sorted(n for n in os.listdir(pa) if n != 'make_gen.py')
        nb = sorted(os.listdir(pb))
        if na != nb:
            ok = False
            print('file lists differ: only make_gen.py: %s; only wtbuild: %s' % (sorted(set(na) - set(nb)), sorted(set(nb) - set(na))))
        diff = [n for n in na if n in nb and not filecmp.cmp(os.path.join(pa, n), os.path.join(pb, n), shallow=False)]
        for rel in ('targets/exefs/sdk.elf',):
            if not filecmp.cmp(os.path.join(py, rel), os.path.join(rs, rel), shallow=False):
                diff.append(rel)
        print('tables: %d files, %d differ%s' % (len(na) + 1, len(diff), (': ' + ', '.join(diff)) if diff else ''))
        for n in diff[:3]:
            if n.endswith(('.c', '.h', '.txt')):
                subprocess.run(['diff', os.path.join(pa, n), os.path.join(pb, n)])
        return 0 if ok and not diff else 1
    finally:
        shutil.rmtree(t, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
