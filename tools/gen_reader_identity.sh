#!/usr/bin/env bash
#
# Generate a development Aliro identity.
#
#   tools/gen_reader_identity.sh [output-dir]     (default: main/certs)
#
# Produces:
#   reader_privkey.pem      reader key pair, private half   (stays on the ESP32)
#   reader_pubkey.pem       reader key pair, public half
#   credential_privkey.pem  test credential, private half   (goes to your test device)
#   credential_pubkey.pem   test credential, public half    (the reader's allow-list entry)
#
# These are development keys. They are not a provisioned Aliro credential and
# no wallet will accept them; they exist so the firmware and a test device
# emulator can complete a transaction on a bench.

set -euo pipefail

out_dir="${1:-$(cd "$(dirname "$0")/.." && pwd)/main/certs}"
mkdir -p "$out_dir"

if ! command -v openssl >/dev/null 2>&1; then
    echo "error: openssl not found in PATH" >&2
    exit 1
fi

gen_pair() {
    local name="$1"
    if [ -f "$out_dir/${name}_privkey.pem" ]; then
        echo "keeping existing $out_dir/${name}_privkey.pem"
        return
    fi
    openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 \
        -out "$out_dir/${name}_privkey.pem" 2>/dev/null
    openssl pkey -in "$out_dir/${name}_privkey.pem" -pubout \
        -out "$out_dir/${name}_pubkey.pem"
    chmod 600 "$out_dir/${name}_privkey.pem"
    echo "generated $out_dir/${name}_{priv,pub}key.pem"
}

gen_pair reader
gen_pair credential

echo "done. Keys are gitignored; do not commit them."
