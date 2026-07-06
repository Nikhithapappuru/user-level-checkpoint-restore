# User-Level Checkpoint Restore in C

This project is a Linux fault-tolerant process management system implemented in `C` with a systems-oriented stack: `ptrace`, `/proc`, `process_vm_readv`, `mmap`, `timerfd`, `fork/exec`, `waitpid`, `kill`, and other low-level Linux system calls. The goal is to periodically checkpoint a running user-space process, detect failure, and restore the latest saved execution state for a demoable managed workload.

## Tech stack

- `C` for all implementation code
- `ptrace` for process stop-and-inspect register capture
- `/proc/<pid>/maps`, `/proc/<pid>/fd`, `/proc/<pid>/fdinfo`, `/proc/<pid>/exe`, `/proc/<pid>/cwd` for process introspection
- `process_vm_readv` for memory dumping
- `mmap` and `mprotect` for rebuilding managed memory regions during restore
- `timerfd` for periodic checkpoint scheduling
- rolling A/B checkpoint slots with `rename()`-based atomic activation
- `fork`, `execv`, `waitpid`, `kill`, `open`, `readlink`, `dup2`, `setsid` for lifecycle management

## What is implemented

- A C-based manager CLI: `init`, `start`, `checkpoint`, `monitor`, `restore`, `status`, `fail`, `stop`
- Periodic checkpointing with retention-aware checkpoint storage
- Rolling A/B checkpoint strategy with `slot_A`, `slot_B`, and `active.meta`
- Failure detection by monitoring the managed PID from `/proc`
- Automatic restore from the newest checkpoint
- Snapshot image format that stores CPU registers from `ptrace`, memory map metadata from `/proc/<pid>/maps`, region bytes from `process_vm_readv`, file descriptor metadata from `/proc/<pid>/fd` and `fdinfo`, and executable/cwd information
- A demo workload in C that keeps live state in a fixed `mmap` region so restore is easy to demonstrate
- Dynamic checkpoint scheduling that becomes more aggressive as runtime grows
- Demo script and runtime files for presentation

## Important scope note

This repository is a serious C/Linux checkpointing project, but it is not a full CRIU replacement. The current restore path is designed for managed single-process demo workloads that keep their critical live state in known `mmap` regions. That keeps the project technically strong, understandable, and demoable while still using the Linux kernel interfaces your professor will expect to see.

The current storage model uses two rolling checkpoint slots instead of an ever-growing history. Each new checkpoint writes into the inactive slot, finishes via temporary-file writes plus atomic `rename()`, and then updates `active.meta` so recovery always points to the last fully completed checkpoint.

## Build

```bash
make clean
make
```

## Manual run

```bash
sudo ./bin/ulcr_manager init configs/demo.conf
sudo ./bin/ulcr_manager start configs/demo.conf
sudo ./bin/ulcr_manager monitor configs/demo.conf
```

In a second terminal:

```bash
sudo ./bin/ulcr_manager status configs/demo.conf
tail -f runtime/processes/counter-demo/process.log
cat runtime/checkpoints/counter-demo/active.meta
```

Manual checkpoint:

```bash
sudo ./bin/ulcr_manager checkpoint configs/demo.conf
```

Simulate crash and restore:

```bash
sudo ./bin/ulcr_manager fail configs/demo.conf
sleep 8
sudo ./bin/ulcr_manager status configs/demo.conf
```

## One-command demo

```bash
sudo bash ./scripts/demo.sh
```

## Project structure

```text
.
|-- configs/demo.conf
|-- demo/counter_demo.c
|-- include/ulcr/
|-- src/
|   |-- common.c
|   |-- config.c
|   |-- manager.c
|   |-- procfs.c
|   |-- runtime.c
|   |-- snapshot.c
|   `-- main.c
|-- scripts/
|   `-- demo.sh
|-- Makefile
`-- README.md
```
