#!/usr/bin/env python3
"""IDR pictures of a T20 / T21 H.264 channel under IMP_Encoder_SetChnFrmRate and SetGOPSize (#367).

A model of the vendor IMP glue in do_channel_process_h264e (T20/T10 libimp 3.12.0, T21 1.0.33), written from
the disassembly, not from OpenIMP: the channel keeps the rate and GOP the encoder runs with (i264e parameters
5 and 7) and the rate and GOP the application asked for.  Before every coded picture:
  - encoder rate = min(source rate, asked rate); a new one keeps the GOP's whole seconds (gop * den / num,
    truncated) and rounds them up to pictures of the new rate; the new GOP is the one asked for from now on;
  - else, if the GOP asked for differs from the one in use: it is rounded up to a multiple of num * den of the
    rate, stored back and becomes the one in use;
  - whatever was sent to i264e sets the channel's IDR flag, which makes that picture an IDR.
CreateChn rounds the GOP up to a multiple of the reduced rate: T20 num * den, T21 num (25/1 for a rate of 0).
A picture is also an IDR when the pictures since the last IDR reach the GOP in use.

    gop_t2x_model.py t20|t21 > tests/t30/<soc>_gop_vectors.txt
Scenarios = tools/apitest/at_enc.c case gopfps (source 15 fps).  Output: G num den gop pictures /
E picture r num den | g gop / I IDR pictures.  The T20 output equals the vendor libimp 3.12.0 on a camera.
"""
import sys
from math import gcd

SC = [  # num den gop pics events
    (15, 1, 20, 70, []),
    (15, 1, 30, 100, [(20, 'r', 5, 1), (60, 'r', 15, 1)]),
    (15, 1, 30, 150, [(15, 'g', 20, 0), (70, 'g', 50, 0)]),
    (15, 1, 20, 70, [(10, 'g', 20, 0)]),
    (15, 2, 40, 80, []),
    (15, 1, 30, 60, [(10, 'r', 5, 1), (10, 'g', 20, 0)]),
]


def red(n, d):
    g = gcd(n, d)
    return (n // g, d // g) if g and n and d else (n, d)


def run(soc, num, den, gop, pics, ev):
    n, d = red(num, den)
    if not n or not d:
        n, d = 25, 1
    unit = n * d if soc == 't20' else n
    g = gop if gop % unit == 0 else (gop // unit + 1) * unit
    asked = g                          # channel +0xe8 (T20): the GOP asked for
    rate = (n, d)                      # asked rate
    src = (15, 1)
    idrs, since, flag = [], 0, True
    for i in range(pics):
        for e in ev:
            if e[0] != i:
                continue
            if e[1] == 'r':
                rate = red(e[2], e[3])
            else:
                asked = e[2]
        want = rate if rate[0] * src[1] < src[0] * rate[1] else src     # min(source, asked)
        if want[0] * d != n * want[1]:
            secs = g * d // n
            frames = secs * want[0]
            g = frames // want[1] + (frames % want[1] != 0)
            n, d = want
            asked = g
            flag = True
        elif g != asked:
            u = n * d
            if asked % u:
                asked = (asked // u + 1) * u
            g = asked
            flag = True
        if flag or since >= g or i == 0:
            idrs.append(i)
            since = 0
            flag = False
        since += 1
    return idrs


def main():
    soc = sys.argv[1] if len(sys.argv) > 1 else 't20'
    print('# %s: model of the vendor IMP glue (tools/gop_t2x_model.py); the T20 model equals libimp 3.12.0 on a camera (apitest case gopfps), #367' % soc.upper())
    for num, den, gop, pics, ev in SC:
        print('G %d %d %d %d' % (num, den, gop, pics))
        for e in ev:
            print('E %d r %d %d' % (e[0], e[2], e[3]) if e[1] == 'r' else 'E %d g %d' % (e[0], e[2]))
        print('I ' + ' '.join(str(i) for i in run(soc, num, den, gop, pics, ev)))


main()
