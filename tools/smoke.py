"""Runs a folder of RP9 packages (or ADFs) on one host build for a long
while and sorts out the ones that look broken: each on the machine its
manifest names (an ADF: AMI_MODEL and the Kickstart given), a picture
every SHOTS frames, the 68000's exceptions logged. Per title: how many
different pictures, the longest stretch of identical ones, how much of
the run is dark, an alert (Guru Meditation, Software Failure) on screen,
the exceptions (address errors, illegal instructions, ...), a halted CPU;
and a contact sheet of its pictures (<out>/<nn>/sheet.png) to look at.
usage: smoke.py <folder> <out> <roms (folder) | kickstart.rom> <system hdfs> <ami_host>
environment: FRAMES (15000), SHOTS (250), JOBS (8), ONLY (part of a name),
INPUT (AMI_INPUT for every title: none by default), AMI_* passed on (an
ADF's machine: AMI_MODEL, AMI_CHIP_KB ...). An RP9's second floppy goes
in DF1.

Replays: titles that once failed, each on its machine with its input
(tools/replays.json), run to a fixed frame and checked against a
reference run of a good build:
  smoke.py replay record <replays.json> <media> <roms> <ami_host>
  smoke.py replay <replays.json> <media> <out> <roms> <ami_host>
(<media>: the disk images, and the references in <media>/ref/<name>/;
ONLY picks replays). The picture at the checkpoint must look like the
reference's. Each run also logs three streams, and the earliest place
where one parts from the reference says which side went wrong first:
  drive commands  the CPU's: CIA-B's drive lines, each DMA's start
                  (cylinder, length, where to): different first = the
                  CPU (or memory) asked for something else
  disk data       the floppy's: a checksum of each DMA's words: different
                  with the same commands = the floppy gave other data
  code            the CPU's path: an address executed that the reference
                  never ran, or an exception it never took
"""
import hashlib, json, os, re, shutil, subprocess, sys, zipfile
from concurrent.futures import ThreadPoolExecutor

FRAMES = int(os.environ.get('FRAMES', '15000'))
SHOTS = int(os.environ.get('SHOTS', '250'))
TIME = '1792656000'
# not counted: privilege violations (exec's Supervisor()), trace (copy
# protections), line A and F (probing), TRAP #n; and Kickstart's probes
# (an illegal instruction) in the first BOOT frames
QUIET_EXC = {8, 9, 10, 11} | set(range(32, 48))
BOOT = 400

def manifest(path):
    try:
        m = zipfile.ZipFile(path).read('rp9-manifest.xml').decode('utf-8-sig')
    except (KeyError, zipfile.BadZipFile):
        return '?', False
    system = re.search(r'<system>([^<]*)</system>', m)
    return system.group(1) if system else '?', '<harddrive' in m

def floppies(path):
    """An RP9's floppies in their order (the second goes in DF1)"""
    m = zipfile.ZipFile(path).read('rp9-manifest.xml').decode('utf-8-sig')
    found = re.findall(r'<floppy[^>]*priority="(\d+)"[^>]*>([^<]*)</floppy>', m)
    return [name for _, name in sorted(found, key=lambda f: int(f[0]))]

def picture(path):
    from PIL import Image
    return Image.open(path).convert('RGB')

def alert(im):
    """A dead-end alert (Guru Meditation, Software Failure): its box's top
    and bottom edges, red or yellow lines across most of the screen in the
    top rows, the rest of them black (the box blinks: some pictures catch
    it)"""
    w, h = im.size
    top = im.crop((0, 0, w, min(h, 120)))
    px = top.load()
    lines = dark = 0
    for y in range(top.size[1]):
        for x in range(0, w, 4):
            r, g, b = px[x, y]
            dark += r < 40 and g < 40 and b < 40
        run = best = 0
        for x in range(w):
            r, g, b = px[x, y]
            hot = r > 150 and b < 70 and (g < 70 or g > 150)
            run = run + 1 if hot else 0
            best = max(best, run)
        lines += best > w // 2
    return lines >= 2 and dark > top.size[1] * (w // 4) * 3 // 4

def dark(im):
    small = im.resize((90, 71))
    return all(r < 24 and g < 24 and b < 24 for r, g, b in small.get_flattened_data())

def sheet(folder, shots):
    from PIL import Image
    if not shots:
        return
    cols, tw, th = 8, 180, 142
    rows = (len(shots) + cols - 1) // cols
    out = Image.new('RGB', (cols * (tw + 2), rows * (th + 2)), (255, 0, 0))
    for i, f in enumerate(shots):
        im = picture(os.path.join(folder, f)).resize((tw, th))
        out.paste(im, ((i % cols) * (tw + 2), (i // cols) * (th + 2)))
    out.save(os.path.join(folder, 'sheet.png'))

def run(args, binary, n, name):
    src, out, roms, hdfs = args
    folder = os.path.join(out, '%03d' % n)
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    adf = not name.lower().endswith('.rp9')
    image = os.path.join(folder, 'disk.adf' if adf else 'title.rp9')
    shutil.copy(os.path.join(src, name), image)
    env = {k: v for k, v in os.environ.items() if not k.startswith('AMI_') or adf}
    system, hd = ('adf', False) if adf else manifest(image)
    if hd:
        for h in ('workbench-135.hdf', 'workbench-311.hdf'):
            shutil.copy(os.path.join(hdfs, h), folder)
    env['AMI_HD0' if hd else 'AMI_DF0'] = image
    if not adf:
        second = floppies(image)[1:2]
        if second:
            disk2 = os.path.join(folder, 'disk2' + os.path.splitext(second[0])[1])
            open(disk2, 'wb').write(zipfile.ZipFile(image).read(second[0]))
            env['AMI_DF1'] = disk2
    env.update(AMI_SHOTS=str(SHOTS), AMI_TIME=TIME, AMI_EXC='1')
    if os.environ.get('INPUT'):
        env['AMI_INPUT'] = os.environ['INPUT']
    try:
        p = subprocess.run([binary, roms, str(FRAMES), os.path.join(folder, 'shot.ppm')], env=env, cwd=folder,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=1200)
        log = p.stdout
    except subprocess.TimeoutExpired as e:
        log = (e.stdout or b'') + b'\n(timed out)\n'
    open(os.path.join(folder, 'log.txt'), 'wb').write(log)
    text = log.decode('latin1')
    exc = {}
    for v, frame in re.findall(r'-- exception (\d+) at PC \$\w+ \(frame (\d+)\)', text):
        if int(v) not in QUIET_EXC and int(frame) >= BOOT:
            exc[int(v)] = exc.get(int(v), 0) + 1
    shots = sorted((f for f in os.listdir(folder) if re.match(r'shot-\d+\.ppm', f)), key=lambda f: int(f[5:-4]))
    sums = [hashlib.md5(open(os.path.join(folder, f), 'rb').read()).hexdigest() for f in shots]
    longest = run_len = 0
    for i in range(1, len(sums)):
        run_len = run_len + 1 if sums[i] == sums[i - 1] else 0
        longest = max(longest, run_len)
    darks = alerts = 0
    for f in shots:
        im = picture(os.path.join(folder, f))
        darks += dark(im)
        alerts += alert(im)
    sheet(folder, shots)
    flags = []
    if 'halted' in text:
        flags.append('HALTED')
    if alerts:
        flags.append('ALERT x%d' % alerts)
    if exc:
        flags.append('exc ' + ','.join('%d:%d' % kv for kv in sorted(exc.items())))
    if shots and darks == len(shots):
        flags.append('ALL DARK')
    if shots and len(set(sums)) <= 2:
        flags.append('STILL')
    if not shots:
        flags.append('NO PICTURES')
    return dict(n=n, name=name, system=system, hd=hd, shots=len(shots), distinct=len(set(sums)),
                frozen=longest * SHOTS, dark=darks, flags=flags)

# ---- Replays ----

REPLAY_SHOTS = 250
LIKE = 0.90         # the checkpoint's picture: this share of blocks alike
DRIVES_RE = re.compile(r'^-- drives (\w+) \(frame (\d+)\), #(\d+)', re.M)
START_RE = re.compile(r'^-- disk (read|write) (\d+) words, sync (\w+)(?: \(not used\))?, cylinder (-?\d+) side (\d+), '
                      r'to \$(\w+) \(frame (\d+)\), #(\d+)', re.M)
END_RE = re.compile(r'^-- disk end (read|write) (\d+) words at \$(\w+), sum (\w+) \(frame (\d+)\), #(\d+)', re.M)
EXC_RE = re.compile(r'^-- exception (\d+) at PC \$(\w+) \(frame (\d+)\), SR \w+, #(\d+)', re.M)


def streams(folder):
    """The run's events, each (what, frame, instruction): the CPU's drive
    commands, the floppy's data, the exceptions; and the code it ran
    ({address: (frame, instruction)})"""
    log = open(os.path.join(folder, 'log.txt'), encoding='latin1').read()
    commands = [('lines ' + v, int(f), int(n)) for v, f, n in DRIVES_RE.findall(log)]
    commands += [('%s %s words, sync %s, cylinder %s side %s, to $%s' % e[:6], int(e[6]), int(e[7]))
                 for e in START_RE.findall(log)]
    commands.sort(key=lambda e: e[2])
    data = [('%s %s words at $%s, sum %s' % e[:4], int(e[4]), int(e[5])) for e in END_RE.findall(log)]
    exceptions = [('exception %s at $%s' % e[:2], int(e[2]), int(e[3]))
                  for e in EXC_RE.findall(log) if int(e[0]) not in QUIET_EXC]
    code = {}
    path = os.path.join(folder, 'cover.txt')
    if os.path.exists(path):
        for line in open(path):
            pc, frame, n = line.split()
            code[int(pc, 16)] = (int(frame), int(n))
    return dict(commands=commands, data=data, exceptions=exceptions, code=code)


def parted(ref, now, last):
    """Where two event lists part: (instruction, frame, what now, what the
    reference had) or None; a list that ended early parts at its run's end"""
    for i in range(max(len(ref), len(now))):
        a = ref[i] if i < len(ref) else None
        b = now[i] if i < len(now) else None
        if a is None or b is None or a[0] != b[0]:
            if b is None:
                return last[1], last[0], 'nothing more', a[0]
            return b[2], b[1], b[0], a[0] if a else 'nothing more'
    return None


def first_parting(ref, now):
    """Each stream's first difference from the reference, earliest first:
    [(instruction, frame, stream, what now, what the reference had)]"""
    last = max(now['code'].values(), key=lambda v: v[1], default=(0, 0))
    found = []
    for stream in ('commands', 'data'):
        p = parted(ref[stream], now[stream], last)
        if p:
            found.append((p[0], p[1], stream) + p[2:])
    known = {e[0] for e in ref['exceptions']}
    new = [e for e in now['exceptions'] if e[0] not in known]
    if new:
        found.append((new[0][2], new[0][1], 'exceptions', new[0][0], 'not taken'))
    new = [(v[1], v[0], pc) for pc, v in now['code'].items() if pc not in ref['code']]
    if new:
        n, frame, pc = min(new)
        found.append((n, frame, 'code', 'ran $%06x (%d addresses the reference never ran)' % (pc, len(new)),
                      'never there'))
    return sorted(found)


VERDICT = {
    'commands': 'the CPU asked the drives for something else first: a CPU, memory or chipset fault, the floppy did as told',
    'data': 'the floppy gave other data first, the commands the same: a floppy fault',
    'exceptions': 'the CPU took an exception the reference never did, before any disk difference',
    'code': 'the CPU went another way first, before any disk difference',
}


def likeness(a, b):
    """The share of 8 x 8 blocks (of 720 x 568) whose colours are close"""
    from PIL import Image
    w, h = 90, 71
    pa = picture(a).resize((w, h), Image.BOX).getdata()
    pb = picture(b).resize((w, h), Image.BOX).getdata()
    return sum(max(abs(x - y) for x, y in zip(p, q)) <= 24 for p, q in zip(pa, pb)) / (w * h)


def replay_run(r, media, folder, roms, binary):
    """One replay into folder; None, or why it couldn't run"""
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    m = r['machine']
    env = {k: v for k, v in os.environ.items() if not k.startswith('AMI_')}
    env.update(AMI_MODEL=str(m['model']), AMI_CHIP_KB=str(m['chip']), AMI_SLOW_KB=str(m['slow']),
               AMI_FAST_KB=str(m['fast']), AMI_TIME=TIME, AMI_EXC='1', AMI_DISKLOG='1',
               AMI_PCCOVER='cover.txt', AMI_SHOTS=str(REPLAY_SHOTS))
    if r.get('input'):
        env['AMI_INPUT'] = r['input']
    copies = []
    for d in r['disks']:
        src = os.path.join(media, d['file'])
        if not os.path.exists(src):
            return 'no %s (from %s)' % (d['file'], d['source'])
        if hashlib.md5(open(src, 'rb').read()).hexdigest() != d['md5']:
            return '%s is not the image named (%s)' % (d['file'], d['source'])
        copy = os.path.join(folder, 'df%d.adf' % d['drive'])
        shutil.copy(src, copy)
        copies.append(copy)
        env['AMI_DF%d' % d['drive']] = copy
    try:
        p = subprocess.run([binary, os.path.join(roms, m['kickstart']), str(r['checkpoint']), 'checkpoint.ppm'],
                           env=env, cwd=folder, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=1200)
        log = p.stdout
    except subprocess.TimeoutExpired as e:
        log = (e.stdout or b'') + b'\n(timed out)\n'
    open(os.path.join(folder, 'log.txt'), 'wb').write(log)
    for c in copies:
        os.remove(c)        # written to by the run: not kept
    shots = sorted((f for f in os.listdir(folder) if re.match(r'checkpoint-\d+\.ppm', f)),
                   key=lambda f: int(f[11:-4]))
    sheet(folder, shots)
    for f in shots:
        os.remove(os.path.join(folder, f))
    return None


def replay_check(r, media, out, roms, binary):
    ref = os.path.join(media, 'ref', r['name'])
    if not os.path.exists(os.path.join(ref, 'checkpoint.ppm')):
        return 'NO REF', 'no reference: smoke.py replay record first', []
    folder = os.path.join(out, r['name'])
    why = replay_run(r, media, folder, roms, binary)
    if why:
        return 'SKIPPED', why, []
    shot = os.path.join(folder, 'checkpoint.ppm')
    like = likeness(shot, os.path.join(ref, 'checkpoint.ppm')) if os.path.exists(shot) else 0.0
    found = first_parting(streams(ref), streams(folder))
    ok = like >= LIKE
    text = '%s at frame %d: picture %d%% like the reference\'s' % (r['expect'], r['checkpoint'], like * 100)
    return ('PASS' if ok else 'FAIL'), text, found


def replays(argv):
    record = argv[:1] == ['record']
    if record:
        spec, media, roms, binary = argv[1:5]
        out = None
    else:
        spec, media, out, roms, binary = argv[:5]
    spec, media, roms, binary = (os.path.abspath(a) for a in (spec, media, roms, binary))
    todo = [r for r in json.load(open(spec))['replays'] if os.environ.get('ONLY', '') in r['name']]
    if record:
        def one(r):
            why = replay_run(r, media, os.path.join(media, 'ref', r['name']), roms, binary)
            return r['name'], why or 'recorded (%s/ref/%s/sheet.png: check it shows %s)' % (media, r['name'], r['expect'])
        with ThreadPoolExecutor(int(os.environ.get('JOBS', '8'))) as ex:
            for name, text in ex.map(one, todo):
                print('%-14s %s' % (name, text))
        return 0
    out = os.path.abspath(out)
    with ThreadPoolExecutor(int(os.environ.get('JOBS', '8'))) as ex:
        results = list(ex.map(lambda r: (r,) + replay_check(r, media, out, roms, binary), todo))
    failed = 0
    for r, status, text, found in results:
        failed += status != 'PASS'
        print('%-14s %-7s %s' % (r['name'], status, text))
        if not found and status in ('PASS', 'FAIL'):
            print('               the same as the reference all through')
        for i, (n, frame, stream, now, was) in enumerate(found):
            print('               %s %-10s frame %5d, #%d: %s (reference: %s)' %
                  ('first' if i == 0 else '     ', stream, frame, n, now, was))
        if found:
            print('               -> ' + VERDICT[found[0][2]])
    print('%d of %d replays pass (contact sheets: %s/<name>/sheet.png)' % (len(results) - failed, len(results), out))
    return 1 if failed else 0


def main():
    if sys.argv[1:2] == ['replay']:
        sys.exit(replays(sys.argv[2:]))
    args = [os.path.abspath(a) for a in sys.argv[1:5]]
    binary = os.path.abspath(sys.argv[5])
    names = sorted(f for f in os.listdir(args[0])
                   if f.lower().endswith(('.rp9', '.adf')) and os.environ.get('ONLY', '') in f)
    with ThreadPoolExecutor(int(os.environ.get('JOBS', '8'))) as ex:
        results = list(ex.map(lambda j: run(args, binary, *j), enumerate(names)))
    bad = 0
    for r in results:
        bad += bool(r['flags'])
        print('%03d %-10s %-2s %3d pics, %3d different, frozen up to %5d frames, %3d dark  %-28s %s' %
              (r['n'], r['system'], 'hd' if r['hd'] else 'fd', r['shots'], r['distinct'], r['frozen'], r['dark'],
               ' '.join(r['flags']), r['name']))
    print('%d of %d flagged (contact sheets: %s/<nnn>/sheet.png)' % (bad, len(results), args[1]))

main()
