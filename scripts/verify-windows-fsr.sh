#!/usr/bin/env bash
# Verify the Windows FSR path (AMD's signed Vulkan DLLs) under Wine or Proton on Linux.
#
# This is the launcher's Windows environment, not a synthetic probe:
#   upscaling:     MK8_FSR4=1 MK8_FSR_VERSION=<4.1.2|3.1.6> MK8_FSR_LIB=<upscaler DLL>
#   frame gen:     MK8_FSR_FG=<fsr3|fsr4> MK8_FSR_FG_LIB=<framegeneration DLL>
# The runtime writes its own window screenshots and logs the [fsr]/[fsr_fg] lines.
#
# Usage:
#   scripts/verify-windows-fsr.sh --runtime <dir with mk8drecomp.exe and the game DLLs> \
#       --data <data root containing extracted/> --fsr-dir <dir with AMD's signed DLLs> [options]
#
# Options:
#   --version 4.1.2|3.1.6   upscaling provider (default 4.1.2)
#   --fg fsr3|fsr4          frame generation instead of upscaling
#   --proton [path]         use Proton (default: Proton-GE in compatibilitytools.d)
#   --out <dir>             evidence directory (default: mk8-windows-fsr-<timestamp>)
#   --timeout <seconds>     process timeout (default 300)
#   --frame <n>             screenshot frame (default 240)
#
# Exit codes: 0 verified, 1 environment/runtime failure, 2 frame generation reached but
# no interpolated frame was produced on this host (the DLL was loaded and queried).
set -u

RUNTIME= DATA= FSR_DIR= OUT= VERSION=4.1.2 FG= PROTON= TIMEOUT=300 FRAME=240

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --runtime) RUNTIME=${2:-}; shift 2 ;;
        --data) DATA=${2:-}; shift 2 ;;
        --fsr-dir) FSR_DIR=${2:-}; shift 2 ;;
        --version) VERSION=${2:-}; shift 2 ;;
        --fg) FG=${2:-}; shift 2 ;;
        --proton) PROTON=${2:-auto}; shift 2 ;;
        --out) OUT=${2:-}; shift 2 ;;
        --timeout) TIMEOUT=${2:-}; shift 2 ;;
        --frame) FRAME=${2:-}; shift 2 ;;
        -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

[ -n "$RUNTIME" ] && [ -d "$RUNTIME" ] || die "--runtime must be a directory"
[ -n "$DATA" ] && [ -d "$DATA" ] || die "--data must be a directory"
[ -d "$DATA/extracted" ] || die "--data must contain extracted/ (the runtime data root, not extracted/ itself)"
[ -n "$FSR_DIR" ] && [ -d "$FSR_DIR" ] || die "--fsr-dir must be a directory"
[ -x "$RUNTIME/mk8drecomp.exe" ] || die "$RUNTIME/mk8drecomp.exe not found"
case "$VERSION" in 4.1.2|3.1.6) ;; *) die "--version must be 4.1.2 or 3.1.6" ;; esac
if [ -n "$FG" ]; then
    case "$FG" in fsr3|fsr4) ;; *) die "--fg must be fsr3 or fsr4" ;; esac
fi
command -v wine >/dev/null || die "wine not found"

OUT=${OUT:-$PWD/mk8-windows-fsr-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT" || die "cannot create $OUT"

# AMD's signed DLLs, pinned by apps/launcher/src/fsr/install.rs (SDK v2.4.0).
check_dll() {
    local file=$1 bytes=$2 hash=$3 actual_bytes actual_hash
    [ -f "$file" ] || die "missing $file"
    actual_bytes=$(stat -c %s "$file")
    actual_hash=$(sha256sum "$file" | cut -d' ' -f1)
    [ "$actual_bytes" = "$bytes" ] || die "$file: $actual_bytes bytes, expected $bytes"
    [ "$actual_hash" = "$hash" ] || die "$file: SHA-256 $actual_hash, expected $hash"
    printf 'dll %-40s %12s bytes  sha256 %s\n' "$(basename "$file")" "$actual_bytes" "$actual_hash"
}
UPSCALER=$FSR_DIR/amd_fidelityfx_upscaler_vk.dll
FG_DLL=$FSR_DIR/amd_fidelityfx_framegeneration_vk.dll
check_dll "$UPSCALER" 36135696 4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7
if [ -n "$FG" ]; then
    check_dll "$FG_DLL" 36806928 d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4
fi

# Wine's Z: is /; a path inside a prefix may be passed as a Windows path already.
win_path() {
    case "$1" in
        [A-Za-z]:[\\/]*) printf '%s' "${1//\//\\}" ;;
        *) printf 'Z:%s' "${1//\//\\}" ;;
    esac
}

LOG=$OUT/run.log
SHOT=$OUT/fsr-$VERSION.png
[ -n "$FG" ] && SHOT=$OUT/fg-$FG.png
rm -f "$RUNTIME/mk8drecomp.log"

ENV=(MK8_EXPERIMENTAL_NO_MII=1 WINEDEBUG=-all LARECOMP_DATA_ROOT="$(win_path "$DATA")"
     LARECOMP_WINDOW=2560x1440 LARECOMP_WINDOW_SHOT="$FRAME"
     LARECOMP_SHOT_PATH="$(win_path "$SHOT")" LARECOMP_EXIT_AFTER_SHOT=1)
if [ -n "$FG" ]; then
    ENV+=(MK8_FSR_FG="$FG" MK8_FSR_FG_LIB="$(win_path "$FG_DLL")")
    [ "$FG" = fsr4 ] && ENV+=(MK8_FSR_MLFI_LIB="$(win_path "$FG_DLL")")
else
    ENV+=(MK8_FSR4=1 MK8_FSR_VERSION="$VERSION" MK8_FSR_LIB="$(win_path "$UPSCALER")")
fi

echo "== runtime: $RUNTIME"
echo "== data:    $DATA"
echo "== display: 2560x1440 virtual desktop (Wine explorer)"
if [ -n "$PROTON" ]; then
    if [ "$PROTON" = auto ]; then
        PROTON=$(find "$HOME/.local/share/Steam/compatibilitytools.d" -maxdepth 2 -name proton -type f 2>/dev/null | head -1)
    fi
    [ -n "$PROTON" ] && [ -x "$PROTON" ] || die "Proton not found; pass --proton <path>"
    echo "== proton:  $PROTON"
    ( cd "$RUNTIME" && env "${ENV[@]}" timeout "$TIMEOUT" "$PROTON" run ./mk8drecomp.exe --play ) >"$LOG" 2>&1
else
    ( cd "$RUNTIME" && env "${ENV[@]}" timeout "$TIMEOUT" \
        wine explorer /desktop=mk8fsr,"2560x1440" ./mk8drecomp.exe --play ) >"$LOG" 2>&1
fi
status=$?
# Under a Wine desktop the runtime's stderr may go to its own log next to the exe.
if [ -s "$RUNTIME/mk8drecomp.log" ] && grep -q '\[vk\]' "$RUNTIME/mk8drecomp.log"; then
    cp "$RUNTIME/mk8drecomp.log" "$OUT/runtime.log"
    LOG=$OUT/runtime.log
fi
echo "== exit:    $status (124 = timeout)"
echo "== log:     $LOG"
echo "== shot:    $SHOT"

faults=$(grep -cE 'GPUVM fault|VK_ERROR_DEVICE_LOST|device lost\?|FATAL:|Unhandled exception|Exception 0xc0000005' "$LOG" || true)
if [ -n "$FG" ]; then
    grep -E '\[fsr_fg\]' "$LOG" | tail -25
    if grep -q 'first interpolated frame recorded' "$LOG"; then
        echo "RESULT: frame generation produced an interpolated frame"
        exit 0
    fi
    echo "RESULT: frame generation did not produce an interpolated frame"
    if grep -q '\[fsr_fg\] disabled:' "$LOG"; then
        grep '\[fsr_fg\] disabled:' "$LOG" | tail -1 | sed 's/^/        clean refusal: /'
    elif grep -q '\[fsr_fg\] active:' "$LOG"; then
        echo "        the swapchain activated but no interpolated frame was recorded (timeout?)"
    else
        echo "        no [fsr_fg] activation or refusal line: the process ended while creating the provider context"
        echo "        rerun with WINEDEBUG=-all,+vulkan to record the failing Vulkan call"
    fi
    [ "$faults" -gt 0 ] && echo "        host fault lines: $faults (see $LOG)"
    exit 2
fi

grep -E '\[fsr\]' "$LOG" | head -15
for want in "[fsr] provider $VERSION" "[fsr] upscale active 1920x1080 -> 2560x1440"; do
    grep -qF "$want" "$LOG" || { echo "RESULT: missing \"$want\""; exit 1; }
done
[ -s "$SHOT" ] || { echo "RESULT: missing $SHOT"; exit 1; }
if [ "$faults" -gt 0 ]; then
    echo "RESULT: host fault reported ($faults lines; see $LOG)"
    exit 1
fi
echo "RESULT: FSR $VERSION upscaled 1920x1080 -> 2560x1440 and saved the window image"
exit 0
