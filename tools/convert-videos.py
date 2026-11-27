#!/usr/bin/env python3
"""Converts the game's videos for the 3DS port.

Run without arguments (or double-click convert-videos.bat on Windows, convert-videos.sh on Linux and macOS, which also
install what is missing) for a window: pick Data.rsdk, a folder and what to make, and follow the bar. With arguments it
works in the terminal, as below.

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
import shutil
import struct
import subprocess
import sys
import tempfile
import time

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


def count_frames(ogv):
    """The Theora stream's frames: its packets after the three headers (each packet is a frame to the game)."""
    pos, serial, packets = 0, None, 0
    while pos + 27 <= len(ogv) and ogv[pos:pos + 4] == b'OggS':
        nseg = ogv[pos + 26]
        segs = ogv[pos + 27:pos + 27 + nseg]
        page_serial = struct.unpack_from('<I', ogv, pos + 14)[0]
        body = pos + 27 + nseg
        if serial is None and ogv[body:body + 7] == bytes([0x80]) + b'theora':
            serial = page_serial
        if page_serial == serial:
            packets += sum(1 for s in segs if s < 255)
        pos = body + sum(segs)
    return max(packets - 3, 1)


def find_ffmpeg(hint=None):
    """ffmpeg's path: the one given, the PATH's, or where winget puts it (a PATH a new install has not reached yet)."""
    if hint and (os.path.isfile(hint) or shutil.which(hint)):
        return shutil.which(hint) or hint
    found = shutil.which('ffmpeg')
    if found:
        return found
    links = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Microsoft', 'WinGet', 'Links', 'ffmpeg.exe')
    return links if os.path.isfile(links) else None


def ffmpeg_has_theora(ffmpeg):
    try:
        out = subprocess.run([ffmpeg, '-hide_banner', '-encoders'], capture_output=True, text=True, timeout=30,
                             creationflags=NO_WINDOW).stdout
    except (OSError, subprocess.SubprocessError):
        return False
    return 'libtheora' in out


NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0) # ffmpeg without a console window of its own (Windows)
# The packs' frame counts, for a bar that moves evenly before each video is read (another count only shifts it)
FRAMES = {'Mania.ogv': 1781, 'BadSonic.ogv': 319, 'BadSonic2.ogv': 319, 'BadTails.ogv': 316, 'BadKnux.ogv': 316, 'BadMighty.ogv': 319,
          'BadRay.ogv': 317, 'BadEnd.ogv': 419, 'MREnd.ogv': 419, 'GoodEnd.ogv': 1230}


class Cancelled(Exception):
    pass


def convert(pack_in, out=None, pack_out=None, size='400x200', bitrate='500k', quality=None, keyint=240, ffmpeg='ffmpeg',
            keep_original=False, progress=None, cancel=None, log=print):
    """Converts the videos of `pack_in` into the folder `out` and/or a new pack `pack_out`.
    progress(done, total, name, index, count): frames converted so far of all; cancel: a threading.Event."""
    width, height = (int(v) for v in size.lower().split('x'))
    log('Reading %s' % pack_in)
    pack = Pack(pack_in)
    work = tempfile.TemporaryDirectory() if not out else None
    folder = out or work.name
    os.makedirs(folder, exist_ok=True)
    present = [n for n in VIDEOS if Pack.digest('Data/Video/' + n) in pack.files]
    total = sum(FRAMES.get(n, 400) for n in present)
    done, converted, total_in, total_out = 0, {}, 0, 0
    try:
        for index, name in enumerate(present, 1):
            if progress:
                progress(done, total, name, index, len(present))
            data = pack.read('Data/Video/' + name)
            frames = count_frames(data)
            src = os.path.join(folder, name[:-4] + '.orig.ogv')
            dst = os.path.join(folder, name)
            with open(src, 'wb') as f:
                f.write(data)
            # One output frame per packet, at the stream's own rate, as the game plays them: libtheora counts packets
            # and the engine never sets the decoder's granule position, so the jumps in the pages' granule positions
            # (the intro's first frame "held" 7 s, a few more later) are not in the game. Following them, as ffmpeg does
            # by default, made the converted intro 7.5 s longer than its music.
            fps = theora_fps(data)
            vf = 'setpts=N/(%d/%d)/TB,scale=%d:%d:flags=lanczos' % (fps[0], fps[1], width, height)
            cmd = [ffmpeg, '-v', 'error', '-nostats', '-progress', 'pipe:1', '-y', '-i', src, '-an', '-vf', vf, '-fps_mode', 'cfr', '-r',
                   '%d/%d' % fps, '-c:v', 'libtheora'] + (['-q:v', str(quality)] if quality is not None else ['-b:v', bitrate]) + [
                       '-g', str(keyint), dst]
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, creationflags=NO_WINDOW)
            share = FRAMES.get(name, 400)
            for line in proc.stdout: # "frame=N" lines as ffmpeg goes
                if cancel is not None and cancel.is_set():
                    proc.terminate()
                    break
                if line.startswith('frame=') and progress:
                    n = int(line[6:].strip() or 0)
                    progress(done + share * min(n, frames) // frames, total, name, index, len(present))
            err = proc.stderr.read()
            proc.wait()
            if cancel is not None and cancel.is_set():
                for p in (src, dst):
                    if os.path.exists(p):
                        os.remove(p)
                raise Cancelled()
            if proc.returncode != 0:
                raise RuntimeError('ffmpeg failed on %s: %s' % (name, err.strip()[-400:]))
            if not keep_original:
                os.remove(src)
            size_out = os.path.getsize(dst)
            with open(dst, 'rb') as f:
                converted[Pack.digest('Data/Video/' + name)] = f.read()
            total_in += len(data)
            total_out += size_out
            done += share
            log('%-14s %6.1f MB -> %5.1f MB' % (name, len(data) / 1e6, size_out / 1e6))
        if progress:
            progress(total, total, '', len(present), len(present))
        if out:
            log('total          %6.1f MB -> %5.1f MB in %s' % (total_in / 1e6, total_out / 1e6, out))
        if pack_out:
            log('Writing %s' % pack_out)
            part = pack_out + '.part'
            written = pack.write(part, converted)
            if os.path.exists(pack_out):
                os.remove(pack_out)
            os.replace(part, pack_out)
            log('%s: %.1f MB (the original %.1f MB), %d videos inside' % (pack_out, written / 1e6, len(pack.data) / 1e6, len(converted)))
        return len(converted)
    finally:
        if work:
            work.cleanup()


# ---------------------------------------------------------------- the window (run without arguments, or with --gui)

def steam_candidates():
    """Where the PC game's Data.rsdk usually is."""
    home = os.path.expanduser('~')
    roots = [r'C:\Program Files (x86)\Steam', r'C:\Program Files\Steam', r'D:\Steam', r'D:\SteamLibrary', r'E:\SteamLibrary',
             os.path.join(home, '.steam', 'steam'), os.path.join(home, '.local', 'share', 'Steam'),
             os.path.join(home, '.var', 'app', 'com.valvesoftware.Steam', '.local', 'share', 'Steam'),
             os.path.join(home, 'Library', 'Application Support', 'Steam')]
    return [os.path.join(r, 'steamapps', 'common', 'Sonic Mania', 'Data.rsdk') for r in roots]


LANGS = ('en', 'fr', 'it', 'de', 'es', 'ja') # the port's own six
LANG_NAMES = ('English', 'Français', 'Italiano', 'Deutsch', 'Español', '日本語') # each in itself, for the selector
# The language picked in the window (CONVERT_VIDEOS_SETTINGS: another file, for tests)
SETTINGS = os.environ.get('CONVERT_VIDEOS_SETTINGS') or os.path.join(os.path.expanduser('~'), '.sonic-mania-3ds-converter.json')

# The window's texts, in the order of LANGS
STRINGS = {
    'intro': ("Converts the intro and ending videos of your Sonic Mania for the 3DS port.\n"
              "Without this, an Old 3DS shows them at a few frames a second.",
              "Convertit les vidéos d'intro et de fin de votre Sonic Mania pour le portage 3DS.\n"
              "Sans cela, une Old 3DS les affiche à quelques images par seconde.",
              "Converte i video dell'intro e dei finali del tuo Sonic Mania per il port 3DS.\n"
              "Senza questo, un Old 3DS li mostra a pochi fotogrammi al secondo.",
              "Konvertiert die Intro- und Endvideos deines Sonic Mania für die 3DS-Portierung.\n"
              "Ohne das zeigt ein Old 3DS sie mit wenigen Bildern pro Sekunde.",
              "Convierte los vídeos de la intro y de los finales de tu Sonic Mania para el port de 3DS.\n"
              "Sin esto, una Old 3DS los muestra a pocos fotogramas por segundo.",
              "Sonic Mania のイントロとエンディングのムービーを3DS版用に変換します。\n"
              "変換しないと、Old 3DSでは毎秒数コマでしか再生されません。"),
    'pack_label': ("Data.rsdk (from your PC game):", "Data.rsdk (de votre jeu PC) :", "Data.rsdk (dal gioco per PC):",
                   "Data.rsdk (aus deinem PC-Spiel):", "Data.rsdk (de tu juego de PC):", "Data.rsdk (PC版のもの):"),
    'browse': ("Browse...", "Parcourir...", "Sfoglia...", "Durchsuchen...", "Examinar...", "参照..."),
    'dest_label': ("Save to this folder:", "Enregistrer dans ce dossier :", "Salva in questa cartella:", "In diesem Ordner speichern:",
                   "Guardar en esta carpeta:", "保存先フォルダ:"),
    'make': ("Make", "Créer", "Crea", "Erstellen", "Crear", "作成するもの"),
    'mode_folder': ("A Video folder (about 12 MB): copy it into sdmc:/3ds/Sonic Mania 3DS/",
                    "Un dossier Video (environ 12 Mo) : copiez-le dans sdmc:/3ds/Sonic Mania 3DS/",
                    "Una cartella Video (circa 12 MB): copiala in sdmc:/3ds/Sonic Mania 3DS/",
                    "Einen Ordner Video (etwa 12 MB): kopiere ihn nach sdmc:/3ds/Sonic Mania 3DS/",
                    "Una carpeta Video (unos 12 MB): cópiala en sdmc:/3ds/Sonic Mania 3DS/",
                    "Video フォルダ (約12 MB): sdmc:/3ds/Sonic Mania 3DS/ にコピーしてください"),
    'mode_pack': ("A new Data.rsdk with the videos inside (about 44 MB smaller than yours): put it in place of the one on the SD card",
                  "Un nouveau Data.rsdk avec les vidéos dedans (environ 44 Mo de moins que le vôtre) : mettez-le à la place de celui de la carte SD",
                  "Un nuovo Data.rsdk con i video dentro (circa 44 MB in meno del tuo): mettilo al posto di quello sulla scheda SD",
                  "Eine neue Data.rsdk mit den Videos darin (etwa 44 MB kleiner als deine): ersetze damit die auf der SD-Karte",
                  "Un Data.rsdk nuevo con los vídeos dentro (unos 44 MB menos que el tuyo): ponlo en lugar del de la tarjeta SD",
                  "ムービー入りの新しい Data.rsdk (元より約44 MB小さい): SDカードのものと置き換えてください"),
    'convert': ("Convert", "Convertir", "Converti", "Konvertieren", "Convertir", "変換"),
    'cancel': ("Cancel", "Annuler", "Annulla", "Abbrechen", "Cancelar", "キャンセル"),
    'open': ("Open the folder", "Ouvrir le dossier", "Apri la cartella", "Ordner öffnen", "Abrir la carpeta", "フォルダを開く"),
    'no_ffmpeg': ("ffmpeg is needed (with libtheora): it was not found.", "ffmpeg est nécessaire (avec libtheora) : il est introuvable.",
                  "Serve ffmpeg (con libtheora): non è stato trovato.", "ffmpeg wird benötigt (mit libtheora): es wurde nicht gefunden.",
                  "Hace falta ffmpeg (con libtheora): no se ha encontrado.", "ffmpeg (libtheora対応) が必要ですが、見つかりません。"),
    'no_theora': ("This ffmpeg has no libtheora: another build is needed.", "Ce ffmpeg n'a pas libtheora : il en faut une autre version.",
                  "Questo ffmpeg non ha libtheora: serve un'altra versione.", "Dieses ffmpeg hat kein libtheora: eine andere Version wird benötigt.",
                  "Este ffmpeg no tiene libtheora: hace falta otra versión.", "この ffmpeg には libtheora がありません。別のビルドが必要です。"),
    'ffmpeg_help': ('Windows: run convert-videos.bat, which installs it, or "winget install Gyan.FFmpeg.Essentials".\n'
                    'Linux: install ffmpeg with your package manager (convert-videos.sh says how). macOS: "brew install ffmpeg".',
                    "Windows : lancez convert-videos.bat, qui l'installe, ou « winget install Gyan.FFmpeg.Essentials ».\n"
                    "Linux : installez ffmpeg avec votre gestionnaire de paquets (convert-videos.sh indique comment). macOS : « brew install ffmpeg ».",
                    'Windows: avvia convert-videos.bat, che lo installa, oppure "winget install Gyan.FFmpeg.Essentials".\n'
                    'Linux: installa ffmpeg con il gestore di pacchetti (convert-videos.sh spiega come). macOS: "brew install ffmpeg".',
                    'Windows: starte convert-videos.bat, das es installiert, oder "winget install Gyan.FFmpeg.Essentials".\n'
                    'Linux: installiere ffmpeg mit deiner Paketverwaltung (convert-videos.sh sagt wie). macOS: "brew install ffmpeg".',
                    'Windows: ejecuta convert-videos.bat, que lo instala, o "winget install Gyan.FFmpeg.Essentials".\n'
                    'Linux: instala ffmpeg con tu gestor de paquetes (convert-videos.sh dice cómo). macOS: "brew install ffmpeg".',
                    "Windows: convert-videos.bat を実行するとインストールされます。または「winget install Gyan.FFmpeg.Essentials」。\n"
                    "Linux: パッケージマネージャーで ffmpeg をインストール (方法は convert-videos.sh が表示)。macOS: 「brew install ffmpeg」。"),
    'get_ffmpeg': ("Get ffmpeg", "Obtenir ffmpeg", "Scarica ffmpeg", "ffmpeg holen", "Descargar ffmpeg", "ffmpeg を入手"),
    'pick_pack': ("Data.rsdk of Sonic Mania", "Data.rsdk de Sonic Mania", "Data.rsdk di Sonic Mania", "Data.rsdk von Sonic Mania",
                  "Data.rsdk de Sonic Mania", "Sonic Mania の Data.rsdk"),
    'pick_dest': ("Folder for the result", "Dossier pour le résultat", "Cartella per il risultato", "Ordner für das Ergebnis",
                  "Carpeta para el resultado", "結果の保存先フォルダ"),
    'err_pack': ("Choose the Data.rsdk of your Sonic Mania (in the PC game's folder).",
                 "Choisissez le Data.rsdk de votre Sonic Mania (dans le dossier du jeu PC).",
                 "Scegli il Data.rsdk del tuo Sonic Mania (nella cartella del gioco per PC).",
                 "Wähle die Data.rsdk deines Sonic Mania (im Ordner des PC-Spiels).",
                 "Elige el Data.rsdk de tu Sonic Mania (en la carpeta del juego de PC).",
                 "Sonic Mania の Data.rsdk を選んでください (PC版のフォルダにあります)。"),
    'err_same': ("Choose another folder to save to: the new Data.rsdk would replace your original.",
                 "Choisissez un autre dossier : le nouveau Data.rsdk remplacerait votre original.",
                 "Scegli un'altra cartella: il nuovo Data.rsdk sostituirebbe l'originale.",
                 "Wähle einen anderen Ordner: die neue Data.rsdk würde dein Original ersetzen.",
                 "Elige otra carpeta: el Data.rsdk nuevo sustituiría a tu original.",
                 "別のフォルダを選んでください。新しい Data.rsdk が元のファイルを上書きしてしまいます。"),
    'folder': ("Folder", "Dossier", "Cartella", "Ordner", "Carpeta", "フォルダ"),
    'cancelling': ("Cancelling...", "Annulation...", "Annullamento...", "Wird abgebrochen...", "Cancelando...", "キャンセル中..."),
    'converting': ("Converting {name} ({i} of {n})", "Conversion de {name} ({i} sur {n})", "Conversione di {name} ({i} di {n})",
                   "Konvertiere {name} ({i} von {n})", "Convirtiendo {name} ({i} de {n})", "{name} を変換中 ({i}/{n})"),
    'finishing': ("Finishing", "Finalisation", "Completamento", "Wird abgeschlossen", "Terminando", "仕上げ中"),
    'left': ("  -  about {m}:{s:02d} left", "  -  environ {m}:{s:02d} restantes", "  -  circa {m}:{s:02d} rimanenti",
             "  -  noch etwa {m}:{s:02d}", "  -  quedan unos {m}:{s:02d}", "  -  残り約 {m}:{s:02d}"),
    'how_folder': ("Copy the Video folder in {dest} into sdmc:/3ds/Sonic Mania 3DS/ on the SD card.",
                   "Copiez le dossier Video de {dest} dans sdmc:/3ds/Sonic Mania 3DS/ sur la carte SD.",
                   "Copia la cartella Video di {dest} in sdmc:/3ds/Sonic Mania 3DS/ sulla scheda SD.",
                   "Kopiere den Ordner Video aus {dest} nach sdmc:/3ds/Sonic Mania 3DS/ auf der SD-Karte.",
                   "Copia la carpeta Video de {dest} en sdmc:/3ds/Sonic Mania 3DS/ de la tarjeta SD.",
                   "{dest} の Video フォルダを、SDカードの sdmc:/3ds/Sonic Mania 3DS/ にコピーしてください。"),
    'how_pack': ("Copy the Data.rsdk in {dest} to sdmc:/3ds/Sonic Mania 3DS/ on the SD card, in place of the one there "
                 "(keep your original in the PC game's folder).",
                 "Copiez le Data.rsdk de {dest} dans sdmc:/3ds/Sonic Mania 3DS/ sur la carte SD, à la place de celui qui s'y trouve "
                 "(gardez votre original dans le dossier du jeu PC).",
                 "Copia il Data.rsdk di {dest} in sdmc:/3ds/Sonic Mania 3DS/ sulla scheda SD, al posto di quello presente "
                 "(tieni l'originale nella cartella del gioco per PC).",
                 "Kopiere die Data.rsdk aus {dest} nach sdmc:/3ds/Sonic Mania 3DS/ auf der SD-Karte und ersetze die dortige "
                 "(behalte dein Original im Ordner des PC-Spiels).",
                 "Copia el Data.rsdk de {dest} en sdmc:/3ds/Sonic Mania 3DS/ de la tarjeta SD, en lugar del que hay "
                 "(guarda tu original en la carpeta del juego de PC).",
                 "{dest} の Data.rsdk を、SDカードの sdmc:/3ds/Sonic Mania 3DS/ にあるものと置き換えてください "
                 "(元のファイルはPC版のフォルダに残しておいてください)。"),
    'done': ("Done: {n} videos converted in {m}:{s:02d}.", "Terminé : {n} vidéos converties en {m}:{s:02d}.",
             "Fatto: {n} video convertiti in {m}:{s:02d}.", "Fertig: {n} Videos in {m}:{s:02d} konvertiert.",
             "Hecho: {n} vídeos convertidos en {m}:{s:02d}.", "完了: {n}本のムービーを {m}:{s:02d} で変換しました。"),
    'cancelled': ("Cancelled. The videos finished before stay in the folder; the one being converted was removed.",
                  "Annulé. Les vidéos déjà terminées restent dans le dossier ; celle en cours a été supprimée.",
                  "Annullato. I video già completati restano nella cartella; quello in corso è stato eliminato.",
                  "Abgebrochen. Die fertigen Videos bleiben im Ordner; das gerade konvertierte wurde entfernt.",
                  "Cancelado. Los vídeos ya terminados se quedan en la carpeta; el que se estaba convirtiendo se ha borrado.",
                  "キャンセルしました。変換済みのムービーはフォルダに残ります。変換中のものは削除しました。"),
    'failed': ("It did not work:", "Cela n'a pas fonctionné :", "Non ha funzionato:", "Es hat nicht geklappt:", "No ha funcionado:",
               "失敗しました:"),
    'converting_title': ("Converting", "Conversion", "Conversione", "Konvertierung", "Convirtiendo", "変換中"),
    'language': ("Language:", "Langue :", "Lingua:", "Sprache:", "Idioma:", "言語:"),
    'stop_close': ("Stop the conversion and close?", "Arrêter la conversion et fermer ?", "Interrompere la conversione e chiudere?",
                   "Konvertierung abbrechen und schließen?", "¿Parar la conversión y cerrar?", "変換を中止して閉じますか？"),
}


def ui_language():
    """The PC's language among the port's six, English otherwise (CONVERT_VIDEOS_LANG overrides it)."""
    forced = os.environ.get('CONVERT_VIDEOS_LANG', '').lower()[:2]
    if forced in LANGS:
        return forced
    if sys.platform.startswith('win'):
        try:
            import ctypes
            primary = ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3FF
            code = {0x09: 'en', 0x0C: 'fr', 0x10: 'it', 0x07: 'de', 0x0A: 'es', 0x11: 'ja'}.get(primary)
            if code:
                return code
        except (AttributeError, OSError):
            pass
    import locale
    for value in (os.environ.get('LC_ALL'), os.environ.get('LC_MESSAGES'), os.environ.get('LANG'), locale.getlocale()[0]):
        if value:
            code = value[:2].lower()
            return code if code in LANGS else 'en'
    return 'en'


def run_gui(ffmpeg_hint=None):
    import queue
    import re
    import threading
    import tkinter as tk
    import webbrowser
    from tkinter import filedialog, messagebox, ttk

    import json

    lang = [LANGS.index(ui_language())]
    try:
        with open(SETTINGS, encoding='utf-8') as f:
            saved = json.load(f).get('language')
        if saved in LANGS and not os.environ.get('CONVERT_VIDEOS_LANG'):
            lang[0] = LANGS.index(saved)
    except (OSError, ValueError):
        pass

    def T(key, **kw):
        return STRINGS[key][lang[0]].format(**kw)

    # Every text that follows the language: the widgets' (option, key); the status and detail lines' last (key, args)
    texts = []
    shown = {'status': None, 'detail': None}

    def L(widget, key, option='text'):
        texts.append((widget, option, key))
        widget.configure(**{option: T(key)})
        return widget

    root = tk.Tk()
    root.title('Sonic Mania 3DS - video converter')
    root.resizable(False, False)
    pad = {'padx': 10, 'pady': 4}

    found = next((p for p in steam_candidates() if os.path.isfile(p)), '')
    desktop = os.path.join(os.path.expanduser('~'), 'Desktop')
    base = os.path.join(desktop if os.path.isdir(desktop) else os.path.expanduser('~'), 'Sonic Mania 3DS')
    pack_var = tk.StringVar(value=found)
    dest_var = tk.StringVar(value=base)
    mode_var = tk.IntVar(value=0)
    status_var = tk.StringVar()
    detail_var = tk.StringVar()

    L(ttk.Label(root, justify='left'), 'intro').grid(row=0, column=0, columnspan=2, sticky='w', **pad)

    # The language: the PC's at first, any of the six on request (kept for the next time)
    picker = ttk.Frame(root)
    picker.grid(row=0, column=2, sticky='ne', **pad)
    L(ttk.Label(picker), 'language').grid(row=0, column=0, padx=(0, 4))
    lang_box = ttk.Combobox(picker, values=LANG_NAMES, state='readonly', width=10)
    lang_box.current(lang[0])
    lang_box.grid(row=0, column=1)

    L(ttk.Label(root), 'pack_label').grid(row=1, column=0, sticky='w', **pad)
    ttk.Entry(root, textvariable=pack_var, width=60).grid(row=1, column=1, sticky='we', **pad)

    def pick_pack():
        p = filedialog.askopenfilename(title=T('pick_pack'), filetypes=[('Sonic Mania', 'Data.rsdk'), ('*', '*')])
        if p:
            pack_var.set(p)

    L(ttk.Button(root, command=pick_pack), 'browse').grid(row=1, column=2, **pad)

    L(ttk.Label(root), 'dest_label').grid(row=2, column=0, sticky='w', **pad)
    ttk.Entry(root, textvariable=dest_var, width=60).grid(row=2, column=1, sticky='we', **pad)

    def pick_dest():
        p = filedialog.askdirectory(title=T('pick_dest'))
        if p:
            dest_var.set(p)

    L(ttk.Button(root, command=pick_dest), 'browse').grid(row=2, column=2, **pad)

    modes = L(ttk.LabelFrame(root), 'make')
    modes.grid(row=3, column=0, columnspan=3, sticky='we', **pad)
    L(ttk.Radiobutton(modes, variable=mode_var, value=0), 'mode_folder').grid(row=0, column=0, sticky='w', padx=8, pady=2)
    L(ttk.Radiobutton(modes, variable=mode_var, value=1), 'mode_pack').grid(row=1, column=0, sticky='w', padx=8, pady=2)

    bar = ttk.Progressbar(root, maximum=1000, length=560)
    bar.grid(row=4, column=0, columnspan=3, sticky='we', **pad)
    ttk.Label(root, textvariable=status_var).grid(row=5, column=0, columnspan=3, sticky='w', **pad)
    ttk.Label(root, textvariable=detail_var, foreground='#555', justify='left', wraplength=640).grid(
        row=6, column=0, columnspan=3, sticky='w', **pad)

    buttons = ttk.Frame(root)
    buttons.grid(row=7, column=0, columnspan=3, sticky='e', **pad)
    start_btn = L(ttk.Button(buttons), 'convert')
    cancel_btn = L(ttk.Button(buttons, state='disabled'), 'cancel')
    open_btn = L(ttk.Button(buttons, state='disabled'), 'open')
    start_btn.grid(row=0, column=0, padx=4)
    cancel_btn.grid(row=0, column=1, padx=4)
    open_btn.grid(row=0, column=2, padx=4)

    def say(which, key=None, text=None, **kw):
        """The status or detail line: a text of the table (redone when the language changes) or one as it is."""
        shown[which] = (key, kw) if key else None
        (status_var if which == 'status' else detail_var).set(T(key, **kw) if key else (text or ''))

    def change_language(_event=None):
        lang[0] = lang_box.current()
        for widget, option, key in texts:
            widget.configure(**{option: T(key)})
        for which in ('status', 'detail'):
            if shown[which]:
                say(which, shown[which][0], **shown[which][1])
        try:
            with open(SETTINGS, 'w', encoding='utf-8') as f:
                json.dump({'language': LANGS[lang[0]]}, f)
        except OSError:
            pass

    lang_box.bind('<<ComboboxSelected>>', change_language)

    ffmpeg = find_ffmpeg(ffmpeg_hint)
    if not ffmpeg or not ffmpeg_has_theora(ffmpeg):
        say('status', 'no_ffmpeg' if not ffmpeg else 'no_theora')
        say('detail', 'ffmpeg_help')
        start_btn.configure(state='disabled')
        L(ttk.Button(buttons, command=lambda: webbrowser.open('https://ffmpeg.org/download.html')), 'get_ffmpeg').grid(
            row=0, column=3, padx=4)

    events = queue.Queue()
    cancel = threading.Event()
    state = {'running': False, 'start': 0.0}

    def open_folder():
        path = dest_var.get()
        if sys.platform.startswith('win'):
            os.startfile(path)
        else:
            subprocess.Popen(['open' if sys.platform == 'darwin' else 'xdg-open', path])

    open_btn.configure(command=open_folder)

    def set_inputs(on):
        for w in root.grid_slaves(row=1) + root.grid_slaves(row=2) + modes.grid_slaves():
            try:
                w.configure(state='normal' if on else 'disabled')
            except tk.TclError:
                pass

    def worker(pack_in, dest, mode):
        try:
            n = convert(pack_in, out=os.path.join(dest, 'Video') if mode == 0 else None,
                        pack_out=os.path.join(dest, 'Data.rsdk') if mode == 1 else None, ffmpeg=ffmpeg,
                        progress=lambda d, t, name, i, c: events.put(('progress', d, t, name, i, c)), cancel=cancel,
                        log=lambda s: events.put(('log', s)))
            events.put(('done', n))
        except Cancelled:
            events.put(('cancelled',))
        except Exception as e: # shown in the window: a bad file, a full disk, ffmpeg's own message
            events.put(('error', str(e)))

    def start():
        pack_in, dest, mode = pack_var.get().strip(), dest_var.get().strip(), mode_var.get()
        if not os.path.isfile(pack_in):
            messagebox.showerror('Data.rsdk', T('err_pack'))
            return
        if mode == 1 and os.path.abspath(os.path.join(dest, 'Data.rsdk')) == os.path.abspath(pack_in):
            messagebox.showerror('Data.rsdk', T('err_same'))
            return
        try:
            os.makedirs(dest, exist_ok=True)
        except OSError as e:
            messagebox.showerror(T('folder'), str(e))
            return
        cancel.clear()
        set_inputs(False)
        start_btn.configure(state='disabled')
        cancel_btn.configure(state='normal')
        open_btn.configure(state='disabled')
        bar['value'] = 0
        say('detail')
        state.update(running=True, start=time.time())
        threading.Thread(target=worker, args=(pack_in, dest, mode), daemon=True).start()

    start_btn.configure(command=start)
    cancel_btn.configure(command=lambda: (cancel.set(), say('status', 'cancelling')))

    def finish(ok):
        state['running'] = False
        set_inputs(True)
        start_btn.configure(state='normal')
        cancel_btn.configure(state='disabled')
        open_btn.configure(state='normal' if ok else 'disabled')

    def poll():
        try:
            while True:
                ev = events.get_nowait()
                if ev[0] == 'progress':
                    _, d, t, name, i, c = ev
                    bar['value'] = 1000 * d / max(t, 1)
                    secs = time.time() - state['start']
                    text = T('converting', name=name, i=i, n=c) if name else T('finishing')
                    if d > t * 0.03:
                        m, s = divmod(int(secs / d * (t - d)), 60)
                        text += T('left', m=m, s=s)
                    say('status', text=text)
                elif ev[0] == 'log':
                    if re.match(r'^\S+\.ogv ', ev[1]): # "Mania.ogv  30.3 MB -> 4.6 MB": numbers, in any language
                        say('detail', text=ev[1])
                elif ev[0] == 'done':
                    bar['value'] = 1000
                    m, s = divmod(int(time.time() - state['start']), 60)
                    finish(True)
                    say('status', 'done', n=ev[1], m=m, s=s)
                    say('detail', 'how_folder' if mode_var.get() == 0 else 'how_pack', dest=dest_var.get())
                elif ev[0] == 'cancelled':
                    bar['value'] = 0
                    finish(False)
                    say('status', 'cancelled')
                    say('detail')
                elif ev[0] == 'error':
                    finish(False)
                    say('status', 'failed')
                    say('detail', text=ev[1])
        except queue.Empty:
            pass
        root.after(100, poll)

    def on_close():
        if state['running'] and not messagebox.askyesno(T('converting_title'), T('stop_close')):
            return
        cancel.set()
        root.destroy()

    root.protocol('WM_DELETE_WINDOW', on_close)
    root.after(100, poll)
    if len(sys.argv) > 1 and '--shot' in sys.argv: # the docs' picture: the window as it opens, then closed
        root.after(1500, root.destroy)
    if os.environ.get('CONVERT_VIDEOS_PICK') in LANGS: # tests: a language picked in the selector, as a person would
        root.after(400, lambda: (lang_box.current(LANGS.index(os.environ['CONVERT_VIDEOS_PICK'])), change_language()))
    if os.environ.get('CONVERT_VIDEOS_AUTOSTART'): # tests: Convert pressed at once, into the folder the variable names
        dest_var.set(os.environ['CONVERT_VIDEOS_AUTOSTART'])
        root.after(500, start)
    root.mainloop()


def main():
    if len(sys.argv) == 1 or '--gui' in sys.argv:
        ffmpeg_hint = None
        if '--ffmpeg' in sys.argv:
            ffmpeg_hint = sys.argv[sys.argv.index('--ffmpeg') + 1]
        try:
            run_gui(ffmpeg_hint)
            return
        except ImportError: # no tkinter (some Linux installs): the terminal way
            print('No tkinter here: run it with arguments (python3 convert-videos.py --help), or install tkinter.')
            if len(sys.argv) == 1:
                sys.exit(1)

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pack_in', metavar='Data.rsdk', help="the game's data pack (yours, from the PC game)")
    ap.add_argument('out', nargs='?', help='output folder (copy it into sdmc:/3ds/Sonic Mania 3DS/)')
    ap.add_argument('--pack', metavar='NEW.rsdk', help='also write a copy of the pack with the converted videos inside it')
    ap.add_argument('--size', default='400x200', help='picture size, WxH (default 400x200: 1024x512 scaled to the top screen)')
    ap.add_argument('--bitrate', default='500k', help='video bit rate (default 500k: the intro is ~4.5 MB)')
    ap.add_argument('--quality', type=int, default=None,
                    help='a constant Theora quality 0-10 instead of the bit rate (4 is cheap but blocky; 6 stops in the heavy stretches)')
    ap.add_argument('--keyint', type=int, default=240, help='frames between keyframes (default 240)')
    ap.add_argument('--ffmpeg', default=None, help='ffmpeg to use (default: the one on the PATH)')
    ap.add_argument('--keep-original', action='store_true', help='also write the pack\'s videos as they are (*.orig.ogv)')
    ap.add_argument('--gui', action='store_true', help='the window (also when run without arguments)')
    args = ap.parse_args()

    if not args.out and not args.pack:
        ap.error('give an output folder, --pack NEW.rsdk, or both')
    if args.pack and os.path.abspath(args.pack) == os.path.abspath(args.pack_in):
        ap.error('--pack must be a new file: put it in place of the original yourself once it is written')
    ffmpeg = find_ffmpeg(args.ffmpeg)
    if not ffmpeg or not ffmpeg_has_theora(ffmpeg):
        sys.exit('ffmpeg with libtheora is needed (https://ffmpeg.org/download.html; Windows: winget install Gyan.FFmpeg.Essentials)')
    convert(args.pack_in, args.out, args.pack, args.size, args.bitrate, args.quality, args.keyint, ffmpeg, args.keep_original)


if __name__ == '__main__':
    main()
