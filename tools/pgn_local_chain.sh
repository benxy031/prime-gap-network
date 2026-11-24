#!/bin/sh
# Local isolation harness for PGN: a second chain built from the same policies.
#
# Requires a core binary carrying the PGN_LOCAL_CHAIN hook in
# pgn/testnet_definition.h and kernel/chainparams.cpp. With PGN_LOCAL_CHAIN
# unset that binary is the published testnet, so this script never touches it:
# different genesis, different network magic, different default ports, and its
# own data directories.
#
# Usage:
#   tools/pgn_local_chain.sh start            # nodes A and B, B connected to A
#   tools/pgn_local_chain.sh status
#   tools/pgn_local_chain.sh info             # heights, genesis, peer counts
#   tools/pgn_local_chain.sh mine 20          # bootstrap blocks on A, no GPU
#   tools/pgn_local_chain.sh cli A getblockhash 0
#   tools/pgn_local_chain.sh stop
#
# Environment:
#   PGN_LOCAL_CHAIN   chain suffix, default "phase0"
#   PGN_CORE_BIN      directory with riecoind and riecoin-cli
#   PGN_LOCAL_ROOT    state root, default build/local-chain/<suffix>
set -eu

SUFFIX=${PGN_LOCAL_CHAIN:-phase0}
ROOT=${PGN_LOCAL_ROOT:-build/local-chain/$SUFFIX}

if [ -z "${PGN_CORE_BIN:-}" ]; then
    for candidate in build/linux-runtime-ui238-gpu114-work/core/bin \
                     ../linux-runtime-ui238-gpu114-work/core/bin \
                     build/linux-runtime-ui238-gpu114/Core; do
        if [ -x "$candidate/riecoind" ]; then PGN_CORE_BIN=$candidate; break; fi
    done
fi
BIN=${PGN_CORE_BIN:-}
if [ -z "$BIN" ] || [ ! -x "$BIN/riecoind" ]; then
    echo "pgn_local_chain: riecoind not found; set PGN_CORE_BIN" >&2
    exit 2
fi

peer_port() { case $1 in A) echo 58951 ;; B) echo 58961 ;; *) return 1 ;; esac; }
rpc_port()  { case $1 in A) echo 58952 ;; B) echo 58962 ;; *) return 1 ;; esac; }
seed_of()   { case $1 in A) echo "" ;; B) echo 127.0.0.1:58951 ;; *) return 1 ;; esac; }
datadir()   { echo "$ROOT/$1"; }

cli() {
    label=$1
    shift
    "$BIN/riecoin-cli" -testnet=1 -datadir="$(datadir "$label")" -rpcport="$(rpc_port "$label")" "$@"
}

start_node() {
    label=$1
    mkdir -p "$(datadir "$label")"
    seed=$(seed_of "$label")
    listen=1
    connect=""
    if [ -n "$seed" ]; then listen=0; connect="-connect=$seed"; fi
    PGN_LOCAL_CHAIN="$SUFFIX" "$BIN/riecoind" \
        -testnet=1 -datadir="$(datadir "$label")" -server=1 \
        -dnsseed=0 -fixedseeds=0 -discover=0 -listenonion=0 -natpmp=0 \
        -rpcbind=127.0.0.1 -rpcallowip=127.0.0.1 \
        -rpcport="$(rpc_port "$label")" -port="$(peer_port "$label")" \
        -listen=$listen $connect -daemon=1 >/dev/null
    echo "started $label  peer=$(peer_port "$label") rpc=$(rpc_port "$label") ${connect:-<seed>}"
}

wait_ready() {
    i=0
    while [ $i -lt 120 ]; do
        if cli "$1" getblockchaininfo >/dev/null 2>&1; then return 0; fi
        i=$((i+1)); sleep 0.5
    done
    echo "node $1 did not become ready" >&2
    return 1
}

cmd_start() {
    echo "chain suffix: $SUFFIX"
    echo "state root:   $ROOT"
    start_node A
    wait_ready A
    start_node B
    wait_ready B
    echo "genesis A: $(cli A getblockhash 0)"
    echo "genesis B: $(cli B getblockhash 0)"
}

cmd_stop() {
    for label in A B; do
        if cli "$label" stop >/dev/null 2>&1; then echo "stopped $label"; fi
    done
    sleep 2
}

cmd_status() {
    for label in A B; do
        if cli "$label" getblockchaininfo >/dev/null 2>&1; then
            echo "$label running  blocks=$(cli "$label" getblockcount)  peers=$(cli "$label" getconnectioncount)"
        else
            echo "$label stopped"
        fi
    done
}

cmd_info() {
    for label in A B; do
        if cli "$label" getblockchaininfo >/dev/null 2>&1; then
            printf '%s blocks=%-4s peers=%-2s genesis=%s\n' "$label" \
                "$(cli "$label" getblockcount)" "$(cli "$label" getconnectioncount)" \
                "$(cli "$label" getblockhash 0)"
        else
            echo "$label stopped"
        fi
    done
}

cmd_mine() {
    python3 "$(dirname "$0")/pgn_local_blocks.py" \
        --datadir="$(datadir A)" --rpcport="$(rpc_port A)" --count="$1"
}

case ${1:-} in
    start)  cmd_start ;;
    stop)   cmd_stop ;;
    status) cmd_status ;;
    info)   cmd_info ;;
    mine)   cmd_mine "${2:-1}" ;;
    cli)    shift; cli "$@" ;;
    *)      sed -n '2,21p' "$0"; exit 2 ;;
esac
