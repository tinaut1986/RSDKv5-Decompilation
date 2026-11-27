#!/bin/sh
# Sonic Mania 3DS: the video converter's window (convert-videos.py), on Linux and macOS.
# Checks for Python 3 with tkinter and for ffmpeg; when one is missing it says which packages it takes and offers to
# install them with the system's package manager (with sudo, except Homebrew), asking first.
cd "$(dirname "$0")" || exit 1

find_tools() {
    PY=""
    for p in python3 python; do
        if command -v "$p" >/dev/null 2>&1 && "$p" -c 'import sys; sys.exit(sys.version_info[0] < 3)' 2>/dev/null; then
            PY=$p
            break
        fi
    done
    missing=""
    [ -z "$PY" ] && missing="$missing python"
    [ -n "$PY" ] && ! "$PY" -c 'import tkinter' 2>/dev/null && missing="$missing tkinter"
    command -v ffmpeg >/dev/null 2>&1 || missing="$missing ffmpeg"
}

find_tools
if [ -n "$missing" ]; then
    echo "Needed to convert the videos, and not found:$missing"
    pkgs=""
    if command -v brew >/dev/null 2>&1; then
        for m in $missing; do
            case $m in python) pkgs="$pkgs python" ;; tkinter) pkgs="$pkgs python-tk" ;; ffmpeg) pkgs="$pkgs ffmpeg" ;; esac
        done
        cmd="brew install$pkgs"
    elif command -v apt-get >/dev/null 2>&1; then
        for m in $missing; do
            case $m in python) pkgs="$pkgs python3" ;; tkinter) pkgs="$pkgs python3-tk" ;; ffmpeg) pkgs="$pkgs ffmpeg" ;; esac
        done
        cmd="sudo apt-get install -y$pkgs"
    elif command -v dnf >/dev/null 2>&1; then
        for m in $missing; do
            case $m in python) pkgs="$pkgs python3" ;; tkinter) pkgs="$pkgs python3-tkinter" ;; ffmpeg) pkgs="$pkgs ffmpeg-free" ;; esac
        done
        cmd="sudo dnf install -y$pkgs"
    elif command -v pacman >/dev/null 2>&1; then
        for m in $missing; do
            case $m in python) pkgs="$pkgs python" ;; tkinter) pkgs="$pkgs tk" ;; ffmpeg) pkgs="$pkgs ffmpeg" ;; esac
        done
        cmd="sudo pacman -S --needed$pkgs"
    elif command -v zypper >/dev/null 2>&1; then
        for m in $missing; do
            case $m in python) pkgs="$pkgs python3" ;; tkinter) pkgs="$pkgs python3-tk" ;; ffmpeg) pkgs="$pkgs ffmpeg" ;; esac
        done
        cmd="sudo zypper install -y$pkgs"
    else
        echo "Install them with your system's package manager, then run this again."
        exit 1
    fi
    echo "This command installs them: $cmd"
    printf "Run it now? [y/N] "
    read -r answer
    case $answer in
        y | Y | yes | YES) sh -c "$cmd" || exit 1 ;;
        *) echo "Without them the videos cannot be converted."; exit 1 ;;
    esac
    find_tools
    if [ -n "$missing" ]; then
        echo "Still not found:$missing"
        exit 1
    fi
fi

exec "$PY" convert-videos.py --gui
