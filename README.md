# DLock — Strict FIFO Lock with a Real Shared File

DLock is a C11 TCP client/server Operating Systems project. All READ and WRITE requests enter the same FIFO queue and use one exclusive lock: only one client can hold access at a time, even when the mode is READ.

## Build

```bash
make clean
make
```

## Run

Run all commands from the project root directory so every client accesses the same `shared_file.txt`.

Terminal 1 — server:

```bash
./dlock_server
```

Terminal 2 — choose READ/WRITE interactively:

```bash
./dlock_client 1
```

Terminal 3 — another client:

```bash
./dlock_client 2
```

The client asks you to select `R` or `W`. Once granted, the selected mode and exclusive lock stay active until you type `q` and press Enter (or close the client connection). Other clients wait in the FIFO queue.

## Actual file behavior

The shared file is `shared_file.txt` in the project root.

- **WRITE mode:** type any line and press Enter. That line is appended immediately to `shared_file.txt` and saved on disk. Type `:show` to display the file, `:clear` to empty it, or `q` to release the lock.
- **READ mode:** the file's actual contents are displayed when access is granted. Type `r` to read it again, or `q` to release the lock. READ mode cannot change or clear the file.
- Data remains in `shared_file.txt` after clients exit and can be read by later clients.

Do not run the server or clients from different working directories: the lock server is centralized, but the file is a local shared path relative to the directory where each client is launched.

## Check the log

```bash
./dlock_checker logs/dlock.log
```

The server retains Lamport clock logging, heartbeats, lease recovery, and the strict FIFO exclusive queue.
