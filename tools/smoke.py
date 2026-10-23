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
in DF1."""
import hashlib, os, re, shutil, subprocess, sys, zipfile
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

def main():
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
