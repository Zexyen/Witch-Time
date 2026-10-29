#!/usr/bin/env python3
"""Developer and CI check of the link step on stand-in modules (testdata/fake_main: no game or sdk code): tables, lift,
compile and link as on a player's PC, then the program is loaded and run until it has mapped the stand-in main.

    python3 tools/wtbuild/smoke_link.py [--prebuilt <dir>] [--lib <file>]... [--wtbuild <path>] [--target windows]

--prebuilt: the runtime made by `wtbuild runtime` (as a release ships it), so only game.so is built here; without it
the link step builds the runtime too, against the system's libraries. --lib: a library a release keeps in build/
(libSDL3.so.0, SDL3.dll). Checks that every symbol resolves (`ldd -r`; only the game functions the runtime replaces,
which the stand-in does not have, may stay undefined) and that witch-time-game maps the image with the values game.so
holds. Needs lz4 and a clang with the AArch64 target and ld.lld (make_fake.py).

Windows (on Windows, or --target windows elsewhere): game.dll and witch-time-game.exe. A DLL cannot leave a symbol
undefined, so the game functions the runtime replaces are stand-ins too (compiled with `clang` from PATH), and the
program starting at all is the check that every import resolves. With --target windows on Linux the program runs under
Wine (--wine <program>, default wine); wtbuild finds a clang that targets MinGW as always (LLVM_ROOT).
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
FAKE = os.path.join(HERE, 'testdata', 'fake_main')
# lifter/recomp_eh.py HOST_OVERRIDES: game functions game_shim_eh.c calls the lifted originals of; not in a stand-in
GAME_ONLY = {'f_7100b382c0', 'f_7100d02de0', 'f_7100c1f600', 'f_71007e1fe4', 'f_7100b20a20', 'f_710067037c', 'f_71007077d0', 'f_710070a740', 'f_710070a760', 'f_7100d01380', 'f_7100cba90c', 'f_7100cba97c', 'f_7100c0ef04', 'f_71007a2894', 'f_7100d02d70', 'f_7100a6f5a0', 'f_7100d02d90', 'f_7100d04414'}
# nvn_sdk.c's sdk addresses the stand-in sdk has no counterpart of (any value links; the smoke run never calls them)
SDKA = ['nvnTextureBuilderGetRawStorageClass', 'nvnTextureBuilderGetSparseTileLayout', 'nvnTextureBuilderGetStorageAlignment',
        'nvnTextureBuilderGetStorageClass', 'nvnTextureBuilderGetStorageSize', 'nvnTextureGetRawStorageClass',
        'nvnTextureGetSparseTileLayout', 'nvnTextureGetStorageClass', 'nvnTextureGetStorageSize', 'chip_descriptor',
        'format_count', 'format_table', 'texture_layout', 'vt_hal_gm200', 'vt_nvgpu_device']


def run(cmd, **kw):
    p = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if p.returncode:
        sys.exit('%s failed (exit %d):\n%s%s' % (' '.join(cmd[:3]), p.returncode, p.stdout[-3000:], p.stderr[-3000:]))
    return p.stdout


def main():
    args = sys.argv[1:]
    windows = sys.platform == 'win32'
    exe = os.path.join(HERE, 'target', 'release', 'wtbuild.exe' if windows else 'wtbuild')
    prebuilt, libs, target, wine = None, [], [], []
    while args:
        a = args.pop(0)
        if a == '--prebuilt':
            prebuilt = os.path.abspath(args.pop(0))
        elif a == '--lib':
            libs.append(os.path.abspath(args.pop(0)))
        elif a == '--wtbuild':
            exe = os.path.abspath(args.pop(0))
        elif a == '--target':
            target = ['--target', args.pop(0)]
        elif a == '--wine':
            wine = [args.pop(0)]
        else:
            sys.exit(__doc__)
    if target == ['--target', 'windows'] and not windows:
        windows, wine = True, wine or ['wine']
    t = tempfile.mkdtemp(prefix='wtsmoke_')
    try:
        repo = os.path.join(t, 'repo')
        shutil.copytree(os.path.join(ROOT, 'runtime'), os.path.join(repo, 'runtime'), ignore=shutil.ignore_patterns('gen'))
        os.makedirs(os.path.join(repo, 'runtime', 'gen'))
        for d in ('lifter', 'targets/exefs', 'build', 'scripts'):          # scripts/: the compile step spells paths through it
            os.makedirs(os.path.join(repo, d))
        shutil.copy(os.path.join(ROOT, 'lifter', 'sdklift_install.txt'), os.path.join(repo, 'lifter'))
        shutil.copy(os.path.join(FAKE, 'roots.txt'), os.path.join(repo, 'lifter', 'sdklift_roots.txt'))
        mk = [sys.executable, os.path.join(FAKE, 'make_fake.py')]
        main_nso, sdk = os.path.join(t, 'main'), os.path.join(repo, 'targets', 'exefs', 'sdk')
        run(mk + [main_nso, '300', '--seed', '1', '--extra', os.path.join(FAKE, 'tables_main.c'), '--ld', '-init=nninitStartup'])
        run(mk + [sdk, '300', '--seed', '2', '--extra', os.path.join(FAKE, 'tables_sdk.c'), '--extra', os.path.join(FAKE, 'fake_nvn.s'),
                  '--extra', os.path.join(FAKE, 'fake_nvn_procs.s'), '--ld', '--no-relax'])
        main_elf = os.path.join(repo, 'targets', 'exefs', 'main.elf')
        run([exe, 'nso2elf', main_nso, main_elf])
        run([exe, 'tables', repo])
        addrs = os.path.join(repo, 'runtime', 'gen', 'nvn_sdk_addrs.h')
        have = open(addrs).read()
        with open(addrs, 'a') as f:
            f.writelines('#define SDKA_%s 0x10000ull  /* smoke_link.py */\n' % n for n in SDKA if '#define SDKA_%s ' % n not in have)
        run([exe, 'lift', main_elf, os.path.join(repo, 'build', 'gen_eh'), '--no-patches'])
        print(run([exe, 'compile', '--repo', repo] + target).strip().splitlines()[-1])
        if windows:
            standin = os.path.join(repo, 'build', 'standin.c')
            with open(standin, 'w') as f:
                f.write('typedef struct Ctx Ctx;\n' + ''.join('void %s(Ctx *c) { (void)c; }\n' % n for n in sorted(GAME_ONLY)))
            run(['clang', '--target=x86_64-w64-mingw32', '-c', standin, '-o', os.path.join(repo, 'build', 'obj', 'zz_standin.obj')])
        if prebuilt:
            shutil.copytree(prebuilt, os.path.join(repo, 'prebuilt'))
        for lib in libs:
            shutil.copy(lib, os.path.join(repo, 'build'))
        out = run([exe, 'link', '--repo', repo] + target)
        print(out.strip())
        if prebuilt and 'runtime: prebuilt' not in out:
            sys.exit('the prebuilt runtime was not used')
        build = os.path.join(repo, 'build')
        if not windows:
            p = subprocess.run(['ldd', '-r', os.path.join(build, 'witch-time-game')], capture_output=True, text=True)
            undef = sorted({l.split()[2] for l in (p.stdout + p.stderr).splitlines() if l.startswith('undefined symbol:')} - GAME_ONLY)
            missing = [l.split()[0] for l in p.stdout.splitlines() if '=> not found' in l]
            if undef or missing:
                sys.exit('unresolved: %s; libraries not found: %s' % (', '.join(undef) or '-', ', '.join(missing) or '-'))
            print('ldd -r: every symbol resolves (but the %d game functions a stand-in lacks)' % len(GAME_ONLY))
        os.makedirs(os.path.join(build, 'run'))
        env = dict(os.environ, SDL_VIDEO_DRIVER='offscreen', SDL_AUDIO_DRIVER='dummy', SSBU_ROMFS=os.path.join(t, 'romfs'))
        if wine:
            env.setdefault('WINEDEBUG', '-all')
        game = os.path.join(build, 'witch-time-game.exe' if windows else 'witch-time-game')
        try:
            p = subprocess.run(wine + [game, main_elf], cwd=os.path.join(build, 'run'), env=env,
                               capture_output=True, text=True, errors='replace', timeout=60)
            text = p.stdout + p.stderr
        except subprocess.TimeoutExpired as e:
            text = (e.stdout or b'').decode(errors='replace') + (e.stderr or b'').decode(errors='replace')
        mapped = [l for l in text.splitlines() if l.startswith(('host address table:', 'image mapped at', 'image protection:'))]
        if len(mapped) < 3:
            sys.exit('witch-time-game did not map the stand-in main:\n' + text[-3000:])
        print('\n'.join(mapped))
        print('smoke_link: ok')
        return 0
    finally:
        shutil.rmtree(t, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
