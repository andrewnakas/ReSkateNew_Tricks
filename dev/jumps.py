# <dev> Per-step board jumps and trick events of one pass.ps1 run: python dev/jumps.py <tag>
import sys,re
lines=open(r'C:\Program Files (x86)\Steam\steamapps\common\Skate\logs\ReSkate.log',encoding='utf-8',errors='replace').read().splitlines()
tag=sys.argv[1]
start=max(i for i,l in enumerate(lines) if f'pass-{tag}-0-begin' in l)
prev=None
for l in lines[start:]:
    m=re.search(rf'pass-{tag}-(\d)-begin',l)
    if m and 'queued' not in l: print('== pass',m.group(1)); prev=None; continue
    if re.search(r'flick-it|no comply flip|flicked|board play |flip grab|carry at|physics 50\d',l) and 'ms deck' not in l: print('  ',l.split('Tricks: ')[-1][:220]); continue
    m=re.search(r'watch: (\d+) ms deck (-?\d+) deg, up (-?[\d.]+).*physics (\d+)',l)
    if m:
        t,a,u,ph=int(m[1]),int(m[2]),float(m[3]),m[4]
        if prev and t-prev[0]<40:
            da=abs(((a-prev[1])+180)%360-180); du=abs(u-prev[2])
            if (da>30 or du>0.5) and ph in('200','201','100','103'): print(f'   jump at {t}: heading {prev[1]}->{a}, up {prev[2]}->{u}, ph {ph}')
        prev=(t,a,u)
