# Multi-Container Runtime

A lightweight Linux container runtime implemented in C with a long-running supervisor and a kernel-space memory monitor.

---

## Team Details

Shreya Bhat (SRN: PES2UG24CS483)

Shreya Bonal (SRN: PES2UG24CS485)

Course: UE24CS242B – Operating Systems

PES University

---

## Features

- Multi-container execution using process isolation
- Supervisor process to manage containers
- CLI support (start, stop, ps, logs)
- Kernel module for memory monitoring
- Soft and hard memory limits enforcement
- Logging system for container output
- Scheduling experiments using CPU and I/O workloads

---

## Build Instructions

cd boilerplate

make

---

## Running the System

Start Supervisor

sudo ./engine supervisor ./rootfs-base

Start Container

sudo ./engine start alpha ./rootfs-alpha "/bin/sh -c 'echo hello; sleep 20'"

List Containers

sudo ./engine ps

View Logs

sudo ./engine logs alpha

Stop Container

sudo ./engine stop alpha

---

## Screenshots

### Supervisor Initialization
![img](https://github.com/user-attachments/assets/51a5f742-a850-494b-b9af-17f2f8e7bb8e)

### Starting Multiple Containers
![img](https://github.com/user-attachments/assets/2cebeca1-388f-4b7f-92da-43ab3b817c77)

### Container Status (ps Command)
![img](https://github.com/user-attachments/assets/29ed130e-7be9-4869-8441-d78179f7acc2)

### Container Logs Output
![img](https://github.com/user-attachments/assets/1fa2fc44-fabf-45a3-8edc-e48583ca1e34)

### Starting Container with Memory Limits
![img](https://github.com/user-attachments/assets/1f7ca85c-a5b4-42b3-ba32-b3f1d29331da)

### Hard Memory Limit Enforcement
![img](https://github.com/user-attachments/assets/c9e37826-e994-4a73-90d3-4e769780a198)

### Scheduling Experiment with Workloads
![img](https://github.com/user-attachments/assets/099ce52b-6d96-4bcb-8bbf-97c18129dc55)

### CLI Command Execution and Container Creation
![img](https://github.com/user-attachments/assets/3ea5d579-5d53-4cf2-9328-d5926c2a8cf1)

### Cleanup and Process Termination
![img](https://github.com/user-attachments/assets/c8e9e3d8-3a52-4522-a750-7562358b8b73)

---

## Scheduling Analysis

We tested CPU-bound (cpu_hog) and I/O-bound (io_pulse) workloads to observe scheduler behavior. CPU-intensive processes consumed more CPU time, while I/O-bound processes frequently yielded the CPU, improving responsiveness. When both workloads ran together, the scheduler balanced execution by allowing I/O tasks to proceed during CPU wait times. This demonstrated fair scheduling and efficient CPU utilization. The experiment showed how process behavior impacts scheduling decisions and system performance.

---

## Logging Mechanism

The logging system is implemented using buffered I/O in user space. Each container's output is captured and stored separately. A supervisor thread continuously monitors and collects logs from running containers. This ensures that logs are not lost and can be accessed later using the logs command. The buffering mechanism improves performance by reducing frequent disk writes.

---

## IPC Mechanism

Inter-process communication is implemented between the CLI and the supervisor using system calls. The CLI sends commands such as start, stop, and logs to the supervisor process, which executes them. The supervisor maintains container state and responds accordingly. This design ensures separation of control and execution, improving modularity and reliability.

---

## Design Decisions

- Used process isolation instead of full virtualization for lightweight execution
- Implemented a supervisor model for centralized control
- Kernel module used for accurate memory tracking
- Simple CLI for ease of use
- Modular design separating runtime, monitor, and workloads

---

## GitHub Repository

https://github.com/Shreya-Bhat-2006/OS-Jackfruit

---

## Notes

- Root filesystem directories (rootfs-*, alpha, beta) are excluded from the repository
- Requires Ubuntu VM (not WSL)
- Secure Boot must be disabled for kernel module
