"""A Workbench floppy that installs itself on DH0: (the hard disk test).

Its startup-sequence formats DH0:, copies the floppy onto it and writes
DH0:S/Startup-Sequence (ENV:, LoadWB); boot it, press Return at Format's
prompt, then take the floppy out and reset: the Amiga boots from DH0:.

  python3 tools/mkinstall.py <workbench.adf> <out.adf> <format> <options> [copy options]
  Workbench 3.1: sys:system/format "FFS QUICK NOICONS" clone
  Workbench 1.3: sys:system/format NOICONS            (no CLONE in 1.3's Copy)
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from adf import ADF

a = ADF(sys.argv[1])
old = a.read(a.lookup('s/startup-sequence'))
# (1.x wants a redirection right after the command, and has no >>)
script = ('makedir ram:env\nassign env: ram:env\nassign t: ram:\n'
          '%s drive DH0: name HD %s\n' % (sys.argv[3], sys.argv[4]) +
          'copy df0: dh0: all %s quiet\n' % (sys.argv[5] if len(sys.argv) > 5 else '') +
          'echo >dh0:s/startup-sequence "makedir ram:env*Nassign env: ram:env*Nc:loadwb*Nendcli >nil:"\n'
          'echo "copied"\n').encode()
assert len(script) < len(old), (len(script), len(old))
a.write_same_size(a.lookup('s/startup-sequence'), script + b' ' * (len(old) - len(script) - 1) + b'\n')
a.save(sys.argv[2])
print(sys.argv[2])
