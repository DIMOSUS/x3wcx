"""Tests of the x3wcx plugin against the Python reference (x3kc catx.py / unpck.py).

Both builds are driven through build/wcxdrv32.exe (x3wcx.wcx) and build/wcxdrv64.exe (x3wcx.wcx64), which call the
exports the way Total Commander does. Game catalogs are only read (list, extract); every change is made on copies in
a temporary folder.

    python tests/test_wcx.py [--game DIR] [--x3kc DIR] [--quick] [--keep]
        --quick   skip the two big catalogs (01.cat 1.3 GB, 02.cat 2.1 GB) and pack only part of the .pck files
"""
import argparse
import glob
import gzip
import hashlib
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, 'build')
DRIVERS = [('x86', os.path.join(BUILD, 'wcxdrv32.exe'), os.path.join(BUILD, 'x3wcx.wcx')),
           ('x64', os.path.join(BUILD, 'wcxdrv64.exe'), os.path.join(BUILD, 'x3wcx.wcx64'))]
XOR33 = bytes(i ^ 0x33 for i in range(256))

E_NOT_SUPPORTED, E_EABORTED, E_TOO_MANY_FILES = 24, 21, 23
CAPS = 1 | 2 | 4 | 8 | 64

failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print('FAIL', what)
    return cond


class Drv:
    def __init__(self, arch, exe, dll):
        self.arch, self.exe, self.dll = arch, exe, dll

    def run(self, *args, abort_after=None, ok=(0,)):
        cmd = [self.exe, self.dll]
        if abort_after is not None:
            cmd += ['--abort-after', str(abort_after)]
        cmd += [str(a) for a in args]
        p = subprocess.run(cmd, capture_output=True)
        if ok is not None and p.returncode not in ok:
            raise RuntimeError('%s %s -> %d\n%s' % (self.arch, ' '.join(map(str, args)), p.returncode,
                                                     p.stderr.decode('utf-8', 'replace')))
        return p

    def list(self, arc):
        out = []
        for line in self.run('list', arc).stdout.decode('utf-8').splitlines():
            name, packed, unp, attr = line.split('\t')
            out.append((name, int(packed), int(unp)))
        return out

    def hashes(self, arc, tmp):
        out = []
        for line in self.run('hashes', arc, tmp).stdout.decode('utf-8').splitlines():
            h, name = line.split('\t', 1)
            out.append((name, h))
        return out


def cat_text(cat):
    b = open(cat, 'rb').read()
    return bytes((c ^ ((0xDB + i) & 0xFF)) for i, c in enumerate(b)).decode('latin1')


def member_md5s(cat, catx):
    out = []
    with open(os.path.splitext(cat)[0] + '.dat', 'rb') as f:
        for p, off, size in catx.read_index(cat):
            f.seek(off)
            h = hashlib.md5()
            left = size
            while left:
                b = f.read(min(left, 1 << 22))
                h.update(b.translate(XOR33))
                left -= len(b)
            out.append((p.replace('/', '\\'), h.hexdigest()))
    return out


def members(cat, catx):
    return [(p, catx.read_member(cat, p)) for p, _, _ in catx.read_index(cat)]


def expect_catalog(cat, expected, first, tmp, catx, what, whole=False):
    """cat/dat equal what catx.write_catalog makes of expected [(path, bytes)]; first line kept as `first`."""
    ref = os.path.join(tmp, 'expected', os.path.basename(cat))
    if os.path.isdir(os.path.dirname(ref)):
        shutil.rmtree(os.path.dirname(ref))
    catx.write_catalog(ref, expected)
    dat, rdat = os.path.splitext(cat)[0] + '.dat', os.path.splitext(ref)[0] + '.dat'
    ok = check(open(dat, 'rb').read() == open(rdat, 'rb').read(), what + ': .dat bytes')
    if whole:
        ok &= check(open(cat, 'rb').read() == open(ref, 'rb').read(), what + ': .cat bytes')
    else:
        got, exp = cat_text(cat).split('\n'), cat_text(ref).split('\n')
        ok &= check(got[0] == first, what + ': first line kept (%r)' % got[0])
        ok &= check(got[1:] == exp[1:], what + ': .cat lines')
    leftovers = [f for f in os.listdir(os.path.dirname(cat)) if 'x3wcx-' in f]
    ok &= check(not leftovers, what + ': no temp files left %s' % leftovers)
    return ok


def file_id(p):
    return os.stat(p).st_ino


# ---------------------------------------------------------------- tests

def test_basics(d, game, tmp):
    check(int(d.run('caps').stdout) == CAPS, d.arch + ' caps')
    for f, want in [(os.path.join(game, 'addon', '04.cat'), '1 1'), (os.path.join(game, '13.cat'), '1 1'),
                    (glob.glob(os.path.join(game, 'addon', 'scripts', '*.pck'))[0], '1 1'),
                    (os.path.join(game, 'addon', '04.dat'), '0 0'), (os.path.join(game, 'addon', 'x3files.xml'), '0 0')]:
        if os.path.exists(f):
            got = d.run('canhandle', f).stdout.decode().strip()
            check(got == want, '%s canhandle %s: %s' % (d.arch, os.path.relpath(f, game), got))


def test_read_catalogs(d, game, tmp, catx, quick):
    cats = sorted(glob.glob(os.path.join(game, '[0-9][0-9].cat'))) + sorted(glob.glob(os.path.join(game, 'addon', '[0-9][0-9].cat')))
    for cat in cats:
        rel = os.path.relpath(cat, game)
        big = os.path.getsize(os.path.splitext(cat)[0] + '.dat') > 500 << 20
        ref = [(p.replace('/', '\\'), s, s) for p, _, s in catx.read_index(cat)]
        check(d.list(cat) == ref, '%s list %s' % (d.arch, rel))
        if quick and big:
            continue
        t0 = time.time()
        got = d.hashes(cat, tmp)
        check(got == member_md5s(cat, catx), '%s contents %s' % (d.arch, rel))
        print('  %s %s: %d members, %.1fs' % (d.arch, rel, len(got), time.time() - t0))
    cat = os.path.join(game, 'addon', '04.cat')
    lines = d.run('lista', cat).stdout.decode('utf-8').splitlines()
    check([tuple(l.split('\t')) for l in lines] == [(p.replace('/', '\\'), str(s), str(s)) for p, _, s in catx.read_index(cat)],
          d.arch + ' ANSI ReadHeader list')
    d.run('test', cat)


def test_pck_read(d, game, tmp, unpck, catx):
    pcks = sorted(glob.glob(os.path.join(game, 'addon', 'scripts', '*.pck')))
    for p in pcks:
        stem = os.path.splitext(os.path.basename(p))[0]
        data = unpck.unpack(open(p, 'rb').read())
        if not check(d.list(p) == [(stem + '.xml', os.path.getsize(p), len(data))], '%s pck list %s' % (d.arch, stem)):
            break
    # every 25th: contents through ProcessFile
    for p in pcks[::25]:
        data = unpck.unpack(open(p, 'rb').read())
        check(d.hashes(p, tmp) == [(os.path.splitext(os.path.basename(p))[0] + '.xml', hashlib.md5(data).hexdigest())],
              '%s pck contents %s' % (d.arch, os.path.basename(p)))
    # extension by folder, then by content
    src = os.path.join(game, 'addon', '04.cat')
    if not os.path.exists(src):
        return
    for p, _, _ in catx.read_index(src):
        if not p.lower().endswith('.pck'):
            continue
        folder = p.split('/')[0].lower()
        data = unpck.unpack(catx.read_member(src, p))
        for where in (folder, 'unknown'):
            dst = os.path.join(tmp, 'pckext', where, os.path.basename(p))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, 'wb').write(catx.read_member(src, p))
            if where in ('scripts', 't', 'director', 'maps'):
                ext = '.xml'
            elif where == 'types':
                ext = '.txt'
            else:
                ext = '.xml' if data.lstrip(b'\xef\xbb\xbf \t\r\n')[:1] in (b'<', b'') else '.txt'
            got = d.list(dst)
            check(got[0][0] == os.path.splitext(os.path.basename(p))[0] + ext and got[0][2] == len(data),
                  '%s pck name %s in %s: %s' % (d.arch, p, where, got[0][0]))


def test_pck_write(d, game, tmp, unpck, quick):
    pcks = sorted(glob.glob(os.path.join(game, 'addon', 'scripts', '*.pck')))
    if quick or d.arch == 'x86':
        pcks = pcks[::10]
    work = os.path.join(tmp, 'pckw', 'scripts')
    os.makedirs(work, exist_ok=True)
    total_in = total_out = total_game = 0
    for p in pcks:
        name = os.path.basename(p)
        arc = os.path.join(work, name)
        shutil.copyfile(p, arc)
        text = unpck.unpack(open(p, 'rb').read())
        ex = os.path.join(tmp, 'pckx')
        os.makedirs(ex, exist_ok=True)
        d.run('extract', arc, ex)
        member = os.path.splitext(name)[0] + '.xml'
        check(open(os.path.join(ex, member), 'rb').read() == text, '%s pck extract %s' % (d.arch, name))
        d.run('pack', arc, ex, '-', 2, member)
        new = open(arc, 'rb').read()
        ok = check(new[:2] == b'\x1f\x8b', '%s pck stays unmasked %s' % (d.arch, name))
        ok = ok and check(gzip.decompress(new) == text and unpck.unpack(new) == text, '%s pck roundtrip %s' % (d.arch, name))
        if not ok:
            break
        total_in += len(text)
        total_out += len(new)
        total_game += os.path.getsize(p)
        os.remove(os.path.join(ex, member))
    print('  %s pck: %d files, %d -> %d bytes (game %d)' % (d.arch, len(pcks), total_in, total_out, total_game))

    # masked: key byte kept, contents roundtrip
    text = unpck.unpack(open(pcks[0], 'rb').read())
    gz = gzip.compress(text)
    key0 = 0x5A
    k = key0 ^ 0xC8
    arc = os.path.join(work, 'masked.pck')
    open(arc, 'wb').write(bytes([key0]) + bytes(b ^ k for b in gz))
    check(d.run('canhandle', arc).stdout.decode().strip() == '1 1', d.arch + ' canhandle masked pck')
    check(d.list(arc)[0][2] == len(text), d.arch + ' masked pck list')
    # by content (Ctrl+PgDn): a game-like gzip under any name, but not a gzip.exe file that stores its name
    import io
    named = io.BytesIO()
    with gzip.GzipFile(filename='notes.txt', mode='wb', fileobj=named) as g:
        g.write(text)
    for fname, data, want in [('plain.bin', gz, '1 1'), ('notes.txt.gz', named.getvalue(), '0 0'), ('named.pck', named.getvalue(), '1 1')]:
        p = os.path.join(work, fname)
        open(p, 'wb').write(data)
        check(d.run('canhandle', p).stdout.decode().strip() == want, '%s canhandle %s' % (d.arch, fname))
    src = os.path.join(tmp, 'pcksrc')
    os.makedirs(src, exist_ok=True)
    edited = text + b'\n<!-- edited -->\n'
    open(os.path.join(src, 'masked.xml'), 'wb').write(edited)
    d.run('pack', arc, src, '-', 2, 'masked.xml')
    new = open(arc, 'rb').read()
    check(new[0] == key0 and unpck.unpack(new) == edited, d.arch + ' masked pck repack keeps key')
    try:
        gzip.decompress(new)
        check(False, d.arch + ' masked pck must not be plain gzip')
    except OSError:
        pass

    # a new .pck (as the game's: unmasked) and deflate edge cases
    rnd = random.Random(7)
    cases = {'empty': b'', 'one': b'x', 'zeros': bytes(300000), 'random': bytes(rnd.getrandbits(8) for _ in range(200000)),
             'mixed': b''.join(rnd.choice([b'<a>', b'text ', bytes([rnd.getrandbits(8)]), b'\x00' * 40]) for _ in range(60000)),
             'periodic': b'abc' * 100000 + b'abcd' * 1000}
    for name, data in cases.items():
        open(os.path.join(src, name + '.xml'), 'wb').write(data)
        arc = os.path.join(work, 'new_' + name + '.pck')
        if os.path.exists(arc):
            os.remove(arc)
        d.run('pack', arc, src, '-', 2, name + '.xml')
        new = open(arc, 'rb').read()
        check(new[:2] == b'\x1f\x8b' and gzip.decompress(new) == data, '%s new pck %s (%d -> %d)' % (d.arch, name, len(data), len(new)))
        check(d.hashes(arc, tmp)[0][1] == hashlib.md5(data).hexdigest(), '%s new pck read back %s' % (d.arch, name))
    p = d.run('pack', os.path.join(work, 'two.pck'), src, '-', 2, 'one.xml', 'zeros.xml', ok=None)
    check(p.returncode == E_TOO_MANY_FILES, d.arch + ' pck with two files refused')
    p = d.run('delete', os.path.join(work, 'new_one.pck'), 'new_one.xml', ok=None)
    check(p.returncode == E_NOT_SUPPORTED, d.arch + ' pck delete refused')


def test_catalog_write(d, game, tmp, catx):
    src_cats = [c for c in (os.path.join(game, 'addon', '04.cat'), os.path.join(game, '03.cat')) if os.path.exists(c)]
    files = os.path.join(tmp, 'files')
    os.makedirs(os.path.join(files, 'scripts'), exist_ok=True)
    os.makedirs(os.path.join(files, 'sub', 'deep'), exist_ok=True)
    rnd = random.Random(1)
    new = {'scripts/plugin.test.one.xml': b'<script>one</script>\n',
           'sub/deep/blob.bin': bytes(rnd.getrandbits(8) for _ in range(3 << 20)),
           'readme with spaces.txt': b'hello world\r\n'}
    for p, b in new.items():
        open(os.path.join(files, *p.split('/')), 'wb').write(b)
    for orig in src_cats:
        rel = os.path.relpath(orig, game).replace(os.sep, '_')
        work = os.path.join(tmp, 'cat_' + d.arch + '_' + rel[:-4])
        if os.path.isdir(work):
            shutil.rmtree(work)
        os.makedirs(work)
        cat = os.path.join(work, '07.cat')  # another number than the original: the first line must stay as it was
        shutil.copyfile(orig, cat)
        shutil.copyfile(os.path.splitext(orig)[0] + '.dat', os.path.join(work, '07.dat'))
        first = cat_text(cat).split('\n')[0]
        exp = members(cat, catx)
        dat = os.path.join(work, '07.dat')
        what = '%s %s' % (d.arch, rel)

        # add: one with SubPath, one with saved paths, one plain; the .dat is appended in place
        ino = file_id(dat)
        d.run('pack', cat, files, 'scripts', 0, 'scripts\\plugin.test.one.xml')
        d.run('pack', cat, files, '-', 2, 'sub\\deep\\blob.bin', 'readme with spaces.txt')
        exp += [('scripts/plugin.test.one.xml', new['scripts/plugin.test.one.xml']), ('sub/deep/blob.bin', new['sub/deep/blob.bin']),
                ('readme with spaces.txt', new['readme with spaces.txt'])]
        expect_catalog(cat, exp, first, tmp, catx, what + ' add')
        check(file_id(dat) == ino, what + ' add appends to the same .dat')

        # replace a member in the middle, path given in another case: stays in its place with its own name
        k = len(exp) // 3
        victim = exp[k][0]
        repl = os.path.join(tmp, 'repl_' + d.arch + '_' + rel[:-4])
        alt = os.path.join(repl, *victim.upper().split('/'))
        os.makedirs(os.path.dirname(alt), exist_ok=True)
        data = b'replaced ' + victim.encode('latin1') * 50
        open(alt, 'wb').write(data)
        d.run('pack', cat, repl, '-', 2, victim.upper().replace('/', '\\'))
        exp[k] = (victim, data)
        expect_catalog(cat, exp, first, tmp, catx, what + ' replace')

        # delete: one member in the middle, plus a folder the way TC asks for it (dir\*.*)
        k = len(exp) // 2
        gone = exp[k][0]
        d.run('delete', cat, gone.replace('/', '\\'), 'sub\\*.*')
        exp = [m for m in exp if m[0] != gone and not m[0].lower().startswith('sub/')]
        expect_catalog(cat, exp, first, tmp, catx, what + ' delete')

        # delete the last member: index rewritten, .dat cut in place
        ino = file_id(dat)
        d.run('delete', cat, exp[-1][0].replace('/', '\\'))
        exp = exp[:-1]
        expect_catalog(cat, exp, first, tmp, catx, what + ' delete tail')
        check(file_id(dat) == ino, what + ' tail delete keeps the .dat')

        # abort through ProcessDataProc: nothing changes, nothing is left behind
        before = (open(cat, 'rb').read(), open(dat, 'rb').read())
        p = d.run('pack', cat, files, '-', 2, 'sub\\deep\\blob.bin', abort_after=1, ok=None)
        check(p.returncode == E_EABORTED, what + ' abort append returns E_EABORTED (%d)' % p.returncode)
        check((open(cat, 'rb').read(), open(dat, 'rb').read()) == before, what + ' abort append leaves the catalog')
        p = d.run('delete', cat, exp[0][0], abort_after=1, ok=None)
        check(p.returncode == E_EABORTED, what + ' abort rewrite returns E_EABORTED (%d)' % p.returncode)
        check((open(cat, 'rb').read(), open(dat, 'rb').read()) == before, what + ' abort rewrite leaves the catalog')
        expect_catalog(cat, exp, first, tmp, catx, what + ' after aborts')

        # ANSI PackFiles, then move mode (sources deleted)
        d.run('packa', cat, files, '-', 2, 'readme with spaces.txt')
        exp.append(('readme with spaces.txt', new['readme with spaces.txt']))
        mv = os.path.join(files, 'moved.txt')
        open(mv, 'wb').write(b'moved')
        d.run('pack', cat, files, '-', 3, 'moved.txt')
        exp.append(('moved.txt', b'moved'))
        check(not os.path.exists(mv), what + ' PK_PACK_MOVE_FILES deletes the source')
        expect_catalog(cat, exp, first, tmp, catx, what + ' ansi + move')

        # what the plugin extracts now is what catx reads
        check(d.hashes(cat, tmp) == member_md5s(cat, catx), what + ' contents after changes')

        # delete everything
        d.run('delete', cat, '*.*')
        expect_catalog(cat, [], first, tmp, catx, what + ' delete all')

    # a new catalog: exactly what catx.write_catalog writes
    work = os.path.join(tmp, 'newcat_' + d.arch)
    if os.path.isdir(work):
        shutil.rmtree(work)
    os.makedirs(work)
    cat = os.path.join(work, '06.cat')
    d.run('pack', cat, files, '-', 2, 'scripts\\plugin.test.one.xml', 'sub\\deep\\blob.bin')
    expect_catalog(cat, [('scripts/plugin.test.one.xml', new['scripts/plugin.test.one.xml']), ('sub/deep/blob.bin', new['sub/deep/blob.bin'])],
                   '06.dat', tmp, catx, d.arch + ' new catalog', whole=True)
    lone = os.path.join(work, '08.dat')
    open(lone, 'wb').write(b'not ours')
    p = d.run('pack', os.path.join(work, '08.cat'), files, '-', 2, 'readme with spaces.txt', ok=None)
    check(p.returncode != 0 and open(lone, 'rb').read() == b'not ours', d.arch + ' lone .dat is not overwritten')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=r'D:\GAMES\X3 Terran Conflict')
    ap.add_argument('--x3kc', default=r'D:\GAMES\x3kc')
    ap.add_argument('--quick', action='store_true')
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--arch', choices=['x86', 'x64'])
    a = ap.parse_args()
    sys.path.insert(0, a.x3kc)
    from x3kc import catx, unpck
    tmp = tempfile.mkdtemp(prefix='x3wcx-test-')
    print('temp', tmp)
    try:
        for arch, exe, dll in DRIVERS:
            if a.arch and arch != a.arch:
                continue
            d = Drv(arch, exe, dll)
            t0 = time.time()
            test_basics(d, a.game, tmp)
            test_read_catalogs(d, a.game, tmp, catx, a.quick)
            test_pck_read(d, a.game, tmp, unpck, catx)
            test_pck_write(d, a.game, tmp, unpck, a.quick)
            test_catalog_write(d, a.game, tmp, catx)
            print('%s done in %.0fs' % (arch, time.time() - t0))
    finally:
        if not a.keep:
            shutil.rmtree(tmp, ignore_errors=True)
    print('FAILED %d' % len(failures) if failures else 'all passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
