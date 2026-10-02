#!/bin/bash
set -euo pipefail

DEVICE="/dev/secure_ram"
MODULE="secure_ram"
MOUNT_POINT="/mnt/secure_ram_test"
DEBUGFS="/sys/kernel/debug"
RAW_STORAGE="$DEBUGFS/secure_ram/raw_storage"
TMP_DIR="/tmp/secure_ram_test"
SECRET="CAPSTONE_SECRET_987654321"

cleanup() {
    set +e

    if mountpoint -q "$MOUNT_POINT"; then
        umount "$MOUNT_POINT"
    fi

    rm -rf "$TMP_DIR"

    if lsmod | grep -q "^${MODULE}"; then
        rmmod "$MODULE"
    fi
}

trap cleanup EXIT

echo "========================================"
echo " Secure RAM Disk Automated Test Suite"
echo "========================================"

if [[ $EUID -ne 0 ]]; then
    echo "Run this script with sudo:"
    echo "sudo ./tests/test_secure_ram.sh"
    exit 1
fi

echo
echo "[1/8] Loading kernel module..."

if lsmod | grep -q "^${MODULE}"; then
    echo "Module already loaded."
else
    insmod "./${MODULE}.ko"
fi

sleep 1

if [[ ! -b "$DEVICE" ]]; then
    echo "FAIL: $DEVICE not found."
    exit 1
fi

echo "PASS: $DEVICE exists."

echo
echo "[2/8] Raw block read/write test..."

printf '%s\n' "$SECRET" > /tmp/secure_ram_input

dd if=/tmp/secure_ram_input \
   of="$DEVICE" \
   bs=512 \
   conv=sync \
   status=none

dd if="$DEVICE" \
   of=/tmp/secure_ram_output \
   bs=512 \
   count=1 \
   status=none

if grep -a -q "$SECRET" /tmp/secure_ram_output; then
    echo "PASS: Raw block data round-trip succeeded."
else
    echo "FAIL: Raw block data mismatch."
    exit 1
fi

echo
echo "[3/8] Encryption verification..."

if ! mountpoint -q "$DEBUGFS"; then
    mount -t debugfs none "$DEBUGFS" 2>/dev/null || true
fi

if [[ ! -r "$RAW_STORAGE" ]]; then
    echo "FAIL: debugfs raw_storage not available."
    exit 1
fi

dd if="$RAW_STORAGE" \
   of=/tmp/secure_ram_raw \
   bs=512 \
   count=1 \
   status=none

if grep -a -q "$SECRET" /tmp/secure_ram_raw; then
    echo "FAIL: Plaintext detected in backing storage."
    exit 1
else
    echo "PASS: Plaintext not found in backing storage."
fi

echo
echo "[4/8] Creating ext4 filesystem..."

mkfs.ext4 -F "$DEVICE" >/dev/null

mkdir -p "$MOUNT_POINT"
mount "$DEVICE" "$MOUNT_POINT"

echo "PASS: ext4 filesystem mounted."

echo
echo "[5/8] Filesystem read/write test..."

echo "Hello from encrypted RAM disk" > "$MOUNT_POINT/test.txt"

if [[ "$(cat "$MOUNT_POINT/test.txt")" == "Hello from encrypted RAM disk" ]]; then
    echo "PASS: Filesystem file round-trip succeeded."
else
    echo "FAIL: Filesystem read/write mismatch."
    exit 1
fi

echo
echo "[6/8] Large-file integrity test..."

dd if=/dev/urandom \
   of="$MOUNT_POINT/source.bin" \
   bs=1M \
   count=4 \
   status=none

cp "$MOUNT_POINT/source.bin" /tmp/source_copy.bin
cp "$MOUNT_POINT/source.bin" "$MOUNT_POINT/roundtrip.bin"

sha256sum /tmp/source_copy.bin | awk '{print $1}' > /tmp/source.sha
sha256sum "$MOUNT_POINT/roundtrip.bin" | awk '{print $1}' > /tmp/roundtrip.sha

if cmp -s /tmp/source.sha /tmp/roundtrip.sha; then
    echo "PASS: 4 MiB SHA-256 integrity test succeeded."
else
    echo "FAIL: Large-file integrity mismatch."
    exit 1
fi

echo
echo "[7/8] Sector-boundary tests..."

for SIZE in 511 512 513 1023 1024 1025 4095 4096 4097; do
    TEST_FILE="$MOUNT_POINT/boundary_${SIZE}.bin"
    OUTPUT_FILE="/tmp/boundary_${SIZE}.bin"

    dd if=/dev/urandom \
       of="$TEST_FILE" \
       bs=1 \
       count="$SIZE" \
       status=none

    cp "$TEST_FILE" "$OUTPUT_FILE"

    if cmp -s "$TEST_FILE" "$OUTPUT_FILE"; then
        echo "PASS: ${SIZE}-byte boundary case."
    else
        echo "FAIL: ${SIZE}-byte boundary case."
        exit 1
    fi
done

echo
echo "[8/8] Teardown test..."

sync
umount "$MOUNT_POINT"

rmmod "$MODULE"

if lsmod | grep -q "^${MODULE}"; then
    echo "FAIL: Module still loaded."
    exit 1
fi

if [[ -b "$DEVICE" ]]; then
    echo "FAIL: $DEVICE still exists after module removal."
    exit 1
fi

echo "PASS: Clean unmount and module removal."

echo
echo "========================================"
echo " ALL TESTS PASSED ✅"
echo "========================================"
