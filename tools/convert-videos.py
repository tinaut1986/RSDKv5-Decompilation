#!/usr/bin/env python3
"""Converts the game's videos for the 3DS port.

Reads the videos out of the player's own Data.rsdk (the intro and the eight ending clips, Theora 1024x512) and
re-encodes them with ffmpeg at the top screen's size (400x200 by default, quality 4, a keyframe every 240 frames),
which costs the Old 3DS ~5 times less to decode and takes a tenth of the space (48.8 MB -> ~5 MB). On an Old 2DS the
intro then shows 97 % of its frames; the pack's own 1024x512 videos play too, but only a few frames a second. The results go to OUT_DIR; copy that folder's files to
sdmc:/3ds/Sonic Mania 3DS/Video/ and the port plays them instead of the pack's (Graphics/Video.cpp).

    python3 tools/convert-videos.py path/to/Data.rsdk out/Video [--size 400x200] [--quality 4] [--keep-original]

Needs ffmpeg built with libtheora (on PATH, or --ffmpeg). The game's data is never committed or shipped: each player
converts their own copy.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys

# Data/Video/ files the game plays: TitleSetup (the intro) and the Ending scenes' UIVideo objects
VIDEOS = ['Mania.ogv', 'BadSonic.ogv', 'BadSonic2.ogv', 'BadTails.ogv', 'BadKnux.ogv', 'BadMighty.ogv', 'BadRay.ogv', 'BadEnd.ogv',
          'MREnd.ogv']


def word_swap(digest):
    # The engine reads each 32-bit word of an MD5 as a little-endian integer (Reader.cpp)
    return b''.join(digest[i:i + 4][::-1] for i in range(0, 16, 4))


def decrypt(data, name, size):
    """Reader.cpp's DecryptBytes, keyed by the file's path and size."""
    key_a = word_swap(hashlib.md5(name.upper().encode()).digest())
    key_b = word_swap(hashlib.md5(str(size).encode()).digest())
    key_no = (size // 4) & 0x7F
    pos_a, pos_b, swap = 0, 8, 0
    out = bytearray(data)
    for i in range(len(out)):
        d = out[i] ^ key_no ^ key_b[pos_b]
        if swap:
            d = ((d << 4) | (d >> 4)) & 0xFF
        out[i] = d ^ key_a[pos_a]
        pos_a += 1
        pos_b += 1
        if pos_a <= 15:
            if pos_b > 12:
                pos_b = 0
                swap ^= 1
        elif pos_b <= 8:
            pos_a = 0
            swap ^= 1
        else:
            key_no = (key_no + 2) & 0x7F
            if swap:
                swap = 0
                pos_a = key_no % 7
                pos_b = key_no % 12 + 2
            else:
                swap = 1
                pos_a = key_no % 12 + 3
                pos_b = key_no % 7
    return bytes(out)


def theora_fps(ogv):
    """The frame rate (numerator, denominator) in the Theora identification header."""
    at = ogv.index(bytes([0x80]) + b'theora') + 7
    return struct.unpack('>II', ogv[at + 15:at + 23])


class Pack:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        if self.data[:4] != b'RSDK':
            sys.exit('%s is not an RSDK data pack' % path)
        count = struct.unpack_from('<H', self.data, 6)[0]
        self.files = {}
        pos = 8
        for _ in range(count):
            digest = self.data[pos:pos + 16]
            offset, size = struct.unpack_from('<II', self.data, pos + 16)
            pos += 24
            self.files[digest] = (offset, size & 0x7FFFFFFF, bool(size & 0x80000000))

    def read(self, path):
        digest = word_swap(hashlib.md5(path.lower().encode()).digest())
        if digest not in self.files:
            return None
        offset, size, encrypted = self.files[digest]
        data = self.data[offset:offset + size]
        return decrypt(data, path, size) if encrypted else data


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pack', help='Data.rsdk')
    ap.add_argument('out', help='output folder (its files go to sdmc:/3ds/Sonic Mania 3DS/Video/)')
    ap.add_argument('--size', default='400x200', help='picture size, WxH (default 400x200: 1024x512 scaled to the top screen)')
    ap.add_argument('--quality', type=int, default=4,
                    help='Theora quality 0-10 (default 4: the intro is 3 MB; 6 looks a little better, costs ~25 %% more to decode)')
    ap.add_argument('--keyint', type=int, default=240, help='frames between keyframes (default 240)')
    ap.add_argument('--ffmpeg', default='ffmpeg')
    ap.add_argument('--keep-original', action='store_true', help='also write the pack\'s videos as they are (*.orig.ogv)')
    args = ap.parse_args()

    width, height = (int(v) for v in args.size.lower().split('x'))
    pack = Pack(args.pack)
    os.makedirs(args.out, exist_ok=True)
    total_in = total_out = 0
    for name in VIDEOS:
        data = pack.read('Data/Video/' + name)
        if data is None:
            print('%-14s not in the pack, skipped' % name)
            continue
        src = os.path.join(args.out, name[:-4] + '.orig.ogv')
        dst = os.path.join(args.out, name)
        with open(src, 'wb') as f:
            f.write(data)
        # One output frame per packet, at the stream's own rate, as the game plays them: libtheora counts packets and
        # the engine never sets the decoder's granule position, so the jumps in the pages' granule positions (the
        # intro's first frame "held" 7 s, a few more later) are not in the game. Following them, as ffmpeg does by
        # default, made the converted intro 7.5 s longer than its music.
        fps = theora_fps(data)
        vf = 'setpts=N/(%d/%d)/TB,scale=%d:%d:flags=lanczos' % (fps[0], fps[1], width, height)
        cmd = [args.ffmpeg, '-v', 'error', '-y', '-i', src, '-an', '-vf', vf, '-fps_mode', 'cfr', '-r', '%d/%d' % fps,
               '-c:v', 'libtheora', '-q:v', str(args.quality), '-g', str(args.keyint), dst]
        subprocess.run(cmd, check=True)
        if not args.keep_original:
            os.remove(src)
        size_out = os.path.getsize(dst)
        total_in += len(data)
        total_out += size_out
        print('%-14s %6.1f MB -> %5.1f MB' % (name, len(data) / 1e6, size_out / 1e6))
    print('total          %6.1f MB -> %5.1f MB in %s' % (total_in / 1e6, total_out / 1e6, args.out))


if __name__ == '__main__':
    main()
