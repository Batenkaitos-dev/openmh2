#!/usr/bin/env python3
"""hfr_clip.py DUMPDIR OUT.mp4 [FPS] [W:H] [DUMPDIR2]: a clip of the pictures an MH2_HFR_DUMP folder holds (development builds).

The folder has rVBLANK.png (the real picture of a step) and mVBLANK_ALPHA.png (an in-between picture, alpha in 1/256, made at that VSync). They are put
in the order they were shown: by VBlank, then alpha (the real picture last: alpha 256), and made into one clip at FPS (default 144; with the fixed alphas
MH2_HFR_ALPHAS=51,102,154,205 a step has 5 pictures and 30 steps a second is 150 pictures a second: use 150). With DUMPDIR2 the two folders go side by
side (the first on the left): two dumps of the same VSyncs with the same alphas have the same pictures. W:H is the clip's size (default 1280:896;
side by side, the width is shared). Uses ffmpeg (with libopenh264) from the environment variable MH2_FFMPEG, else on the PATH. Never commit a clip: it is the game's picture."""
import os
import re
import shutil
import subprocess
import sys
import tempfile


def find_ffmpeg():
    return os.environ.get('MH2_FFMPEG') or shutil.which('ffmpeg') or 'ffmpeg'


def order(d):
    out = []
    for f in os.listdir(d):
        m = re.match(r'r(\d+)\.png$', f)
        if m:
            out.append((int(m.group(1)), 256, f))
            continue
        m = re.match(r'm(\d+)_(\d+)\.png$', f)
        if m:
            out.append((int(m.group(1)), int(m.group(2)), f))
    out.sort()
    return [f for (_, _, f) in out]


def link(d, tmp):
    files = order(d)
    os.makedirs(tmp)
    for i, f in enumerate(files):
        src = os.path.join(os.path.abspath(d), f)
        dst = os.path.join(tmp, '%06d.png' % i)
        try:
            os.symlink(src, dst)
        except OSError:
            shutil.copyfile(src, dst)
    return len(files)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    d1, out = sys.argv[1], sys.argv[2]
    fps = sys.argv[3] if len(sys.argv) > 3 else '144'
    size = sys.argv[4] if len(sys.argv) > 4 else '1280:896'
    d2 = sys.argv[5] if len(sys.argv) > 5 else None
    ff = find_ffmpeg()
    tmp = tempfile.mkdtemp(prefix='hfr_clip_')
    try:
        n = link(d1, os.path.join(tmp, 'a'))
        enc = ['-c:v', 'libopenh264', '-b:v', '12M', '-pix_fmt', 'yuv420p']
        if d2:
            n = min(n, link(d2, os.path.join(tmp, 'b')))
            w, h = size.split(':')
            half = '%d:%d' % (int(w) // 2, int(h))
            cmd = [ff, '-y', '-loglevel', 'error', '-framerate', fps, '-i', os.path.join(tmp, 'a', '%06d.png'), '-framerate', fps, '-i', os.path.join(tmp, 'b', '%06d.png'),
                   '-filter_complex', '[0]scale=%s:flags=neighbor[l];[1]scale=%s:flags=neighbor[r];[l][r]hstack' % (half, half), '-frames:v', str(n)] + enc + [out]
        else:
            cmd = [ff, '-y', '-loglevel', 'error', '-framerate', fps, '-i', os.path.join(tmp, 'a', '%06d.png'), '-vf', 'scale=%s:flags=neighbor' % size] + enc + [out]
        subprocess.check_call(cmd)
        print('%s: %d pictures at %s fps' % (out, n, fps))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
