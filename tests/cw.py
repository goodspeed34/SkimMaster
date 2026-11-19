from contextlib import closing
import sys
import array, math, subprocess,tempfile, pathlib,sqlite3
mapping=dict(C='-.-.',Q='--.-',K='-.-',A='.-',B='-...',D='-..',E='.',S='...',T='-',N='-.')
mapping['1']='.----'
dot=round(48000*1.2/22)
unit=array.array('f')
for i in range(dot):
    phase=math.tau*700*i/48000
    unit.extend((.4*math.cos(phase),.4*math.sin(phase)))
zero=bytes(dot*8)
with tempfile.TemporaryDirectory() as d:
    db=pathlib.Path(d)/'spots'
    p=subprocess.Popen([sys.argv[1],'--receive','CW','-','14020000','test',db,'0'],stdin=subprocess.PIPE)
    p.stdin.write(zero*15)
    for word in ('CQ CQ DE K1ABC K1ABC TEST '*3).split():
        for ch in word:
            for mark in mapping[ch]:
                p.stdin.write(unit.tobytes()*(1 if mark=='.' else 3)+zero)
            p.stdin.write(zero*2)
        p.stdin.write(zero*4)
    p.stdin.write(zero*50);p.stdin.close()
    assert p.wait()==0
    with closing(sqlite3.connect(db)) as con, con:
        rows=con.execute("SELECT callsign,frequency_hz FROM spots").fetchall()
    assert rows and all(call=="K1ABC" and abs(hz-14020700)<12 for call,hz in rows),rows
