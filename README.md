[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/vSC6zVx5)
[![Open in Codespaces](https://classroom.github.com/assets/launch-codespace-2972f46106e565e64193e422d61a12cf1da4916b45550586e14ef0a7c637dd04.svg)](https://classroom.github.com/open-in-codespaces?assignment_repo_id=21385581)


# CS5115 PA6 — Client–Server Communication using FIFOs + fork()

## Chosen IPC Approach
This assignment uses **UNIX Named Pipes (FIFOs)** for interprocess communication.

- Server creates a well-known FIFO at `/tmp/arith_req_fifo`.
- Each client creates its own FIFO `/tmp/arith_resp_<PID>.fifo` for receiving the result.
- Server uses `fork()` to create a child process for each request. The child computes the arithmetic result and writes response back to the client’s FIFO.
- The parent server continues reading future client requests without blocking.

This demonstrates:
- creating processes using `fork()`
- IPC using Named Pipes (FIFOs)
- request–response protocol design
- synchronization and structured binary message passing

---

## Compilation Steps

### Using Makefile
```bash
make

## Assumptions and Limitations

Assumes same host environment (FIFOs are local IPC, not network).

Client and server must run in same OS namespace (Linux/macOS/WSL).

Binary struct data assumes same architecture and ABI.

Integer math is 64-bit signed; overflow not checked.

If permissions block writing: chmod 666 /tmp/arith_req_fifo.

Stale FIFO files may remain after crash.

## Chosen IPC Approach: 
UNIX Named Pipes (FIFOs) created using mkfifo().
