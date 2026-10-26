"""Tests of the x3wcx plugin against the Python reference (x3kc catx.py / unpck.py).

Both builds are driven through build/wcxdrv32.exe (x3wcx.wcx) and build/wcxdrv64.exe (x3wcx.wcx64), which call the
exports the way Total Commander does. Game catalogs are only read (list, extract); every change is made on copies in
a temporary folder. Without --game the tests run on small synthetic catalogs made with catx.write_catalog.

    python tests/test_wcx.py [--game DIR] [--x3kc DIR] [--quick] [--keep] [--arch x86|x64]
        --game    X3 TC/AP folder (env X3_GAME); omitted: synthetic data
        --x3kc    checkout of https://github.com/DIMOSUS/x3kc (env X3KC), unless the package is installed
        --quick   skip catalogs whose .dat is over 500 MB (the game's 01 and 02) and pack only part of the .pck files
"""
import argparse
import collections
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

E_NOT_SUPPORTED, E_EABORTED, E_TOO_MANY_FILES, E_ECREATE = 24, 21, 23, 16
CAPS = 1 | 2 | 4 | 8 | 16 | 64 | 128

failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print('FAIL', what)
    return cond


class Drv:
    def __init__(self, arch, exe, dll, ini=None):
        self.arch, self.exe, self.dll, self.ini = arch, exe, dll, ini

    def with_ini(self, ini):
        return Drv(self.arch, self.exe, self.dll, ini)

    def run(self, *args, abort_after=None, ok=(0,)):
        cmd = [self.exe, self.dll]
        if abort_after is not None:
            cmd += ['--abort-after', str(abort_after)]
        if self.ini:
            cmd += ['--ini', self.ini]
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
    # extension of the member by content (DDS textures in dds/, a plain list in director/, XML in types/ ...);
    # samples of every top folder of every catalog, each also under an unrelated folder (as in TC's temp copy)
    cats = sorted(glob.glob(os.path.join(game, '[0-9][0-9].cat'))) + sorted(glob.glob(os.path.join(game, 'addon', '[0-9][0-9].cat')))
    per_top = collections.Counter()
    kinds = collections.Counter()
    for cat in cats:
        for p, _, _ in catx.read_index(cat):
            if not p.lower().endswith('.pck'):
                continue
            top = p.split('/')[0].lower()
            special = os.path.basename(p).lower() in ('mdfiles.pck', 'hq.pck')
            if per_top[top] >= 4 and not special:
                continue
            per_top[top] += 1
            raw = catx.read_member(cat, p)
            data = unpck.unpack(raw)
            folder = p.split('/')[-2].lower() if '/' in p else ''
            for where in (folder or 'root', 'unknown'):
                dst = os.path.join(tmp, 'pckext', where, os.path.basename(p))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                open(dst, 'wb').write(raw)
                ext = expected_pck_ext(data, where)
                kinds[ext] += 1
                got = d.list(dst)
                check(got[0][0] == os.path.splitext(os.path.basename(p))[0] + ext and got[0][2] == len(data),
                      '%s pck name %s in %s: %s, want %s' % (d.arch, p, where, got[0][0], ext))
    print('  %s pck member names: %s' % (d.arch, dict(kinds)))


def expected_pck_ext(data, folder):
    """The member extension the plugin should give a .pck holding data (an independent spelling of its rule)."""
    for sig, ext in ((b'DDS ', '.dds'), (b'\x89PNG\r\n\x1a\n', '.png'), (b'\xff\xd8\xff', '.jpg'), (b'BOB1', '.bob'),
                     (b'CUT1', '.bob'), (b'OggS', '.ogg')):
        if data.startswith(sig):
            return ext
    if data[:4] == b'RIFF' and data[8:12] == b'WAVE':
        return '.wav'
    text = (data[3:] if data.startswith(b'\xef\xbb\xbf') else data).lstrip(b' \t\r\n')
    if not text:
        return '.txt' if folder == 'types' else '.xml'
    if text[:1] == b'<':
        return '.xml'
    return '.bin' if b'\x00' in data[:4096] else '.txt'


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
    # several gzip members back to back: read whole, as unpck does, and kept whole on repack
    multi = os.path.join(work, 'multi.pck')
    parts = [b'<a>first</a>\n', b'<b>second</b>\n', b'third' * 1000]
    open(multi, 'wb').write(b''.join(gzip.compress(x) for x in parts))
    whole = b''.join(parts)
    check(unpck.unpack(open(multi, 'rb').read()) == whole, 'reference reads multi-member gzip')
    check(d.list(multi)[0][2] == len(whole), d.arch + ' multi-member pck size')
    check(d.hashes(multi, tmp)[0][1] == hashlib.md5(whole).hexdigest(), d.arch + ' multi-member pck contents')
    ex = os.path.join(tmp, 'pckm_' + d.arch)
    os.makedirs(ex, exist_ok=True)
    d.run('extract', multi, ex)
    d.run('pack', multi, ex, '-', 2, 'multi.xml')
    check(unpck.unpack(open(multi, 'rb').read()) == whole, d.arch + ' multi-member pck repack keeps all')

    # an existing .pck takes back only its own member (any case, any extension), nothing dropped in by mistake
    own = os.path.join(work, 'own.pck')
    open(own, 'wb').write(gzip.compress(b'<own/>'))
    before = open(own, 'rb').read()
    open(os.path.join(src, 'other.txt'), 'wb').write(b'not xml at all')
    p = d.run('pack', own, src, '-', 2, 'other.txt', ok=None)
    check(p.returncode == E_NOT_SUPPORTED and open(own, 'rb').read() == before, d.arch + ' pck refuses a foreign file (%d)' % p.returncode)
    open(os.path.join(src, 'OWN.TXT'), 'wb').write(b'<own>2</own>')
    d.run('pack', own, src, '-', 2, 'OWN.TXT')
    check(unpck.unpack(open(own, 'rb').read()) == b'<own>2</own>', d.arch + ' pck takes its own name in another case/extension')

    # a member name outside latin1 is fine for a .pck (no path is stored), only catalogs need latin1
    cyr = os.path.join(work, 'скрипт.pck')
    open(cyr, 'wb').write(gzip.compress(b'<x/>'))
    open(os.path.join(src, 'скрипт.xml'), 'wb').write(b'<x>cyr</x>')
    d.run('pack', cyr, src, '-', 2, 'скрипт.xml')
    check(unpck.unpack(open(cyr, 'rb').read()) == b'<x>cyr</x>', d.arch + ' pck with a cyrillic name repacks')

    p = d.run('pack', os.path.join(work, 'two.pck'), src, '-', 2, 'one.xml', 'zeros.xml', ok=None)
    check(p.returncode == E_TOO_MANY_FILES, d.arch + ' pck with two files refused')
    p = d.run('delete', os.path.join(work, 'new_one.pck'), 'new_one.xml', ok=None)
    check(p.returncode == E_NOT_SUPPORTED, d.arch + ' pck delete refused')


def test_models(d, game, tmp, unpck, catx):
    """.pbd/.pbb (gzip-packed .bod/.bob models): member named .bod/.bob, contents as unpck reads them, repack."""
    found = {'pbd': [], 'pbb': []}
    for cat in sorted(glob.glob(os.path.join(game, 'addon', '[0-9][0-9].cat'))) + sorted(glob.glob(os.path.join(game, '[0-9][0-9].cat'))):
        for p, _, size in catx.read_index(cat):
            ext = p.lower().rsplit('.', 1)[-1]
            if ext in found and len(found[ext]) < 3:
                found[ext].append((cat, p))
        if all(len(v) == 3 for v in found.values()):
            break
    check(found['pbd'] and found['pbb'], d.arch + ' models found in catalogs')
    work = os.path.join(tmp, 'models_' + d.arch)
    os.makedirs(work, exist_ok=True)
    for ext, member_ext in (('pbd', '.bod'), ('pbb', '.bob')):
        for cat, p in found[ext]:
            raw = catx.read_member(cat, p)
            arc = os.path.join(work, os.path.basename(p))
            open(arc, 'wb').write(raw)
            data = unpck.unpack(raw)
            stem = os.path.splitext(os.path.basename(p))[0]
            what = '%s %s' % (d.arch, p)
            check(d.run('canhandle', arc).stdout.decode().strip() == '1 1', what + ' canhandle')
            check(d.list(arc) == [(stem + member_ext, len(raw), len(data))], what + ' list %s' % d.list(arc))
            check(d.hashes(arc, tmp) == [(stem + member_ext, hashlib.md5(data).hexdigest())], what + ' contents')
            # F4-style edit: extract, change, pack back under the member's name
            ex = os.path.join(work, 'x')
            os.makedirs(ex, exist_ok=True)
            d.run('extract', arc, ex)
            edited = data + (b'\n// edited\n' if ext == 'pbd' else b'\x00edited')
            open(os.path.join(ex, stem + member_ext), 'wb').write(edited)
            d.run('pack', arc, ex, '-', 2, stem + member_ext)
            new = open(arc, 'rb').read()
            check(new[:2] == b'\x1f\x8b' and unpck.unpack(new) == edited, what + ' repack')
            os.remove(os.path.join(ex, stem + member_ext))
    # a new model archive from a .bod
    src = os.path.join(work, 'src')
    os.makedirs(src, exist_ok=True)
    open(os.path.join(src, 'new model.bod'), 'wb').write(b'// new\n1;2;3;\n')
    arc = os.path.join(work, 'new model.pbd')
    if os.path.exists(arc):
        os.remove(arc)
    d.run('pack', arc, src, '-', 2, 'new model.bod')
    check(gzip.decompress(open(arc, 'rb').read()) == b'// new\n1;2;3;\n' and d.list(arc)[0][0] == 'new model.bod',
          d.arch + ' new .pbd')


def write_ini(path, pack_on_add):
    with open(path, 'w') as f:
        f.write('[other]\nx=1\n' if pack_on_add is None else '[x3wcx]\nPackOnAdd=%d\n' % pack_on_add)
    return path


def test_pack_on_add(d, game, tmp, catx, unpck):
    """Adding to a catalog packs the types the game looks up packed first: .xml/.txt/.dds -> .pck, .bod -> .pbd,
    .bob -> .pbb; other files and files the catalog already holds unpacked go in as they are."""
    orig = os.path.join(game, 'addon', '04.cat')
    work = os.path.join(tmp, 'packadd_' + d.arch)
    if os.path.isdir(work):
        shutil.rmtree(work)
    files = os.path.join(work, 'files')
    os.makedirs(os.path.join(files, 'x'))
    cat = os.path.join(work, '08.cat')
    rnd = random.Random(5)
    src = {'x/a.xml': b'<?xml version="1.0"?>\n<a>' + b'<b/>' * 2000 + b'</a>\n',
           'x/B.TXT': b'22;0;\n' * 3000,
           'x/tex.dds': b'DDS |\x00\x00\x00' + bytes(rnd.getrandbits(8) for _ in range(5000)) + bytes(20000),
           'x/model.bod': b'// model\n' + b'1; 2; 3; -1;\n' * 2000,
           'x/model2.bob': b'BOB1INFO' + bytes(rnd.getrandbits(8) for _ in range(3000)),
           'x/data.bin': bytes(rnd.getrandbits(8) for _ in range(4000)),
           'x/already.pck': gzip.compress(b'<already/>')}
    for p, b in src.items():
        open(os.path.join(files, *p.split('/')), 'wb').write(b)
    packed = {'x/a.xml': 'x/a.pck', 'x/B.TXT': 'x/B.pck', 'x/tex.dds': 'x/tex.pck', 'x/model.bod': 'x/model.pbd',
              'x/model2.bob': 'x/model2.pbb'}

    def contents():
        return [(p, catx.read_member(cat, p)) for p, _, _ in catx.read_index(cat)]

    for ini in (None, write_ini(os.path.join(work, 'on.ini'), 1), write_ini(os.path.join(work, 'nokey.ini'), None)):
        dd = d.with_ini(ini)
        what = '%s pack on add (%s)' % (d.arch, os.path.basename(ini) if ini else 'no ini')
        shutil.copyfile(orig, cat)
        shutil.copyfile(os.path.splitext(orig)[0] + '.dat', os.path.join(work, '08.dat'))
        before = contents()
        dd.run('pack', cat, files, '-', 2, *[p.replace('/', '\\') for p in src])
        got = contents()
        ok = check(got[:len(before)] == before, what + ': old members untouched')
        names = [p for p, _ in got[len(before):]]
        ok &= check(names == [packed.get(p, p) for p in src], what + ': names %s' % names)
        for (p, b), (q, stored) in zip(src.items(), got[len(before):]):
            if p in packed:
                ok &= check(stored[:2] == b'\x1f\x8b' and unpck.unpack(stored) == b, what + ': %s packed as %s' % (p, q))
            else:
                ok &= check(stored == b, what + ': %s stored as it is' % p)
        ok &= check(d.hashes(cat, tmp) == member_md5s(cat, catx), what + ': plugin reads it back')
        if not ok:
            return

    # replacing: an .xml replaces the catalog's .pck of that name in its place, packed
    pcks = [(i, p) for i, (p, _, _) in enumerate(catx.read_index(cat)) if p.lower().endswith('.pck')]
    i, target = pcks[len(pcks) // 2]
    xml = os.path.join(files, *(os.path.splitext(target)[0] + '.xml').split('/'))
    os.makedirs(os.path.dirname(xml), exist_ok=True)
    open(xml, 'wb').write(b'<replaced/>\n')
    n = len(catx.read_index(cat))
    d.run('pack', cat, files, '-', 2, os.path.relpath(xml, files))
    idx = catx.read_index(cat)
    check(len(idx) == n and idx[i][0] == target and unpck.unpack(catx.read_member(cat, target)) == b'<replaced/>\n',
          d.arch + ' .xml replaces the .pck in place')

    # a file the catalog already holds unpacked stays unpacked
    raw = d.with_ini(write_ini(os.path.join(work, 'off.ini'), 0))
    open(os.path.join(files, 'x', 'loose.xml'), 'wb').write(b'<v1/>')
    raw.run('pack', cat, files, '-', 2, 'x\\loose.xml')
    check(catx.read_index(cat)[-1][0] == 'x/loose.xml' and catx.read_member(cat, 'x/loose.xml') == b'<v1/>',
          d.arch + ' PackOnAdd=0 stores as it is')
    open(os.path.join(files, 'x', 'loose.xml'), 'wb').write(b'<v2/>')
    n = len(catx.read_index(cat))
    d.run('pack', cat, files, '-', 2, 'x\\loose.xml')
    idx = catx.read_index(cat)
    check(len(idx) == n and idx[-1][0] == 'x/loose.xml' and catx.read_member(cat, 'x/loose.xml') == b'<v2/>',
          d.arch + ' an unpacked member is replaced unpacked')
    check(int(d.run('caps').stdout) == CAPS, d.arch + ' caps with options')


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
    # catalog paths are latin1: a cyrillic name is refused and nothing changes
    open(os.path.join(files, 'файл.txt'), 'wb').write(b'x')
    before = open(cat, 'rb').read()
    p = d.run('pack', cat, files, '-', 2, 'файл.txt', ok=None)
    check(p.returncode == 12 and open(cat, 'rb').read() == before, d.arch + ' non-latin1 path refused (%d)' % p.returncode)

    # a catalog with members but no .dat: refused, and no empty .dat left behind
    shutil.copyfile(cat, os.path.join(work, '09.cat'))
    p = d.run('pack', os.path.join(work, '09.cat'), files, '-', 2, 'readme with spaces.txt', ok=None)
    check(p.returncode != 0 and not os.path.exists(os.path.join(work, '09.dat')), d.arch + ' no .dat: refused without creating one')

    # a *.x3wcx-bak left by a cut-short swap blocks every write until it is sorted out
    for bak in ('06.dat.x3wcx-bak', '06.cat.x3wcx-bak'):
        open(os.path.join(work, bak), 'wb').write(b'old copy')
        before = (open(cat, 'rb').read(), open(os.path.join(work, '06.dat'), 'rb').read())
        for args in (('pack', cat, files, '-', 2, 'readme with spaces.txt'), ('delete', cat, 'sub\\deep\\blob.bin'),
                     ('pack', cat, files, '-', 2, 'scripts\\plugin.test.one.xml')):
            p = d.run(*args, ok=None)
            check(p.returncode == E_ECREATE, '%s %s refused while %s exists (%d)' % (d.arch, args[0], bak, p.returncode))
        check((open(cat, 'rb').read(), open(os.path.join(work, '06.dat'), 'rb').read()) == before, d.arch + ' catalog untouched with ' + bak)
        check(open(os.path.join(work, bak), 'rb').read() == b'old copy', d.arch + ' backup kept: ' + bak)
        os.remove(os.path.join(work, bak))

    lone = os.path.join(work, '08.dat')
    open(lone, 'wb').write(b'not ours')
    p = d.run('pack', os.path.join(work, '08.cat'), files, '-', 2, 'readme with spaces.txt', ok=None)
    check(p.returncode != 0 and open(lone, 'rb').read() == b'not ours', d.arch + ' lone .dat is not overwritten')


def make_synthetic_game(root, catx):
    """A small stand-in for the game folder, laid out like X3 TC/AP: catalogs with .pck members in the folders the
    plugin knows (and in others), paths with spaces, first lines that do not name their .dat, addon/scripts/*.pck."""
    rnd = random.Random(42)

    def xml(i):
        return ('<?xml version="1.0" encoding="UTF-8"?>\n<script><name>s%d</name>%s</script>\n'
                % (i, ''.join('<line>%d</line>' % rnd.getrandbits(rnd.randint(1, 30)) for _ in range(rnd.randint(10, 400))))).encode()

    def blob(n):
        return bytes(rnd.getrandbits(8) for _ in range(n))

    def write_cat(cat, members, first=None):
        catx.write_catalog(cat, members)
        if first:  # stock catalogs say e.g. foo.dat
            lines = [first] + ['%s %d' % (p, len(b)) for p, b in members]
            idx = ('\n'.join(lines) + '\n').encode('latin1')
            open(cat, 'wb').write(bytes((c ^ ((0xDB + i) & 0xFF)) for i, c in enumerate(idx)))

    def catalog(n):
        m = [('scripts/!lib.s%d.pck' % i, gzip.compress(xml(i))) for i in range(n)]
        m += [('t/%04d-L044.pck' % (i + n), gzip.compress(xml(i + n))) for i in range(3)]
        m += [('types/TShips.pck', gzip.compress(b'22;0;\n' * 500)), ('maps/x3_universe.pck', gzip.compress(xml(99)))]
        # what .pck hold besides XML: textures, a plain list in director/, XML in types/, empty files, odd binaries
        m += [('dds/-%d.pck' % n, gzip.compress(b'DDS |\x00\x00\x00' + blob(4000))),
              ('director/mdfiles.pck', gzip.compress(b'0.1 Assassinate 1\r\n0.13 Follow Ship\r\n')),
              ('types/hq.pck', gzip.compress(b'<?xml version="1.0" encoding="iso-8859-1"?>\n<hq/>\n')),
              ('types/empty.pck', gzip.compress(b'')), ('t/empty.pck', gzip.compress(b'')),
              ('loadscr/logo.pck', gzip.compress(b'\xff\xd8\xff\xe0' + blob(3000))),
              ('misc/data.pck', gzip.compress(b'ab\x00cd' + blob(500)))]
        m += [('director/2.024 Terran Plot.pck', gzip.compress(xml(98))), ('L/x3story.obj', blob(30000)),
              ('objects/cut/00749 big model.pbd', gzip.compress(b'/=====\n// x3wcx test model\n' + b'1; 2; 3; -1; // point\n' * 3000)),
              ('objects/cut/00948.pbb', gzip.compress(b'CUT1INFO$' + blob(60000))),
              ('objects/ships/raw.bin', blob(200000)), ('textures/empty.txt', b''),
              ('Documents/readme.css', b'body {}\n' * 20)]
        rnd.shuffle(m)
        return m

    write_cat(os.path.join(root, '01.cat'), catalog(40))
    write_cat(os.path.join(root, '03.cat'), catalog(25))
    write_cat(os.path.join(root, '13.cat'), catalog(5), first='foo.dat')
    os.makedirs(os.path.join(root, 'addon', 'scripts'))
    write_cat(os.path.join(root, 'addon', '01.cat'), catalog(30))
    write_cat(os.path.join(root, 'addon', '04.cat'), catalog(10), first='foo.dat')
    for i in range(60):
        open(os.path.join(root, 'addon', 'scripts', 'plugin.test.%02d.pck' % i), 'wb').write(gzip.compress(xml(1000 + i)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=os.environ.get('X3_GAME'),
                    help='X3 TC/AP folder (only read); without it a small synthetic one is made (env X3_GAME)')
    ap.add_argument('--x3kc', default=os.environ.get('X3KC'),
                    help='checkout of https://github.com/DIMOSUS/x3kc, unless the package is installed (env X3KC)')
    ap.add_argument('--quick', action='store_true')
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--arch', choices=['x86', 'x64'])
    a = ap.parse_args()
    if a.x3kc:
        sys.path.insert(0, a.x3kc)
    try:
        from x3kc import catx, unpck
    except ImportError:
        sys.exit('x3kc not found: pass --x3kc <checkout of https://github.com/DIMOSUS/x3kc> or pip install it')
    tmp = tempfile.mkdtemp(prefix='x3wcx-test-')
    print('temp', tmp)
    try:
        if not a.game:
            a.game = os.path.join(tmp, 'game')
            make_synthetic_game(a.game, catx)
            print('no --game: using synthetic catalogs')
        for arch, exe, dll in DRIVERS:
            if a.arch and arch != a.arch:
                continue
            d = Drv(arch, exe, dll)
            t0 = time.time()
            test_basics(d, a.game, tmp)
            test_read_catalogs(d, a.game, tmp, catx, a.quick)
            test_pck_read(d, a.game, tmp, unpck, catx)
            test_pck_write(d, a.game, tmp, unpck, a.quick)
            test_models(d, a.game, tmp, unpck, catx)
            # byte-exact comparisons with catx.write_catalog: files go in as they are
            test_catalog_write(d.with_ini(write_ini(os.path.join(tmp, 'raw.ini'), 0)), a.game, tmp, catx)
            test_pack_on_add(d, a.game, tmp, catx, unpck)
            print('%s done in %.0fs' % (arch, time.time() - t0))
    finally:
        if not a.keep:
            shutil.rmtree(tmp, ignore_errors=True)
    print('FAILED %d' % len(failures) if failures else 'all passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
