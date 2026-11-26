#!/usr/bin/env python3
"""Converts the game's videos for the 3DS port.

Reads the videos out of the player's own Data.rsdk (the intro, the bad endings' eight clips and the good ending,
Theora 1024x512) and re-encodes them with ffmpeg at the top screen's size (400x200 by default, 500 kbit/s, a keyframe
every 240 frames), which costs the Old 3DS ~4 times less to decode and takes a sixth of the space (56 MB -> ~12 MB).
On an Old 2DS the intro then shows all of its frames but the first few; the pack's own 1024x512 videos play too, but
only a few frames a second (the game says so under the picture). Two ways to hand them to the port:

    python3 tools/convert-videos.py path/to/Data.rsdk Video
        a folder of converted videos: copy it into sdmc:/3ds/Sonic Mania 3DS/ (the port plays those first)
    python3 tools/convert-videos.py path/to/Data.rsdk --pack Data-3ds.rsdk
        a copy of Data.rsdk with the converted videos inside it (~44 MB smaller): rename it Data.rsdk and put it in
        place of the original on the SD card; nothing else is needed

    [--size 400x200] [--bitrate 500k] [--keep-original]

A constant bit rate, not a constant quality: the quiet stretches (most of the videos) get several times the bits
quality 4 gave them (which looked blocky), while the few heavy ones (the intro's 21-24 s and 53-55 s) stay as cheap to
decode as at quality 4. Quality 6 or 7 made those stop and jump on the Old 2DS; 450 and 520 kbit/s did not.

Needs ffmpeg built with libtheora (on PATH, or --ffmpeg). The game's data is never committed or shipped: each player
converts their own copy.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

# Data/Video/ files the game plays: TitleSetup (the intro) and the Ending scenes' UIVideo objects
VIDEOS = ['Mania.ogv', 'BadSonic.ogv', 'BadSonic2.ogv', 'BadTails.ogv', 'BadKnux.ogv', 'BadMighty.ogv', 'BadRay.ogv', 'BadEnd.ogv',
          'MREnd.ogv', 'GoodEnd.ogv']


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
        self.entries = [] # in the pack's order: (digest, offset, size, encrypted)
        pos = 8
        for _ in range(count):
            digest = self.data[pos:pos + 16]
            offset, size = struct.unpack_from('<II', self.data, pos + 16)
            pos += 24
            self.files[digest] = (offset, size & 0x7FFFFFFF, bool(size & 0x80000000))
            self.entries.append((digest, offset, size & 0x7FFFFFFF, bool(size & 0x80000000)))

    @staticmethod
    def digest(path):
        return word_swap(hashlib.md5(path.lower().encode()).digest())

    def write(self, path, replace):
        """A copy of the pack with the files in `replace` (digest -> contents) stored unencrypted, the others as they are
        (still encrypted: the key comes from a file's path and size, not from where it lies)."""
        header = 8 + 24 * len(self.entries)
        view = memoryview(self.data)
        entries, bodies, at = [], [], header
        for digest, offset, size, encrypted in self.entries:
            if digest in replace:
                body, flags = replace[digest], len(replace[digest])
            else:
                body, flags = view[offset:offset + size], size | (0x80000000 if encrypted else 0)
            entries.append(digest + struct.pack('<II', at, flags))
            bodies.append(body)
            at += len(body)
        if at > 0xFFFFFFFF:
            sys.exit('the new pack would be over 4 GB')
        with open(path, 'wb') as f:
            f.write(self.data[:8]) # RSDK, the version, the file count
            for e in entries:
                f.write(e)
            for b in bodies:
                f.write(b)
        return at

    def read(self, path):
        digest = word_swap(hashlib.md5(path.lower().encode()).digest())
        if digest not in self.files:
            return None
        offset, size, encrypted = self.files[digest]
        data = self.data[offset:offset + size]
        return decrypt(data, path, size) if encrypted else data


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pack_in', metavar='Data.rsdk', help="the game's data pack (yours, from the PC game)")
    ap.add_argument('out', nargs='?', help='output folder (copy it into sdmc:/3ds/Sonic Mania 3DS/)')
    ap.add_argument('--pack', metavar='NEW.rsdk', help='also write a copy of the pack with the converted videos inside it')
    ap.add_argument('--size', default='400x200', help='picture size, WxH (default 400x200: 1024x512 scaled to the top screen)')
    ap.add_argument('--bitrate', default='500k', help='video bit rate (default 500k: the intro is ~4.5 MB)')
    ap.add_argument('--quality', type=int, default=None,
                    help='a constant Theora quality 0-10 instead of the bit rate (4 is cheap but blocky; 6 stops in the heavy stretches)')
    ap.add_argument('--keyint', type=int, default=240, help='frames between keyframes (default 240)')
    ap.add_argument('--ffmpeg', default='ffmpeg')
    ap.add_argument('--keep-original', action='store_true', help='also write the pack\'s videos as they are (*.orig.ogv)')
    args = ap.parse_args()

    if not args.out and not args.pack:
        ap.error('give an output folder, --pack NEW.rsdk, or both')
    if args.pack and os.path.abspath(args.pack) == os.path.abspath(args.pack_in):
        ap.error('--pack must be a new file: put it in place of the original yourself once it is written')
    width, height = (int(v) for v in args.size.lower().split('x'))
    pack = Pack(args.pack_in)
    work = tempfile.TemporaryDirectory() if not args.out else None
    out = args.out or work.name
    os.makedirs(out, exist_ok=True)
    converted = {}
    total_in = total_out = 0
    for name in VIDEOS:
        data = pack.read('Data/Video/' + name)
        if data is None:
            print('%-14s not in the pack, skipped' % name)
            continue
        src = os.path.join(out, name[:-4] + '.orig.ogv')
        dst = os.path.join(out, name)
        with open(src, 'wb') as f:
            f.write(data)
        # One output frame per packet, at the stream's own rate, as the game plays them: libtheora counts packets and
        # the engine never sets the decoder's granule position, so the jumps in the pages' granule positions (the
        # intro's first frame "held" 7 s, a few more later) are not in the game. Following them, as ffmpeg does by
        # default, made the converted intro 7.5 s longer than its music.
        fps = theora_fps(data)
        vf = 'setpts=N/(%d/%d)/TB,scale=%d:%d:flags=lanczos' % (fps[0], fps[1], width, height)
        cmd = [args.ffmpeg, '-v', 'error', '-y', '-i', src, '-an', '-vf', vf, '-fps_mode', 'cfr', '-r', '%d/%d' % fps,
               '-c:v', 'libtheora'] + (['-q:v', str(args.quality)] if args.quality is not None else ['-b:v', args.bitrate]) + [
               '-g', str(args.keyint), dst]
        subprocess.run(cmd, check=True)
        if not args.keep_original:
            os.remove(src)
        size_out = os.path.getsize(dst)
        with open(dst, 'rb') as f:
            converted[Pack.digest('Data/Video/' + name)] = f.read()
        total_in += len(data)
        total_out += size_out
        print('%-14s %6.1f MB -> %5.1f MB' % (name, len(data) / 1e6, size_out / 1e6))
    if args.out:
        print('total          %6.1f MB -> %5.1f MB in %s' % (total_in / 1e6, total_out / 1e6, args.out))
    if args.pack:
        size = pack.write(args.pack, converted)
        print('%s: %.1f MB (the original %.1f MB), %d videos inside' % (args.pack, size / 1e6, len(pack.data) / 1e6, len(converted)))
    if work:
        work.cleanup()


if __name__ == '__main__':
    main()
