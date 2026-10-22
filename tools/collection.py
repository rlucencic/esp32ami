"""Runs a folder of RP9 games on two host builds and compares their pictures:
each game on the machine its manifest names (A500 with Kickstart 1.2 or
1.3, A500+ ECS with 1 MB chip RAM, A2000, A1200; hard disk games in DH0:
with workbench-135.hdf and workbench-311.hdf beside them), from a copy in
a folder of its own (games write to their disks), a click, fire, Space
and Return at 80-116 s, a picture every SHOTS frames up to FRAMES. Prints
each game's pictures that differ. The Amiga's clock starts at the same
time in every run (AMI_TIME: Epsilon 9 and Maniac Ball are seeded from
it).
usage: collection.py <games> <out> <roms> <system hdfs> tag=ami_host tag=ami_host
environment: FRAMES (7000), SHOTS (1000), JOBS (8), ONLY (part of a name)"""
import hashlib, os, re, shutil, subprocess, sys, zipfile
from concurrent.futures import ThreadPoolExecutor

INPUT = '4000:m:0:0:1;4005:m:0:0:0;4500:j:10;4505:j:0;5000:k:40:1;5005:k:40:0;5800:k:44:1;5805:k:44:0'
FRAMES = int(os.environ.get('FRAMES', '7000'))
SHOTS = int(os.environ.get('SHOTS', '1000'))
TIME = '1792656000'     # the Amiga's clock at the start (2026-10-22): games seeded from it

def manifest(path):
    m = zipfile.ZipFile(path).read('rp9-manifest.xml').decode('utf-8-sig')
    system = re.search(r'<system>([^<]*)</system>', m)
    rom = re.search(r'<systemrom>([^<]*)</systemrom>', m)
    return system.group(1) if system else '', rom.group(1) if rom else '', '<harddrive' in m

def machine(game, roms):
    system, rom, hd = manifest(game)
    env = {k: v for k, v in os.environ.items() if not k.startswith('AMI_')}
    if system in ('a-1200', 'a-4000'):
        kick = 'amiga-os-310-a1200.rom'
        env.update(AMI_MODEL='3', AMI_CHIP_KB='2048', AMI_SLOW_KB='0')
    else:
        kick = 'amiga-os-130.rom' if rom == '130' else 'amiga-os-120.rom'
        if system == 'a-500plus':
            env.update(AMI_ECS='1', AMI_CHIP_KB='1024', AMI_SLOW_KB='0')
        elif system == 'a-2000':
            env['AMI_MODEL'] = '1'
        if hd:
            env['AMI_FAST_KB'] = '1024'
    return os.path.join(roms, kick), env, system, hd

def run(args, tag, binary, n, name):
    games, out, roms, hdfs = args
    folder = os.path.join(out, tag, '%02d' % n)
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    rp9 = os.path.join(folder, 'game.rp9')
    shutil.copy(os.path.join(games, name), rp9)
    kick, env, system, hd = machine(rp9, roms)
    if hd:
        for h in ('workbench-135.hdf', 'workbench-311.hdf'):
            shutil.copy(os.path.join(hdfs, h), folder)
    env['AMI_HD0' if hd else 'AMI_DF0'] = rp9
    env.update(AMI_INPUT=INPUT, AMI_SHOTS=str(SHOTS), AMI_TIME=TIME)
    p = subprocess.run([binary, kick, str(FRAMES), os.path.join(folder, 'shot.ppm')], env=env, cwd=folder,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
    open(os.path.join(folder, 'log.txt'), 'wb').write(p.stdout)
    sums = {f[5:-4]: hashlib.md5(open(os.path.join(folder, f), 'rb').read()).hexdigest()
            for f in os.listdir(folder) if f.startswith('shot-') and f.endswith('.ppm')}
    return sums, system, hd

def main():
    args = [os.path.abspath(a) for a in sys.argv[1:5]]
    builds = [a.split('=', 1) for a in sys.argv[5:7]]
    names = sorted(f for f in os.listdir(args[0])
                   if f.endswith('.rp9') and os.environ.get('ONLY', '') in f)
    jobs = [(tag, os.path.abspath(binary), n, name) for tag, binary in builds for n, name in enumerate(names)]
    with ThreadPoolExecutor(int(os.environ.get('JOBS', '8'))) as ex:
        results = list(ex.map(lambda j: run(args, *j), jobs))
    same = 0
    for n, name in enumerate(names):
        (a, system, hd), (b, _, _) = results[n], results[len(names) + n]
        diff = sorted((f for f in set(a) | set(b) if a.get(f) != b.get(f)), key=int)
        same += not diff
        note = ' (%s: no pictures, see its log.txt)' % builds[0 if not a else 1][0] if not a or not b else ''
        print('%02d %-9s %-3s %-30s %s%s' % (n, system, 'hd' if hd else 'fd',
                                             'DIFF ' + ' '.join(diff) if diff else 'same', name, note))
    print('%d of %d the same (pictures in %s/<tag>/<nn>)' % (same, len(names), args[1]))

main()
