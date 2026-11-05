#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# hotplug0_mut.py (11-04): mutation check of the daemon's hotplug0_boot (~/gpu/phxboot-hotplug0 ->
# debug.phxhotplug0). Each mutation breaks one property on a copy; run.sh's hp0_* scenarios must then FAIL.
#   cd tests/mock && python3 hotplug0_mut.py
import tempfile
import subprocess, sys, os
HERE=os.path.dirname(os.path.abspath(__file__)); os.chdir(HERE); src=os.path.join("..","..","tools","phxctl","phxboot-navi31.sh"); tmp=tempfile.mkdtemp(prefix="hp0mut."); s=open(src).read()
M=[('no_desk_gate','  [ $DESK = 1 ] || { echo "hotplug0: not a desktop boot','  true || { echo "hotplug0: not a desktop boot'),
   ('no_bootarg','    *" -phxhotplugoff "*) echo "hotplug0:','    *" -phxhotplugoffXX "*) echo "hotplug0:'),
   ('no_level_check','  case "$lvl" in 1|2) ;; *) echo "hotplug0:','  case "$lvl" in *) ;; x) echo "hotplug0:'),
   ('empty_is_2','<"$G/phxboot-hotplug0" 2>/dev/null); [ -n "$lvl" ] || lvl=1','<"$G/phxboot-hotplug0" 2>/dev/null); [ -n "$lvl" ] || lvl=2'),
   ('no_oid_gate','  sysctl -n debug.phxhotplug0 >/dev/null 2>&1 || {','  true || {'),
   ('fixed_level','out=$(sysctl -w debug.phxhotplug0=$lvl 2>&1)','out=$(sysctl -w debug.phxhotplug0=2 2>&1)'),
   ('before_acer','\nhotplug_boot\nhotplug0_boot   #','\nhotplug0_boot\nhotplug_boot   #'),
   ('not_called','\nhotplug0_boot   #','\n:   #')]
sc='hp0_off hp0_on hp0_l2 hp0_badflag hp0_nooid hp0_bootarg hp0_nodesk hp0_fail'.split()
# 11-04: PAR mutations at once (default 2), each one's run.sh serial (PAR=1) at nice 19; printed in order
from concurrent.futures import ThreadPoolExecutor
def check(mu):
    name,o,n=mu
    if s.count(o)!=1: return 'BAD',name,''
    f=os.path.join(tmp,name+'.sh'); open(f,'w').write(s.replace(o,n))
    r=subprocess.run(['nice','-n','19','bash','run.sh']+sc,env=dict(os.environ,DAEMON=f,PAR='1',SCR=os.path.join(tmp,'mock.'+name)),capture_output=True,text=True).stdout
    line=[l for l in r.splitlines() if l.startswith('RESULT')][-1]
    return ('MISSED',name,'') if ' 0 failed' in line else ('CAUGHT',name,line)
c=m=b=0
with ThreadPoolExecutor(max_workers=max(1,int(os.environ.get('PAR','2')))) as ex:
    for v,name,line in ex.map(check,M):
        print(v,name,line) if line else print(v,name)
        c+=v=='CAUGHT'; m+=v=='MISSED'; b+=v=='BAD'
import shutil; shutil.rmtree(tmp, ignore_errors=True)   # the failing runs' work dirs live under tmp
print(f'RESULT: {c} caught, {m} missed, {b} bad')
