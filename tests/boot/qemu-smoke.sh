#!/usr/bin/env bash
# Boot the release ISO through both firmware paths and require the shell
# prompt. Reaching the prompt exercises timer IRQs after STI, not merely
# the pre-interrupt kernel banner.
set -euo pipefail

iso=${1:-release/kvantos.iso}
qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
timeout_s=${BOOT_TIMEOUT_SECONDS:-60}
prompt='kvant:/$ '

if [[ ! -f "$iso" ]]; then
    echo "missing ISO: $iso" >&2
    exit 2
fi
if ! command -v "$qemu" >/dev/null 2>&1; then
    echo "QEMU not found: $qemu" >&2
    exit 2
fi

# Preserve serial/QEMU logs when CI requests them; default remains disposable.
if [[ -n ${BOOT_LOG_DIR:-} ]]; then
    mkdir -p "$BOOT_LOG_DIR"
    scratch=$(mktemp -d "$BOOT_LOG_DIR/$(basename "$iso").${BOOT_MEDIA:-cd}.XXXXXX")
else
    scratch=$(mktemp -d)
fi
active_pid=
cleanup() {
    if [[ -n "$active_pid" ]]; then
        kill "$active_pid" >/dev/null 2>&1 || true
        wait "$active_pid" >/dev/null 2>&1 || true
    fi
    if [[ -z ${BOOT_LOG_DIR:-} ]]; then rm -rf "$scratch"; fi
}
trap cleanup EXIT

run_case() {
    local label=$1
    local expected_marker=$2
    shift 2
    local safe_label=${label//[^a-zA-Z0-9_-]/_}
    local serial_log="$scratch/$safe_label.serial"
    local qemu_log="$scratch/$safe_label.qemu"

    local early_exit=0 qemu_status=0
    echo "[boot-smoke] $label image=$iso"
    "$qemu" "$@" \
        -display none -monitor none \
        -serial "file:$serial_log" \
        -no-reboot -no-shutdown \
        >"$qemu_log" 2>&1 &
    active_pid=$!

    for ((second = 0; second < timeout_s; second++)); do
        if grep -aFq -- "$prompt" "$serial_log" 2>/dev/null; then
            kill "$active_pid" >/dev/null 2>&1 || true
            wait "$active_pid" >/dev/null 2>&1 || true
            active_pid=
            if [[ -n "$expected_marker" ]] && ! grep -aFq -- "$expected_marker" "$serial_log"; then
                echo "[boot-smoke] FAIL: $label reached kvsh but missed marker '$expected_marker'" >&2
                cat "$serial_log" >&2
                cat "$qemu_log" >&2
                return 1
            fi
            if [[ "$label" == BIOS* && -n ${BIOS_MIN_RAM_MB:-} ]]; then
                local detected
                detected=$(sed -n 's/.*detected \([0-9][0-9]*\) MiB RAM.*/\1/p' "$serial_log" | head -1)
                if [[ -z "$detected" ]] || ((detected < BIOS_MIN_RAM_MB)); then
                    echo "[boot-smoke] FAIL: $label reported RAM '$detected', expected at least $BIOS_MIN_RAM_MB MiB" >&2
                    cat "$serial_log" >&2
                    return 1
                fi
            fi
            echo "[boot-smoke] PASS: $label reached kvsh"
            return 0
        fi
        if ! kill -0 "$active_pid" >/dev/null 2>&1; then
            if wait "$active_pid"; then qemu_status=0; else qemu_status=$?; fi
            active_pid=
            early_exit=1
            break
        fi
        sleep 1
    done

    if [[ -n "$active_pid" ]]; then
        kill "$active_pid" >/dev/null 2>&1 || true
        wait "$active_pid" >/dev/null 2>&1 || true
        active_pid=
    fi
    if ((early_exit)); then
        echo "[boot-smoke] FAIL: $label QEMU exited with status $qemu_status before kvsh" >&2
    else
        echo "[boot-smoke] FAIL: $label did not reach kvsh within ${timeout_s}s" >&2
    fi
    [[ -f "$serial_log" ]] && cat "$serial_log" >&2
    [[ -f "$qemu_log" ]] && cat "$qemu_log" >&2
    return 1
}

# BOOT_MEDIA=usb exercises the raw hybrid image, not El Torito CD boot.
if [[ ${BOOT_MEDIA:-cd} == usb ]]; then
    media=(-drive "if=none,id=bootmedia,format=raw,readonly=on,file=$iso"
           -device qemu-xhci -device usb-storage,drive=bootmedia -boot c)
else
    media=(-cdrom "$iso" -boot d)
fi

# BIOS / GRUB / 32-bit kernel, using an old CPU model as a compatibility check.
run_case "BIOS 32-bit (${BOOT_MEDIA:-cd})" "" \
    -machine pc,accel=tcg -cpu pentium -m 128 \
    -vga std "${media[@]}"

# Full releases also retain a bare-system floppy fallback. Test the real FDC
# path, not a floppy image attached as a hard disk. ISO-only CI has no floppy.
if [[ ${BOOT_MEDIA:-cd} == cd && -f release/kvantos-floppy.img ]]; then
    run_case "BIOS 32-bit (floppy)" "" \
        -machine pc,accel=tcg -cpu pentium -m 128 -vga std \
        -drive if=floppy,format=raw,readonly=on,file=release/kvantos-floppy.img -boot a
fi

# Find a matching OVMF CODE/VARS pair. CI installs the standard Ubuntu package.
ovmf_code=${OVMF_CODE:-}
ovmf_vars_template=${OVMF_VARS:-}
if [[ -n "$ovmf_code" || -n "$ovmf_vars_template" ]]; then
    if [[ ! -f "$ovmf_code" || ! -f "$ovmf_vars_template" ]]; then
        echo "set both OVMF_CODE and OVMF_VARS, or neither" >&2
        exit 2
    fi
else
    for pair in \
        "/usr/share/OVMF/OVMF_CODE.fd|/usr/share/OVMF/OVMF_VARS.fd" \
        "/usr/share/OVMF/OVMF_CODE_4M.fd|/usr/share/OVMF/OVMF_VARS_4M.fd"; do
        code=${pair%%|*}
        vars=${pair#*|}
        if [[ -f "$code" && -f "$vars" ]]; then
            ovmf_code=$code
            ovmf_vars_template=$vars
            break
        fi
    done
fi
if [[ ! -f "$ovmf_code" || ! -f "$ovmf_vars_template" ]]; then
    echo "OVMF CODE/VARS firmware pair not found (install the ovmf package)" >&2
    exit 2
fi

ovmf_vars="$scratch/OVMF_VARS.fd"
cp "$ovmf_vars_template" "$ovmf_vars"
run_case "UEFI 64-bit" "KvantEFI 3.1.1 (UEFI x86_64)" \
    -machine q35,accel=tcg -cpu max -m 256 \
    -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
    -drive "if=pflash,format=raw,file=$ovmf_vars" \
    "${media[@]}"
