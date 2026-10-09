#!/usr/bin/env bash

set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

SERVER_LOG="$(mktemp)"
CLIENT_LOG="$(mktemp)"
SECOND_CLIENT_LOG="$(mktemp)"

SERVER_PID=""
CLIENT_PID=""

cleanup() {
    # Stop any client still running.
    if [[ -n "$CLIENT_PID" ]]; then
        kill "$CLIENT_PID" 2>/dev/null || true
        wait "$CLIENT_PID" 2>/dev/null || true
    fi

    # Stop the server started by this test.
    if [[ -n "$SERVER_PID" ]]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi

    rm -f "$SERVER_LOG" "$CLIENT_LOG" "$SECOND_CLIENT_LOG"
}

fail() {
    echo
    echo "[FAIL] $*"
    echo "----- Server log -----"
    cat "$SERVER_LOG" || true
    echo "----- First client log -----"
    cat "$CLIENT_LOG" || true
    echo "----- Second client log -----"
    cat "$SECOND_CLIENT_LOG" || true
    exit 1
}

trap cleanup EXIT

echo "=========================================="
echo " DLock Module 6: Crash Recovery Test"
echo "=========================================="

# Build the project.
echo "[TEST] Building project..."
make all

# Start the server with line-buffered output.
echo "[TEST] Starting DLock server..."

stdbuf -oL -eL ./dlock_server >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!

# Wait for server startup.
SERVER_READY=0

for _ in $(seq 1 50); do
    if grep -q "Listening on TCP port" "$SERVER_LOG"; then
        SERVER_READY=1
        break
    fi

    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        fail "Server exited unexpectedly."
    fi

    sleep 0.1
done

[[ "$SERVER_READY" -eq 1 ]] ||
    fail "Server did not start within 5 seconds."

echo "[TEST] Server started."

# Start client 9901.
echo "[TEST] Starting lock-holder client 9901..."

stdbuf -oL -eL ./dlock_client 9901 >"$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!

# Wait for the client to enter the critical section.
ACQUIRED=0

for _ in $(seq 1 40); do
    if grep -q "ENTERING CRITICAL SECTION" "$CLIENT_LOG"; then
        ACQUIRED=1
        break
    fi

    if ! kill -0 "$CLIENT_PID" 2>/dev/null; then
        fail "Client 9901 exited before acquiring the lock."
    fi

    sleep 0.1
done

[[ "$ACQUIRED" -eq 1 ]] ||
    fail "Client 9901 did not enter the critical section in time."

echo "[TEST] Client 9901 acquired the lock."

# Forcefully terminate the client while it holds the lock.
echo "[TEST] Simulating client 9901 crash..."

kill -KILL "$CLIENT_PID" 2>/dev/null || true
wait "$CLIENT_PID" 2>/dev/null || true
CLIENT_PID=""

# Wait for the server to detect the crash and recover the lock.
echo "[TEST] Waiting for server recovery..."

RECOVERED=0

for _ in $(seq 1 80); do
    if grep -q "EVENT=RECOVERY CLIENT=9901" "$SERVER_LOG"; then
        RECOVERED=1
        break
    fi

    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        fail "Server exited before recording recovery."
    fi

    sleep 0.1
done

[[ "$RECOVERED" -eq 1 ]] ||
    fail "Server did not record recovery for client 9901."

echo "[TEST] Server recorded crash recovery."

# Start a second client and check that it can acquire the lock.
echo "[TEST] Starting client 9902..."

if ! timeout 12s ./dlock_client 9902 >"$SECOND_CLIENT_LOG" 2>&1; then
    fail "Client 9902 failed or timed out."
fi

if ! grep -q "EVENT=GRANT CLIENT=9902" "$SERVER_LOG"; then
    fail "Server did not grant the lock to client 9902."
fi

echo
echo "=========================================="
echo " [PASS] Client 9901 acquired the lock."
echo " [PASS] Client 9901 was terminated."
echo " [PASS] Server recorded crash recovery."
echo " [PASS] Client 9902 acquired the lock."
echo "=========================================="
echo "Module 6 crash recovery test passed!"
