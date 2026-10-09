# DLock — Distributed Mutual Exclusion Service

DLock is a C11 TCP client/server demonstration of FIFO mutual exclusion.

## Modules
- **Module 1:** centralized TCP lock server and FIFO waiting queue.
- **Module 2:** Lamport logical clocks on protocol messages and persistent event logs in `logs/dlock.log`.
- **Module 3:** client heartbeats, a five-second lease timeout, lock recovery after disconnect/timeout, and a log-based safety checker.

## Build
```bash
make clean
make
```

## Run
Terminal 1:
```bash
./dlock_server
```
Terminal 2 and additional terminals:
```bash
./dlock_client 1
./dlock_client 2
./dlock_client 3
```
Run each client in a separate terminal. Each normal client holds the lock for about five seconds and sends heartbeats once per second.

## Check the log
After one or more clients finish:
```bash
./dlock_checker
```
The log is append-only and survives server restarts. Lamport time is initialized from the last timestamp in the log.

## Crash-recovery demo
Start the server, run two clients in separate terminals, then kill the first client while it holds the lock (for example, `Ctrl+C` in that client's terminal). The server should record `RECOVERY` and grant the lock to the next queued client. If a client remains connected but stops sending heartbeats, the server uses the lease timeout to recover the lock.

## Limitations
This is an educational localhost demo, not a production distributed lock service. The safety checker validates the recorded server event log; it cannot prove correctness for events missing from the log. Lease timeouts can also be affected by scheduling delays. A real deployment would require stronger fencing tokens, authentication, and network-partition handling.
