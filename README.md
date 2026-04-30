# Multi-Container Runtime 

## 🔹 Team Information

* **Vishal H** — PES1UG24AM322
* **Ayush Bastawad** — PES1UG24AM348

---

## 🔹 Overview

This project implements a lightweight container runtime in C using Linux system primitives. It demonstrates key operating system concepts such as process isolation, scheduling, logging, and kernel interaction in a practical environment.

###  Key Features

* Multi-container execution using Linux namespaces
* Centralized supervisor process
* Per-container logging system
* Kernel-level monitoring using a Loadable Kernel Module (LKM)
* CPU vs I/O scheduling demonstration
* Clean lifecycle management

---

## 🔹 Build, Load, and Run Instructions

### 1. Build the Project

```bash
cd boilerplate
make
```

---

### 2. Load Kernel Module

```bash
sudo insmod monitor.ko
sudo dmesg | tail -n 20
```

---

### 3. Start Supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

---

### 4. Prepare Containers

```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

---

### 5. Start Containers

```bash
sudo ./engine start alpha ./rootfs-alpha /cpu_hog
sudo ./engine start beta ./rootfs-beta /io_pulse
```

---

### 6. Inspect Containers

```bash
sudo ./engine ps
```

---

### 7. View Logs

```bash
ls logs
cat logs/alpha.log
```

---

### 8. Stop Containers

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
```

---

## 📸 Demo with Screenshots — Kindly Refer [View Screenshots](screenshots.pdf)


### Screenshot 1 — Multi-container supervision
The project is built successfully using `make`, the kernel module is loaded with `sudo insmod monitor.ko`, and the `/dev/container_monitor` device is verified. The supervisor is then started and launches both container alpha (PID 53750) and container beta (PID 53755) under a single supervisor process.

---

### Screenshot 2 — Metadata tracking
Output of `sudo ./engine ps` showing tracked container metadata including container ID, PID, and state. Both alpha (PID 17066) and beta (PID 17071) are shown as RUNNING at the time of the ps call, confirming the supervisor is actively tracking both containers.

---

### Screenshot 3 — Bounded-buffer logging
Output of `sudo ./engine logs alpha` shows the logging pipeline operating. The supervisor terminal shows "Finished writing logs" after the logtest container (PID 19654) completes, confirming the producer-consumer bounded buffer pipeline captured and flushed the container's output to the log file.

---

### Screenshot 4 — CLI and IPC
The CLI command `sudo ./engine stop alpha` is issued from the client terminal (top). The supervisor process receives the command over the UNIX domain socket and responds with "Stopping container alpha" (bottom), demonstrating the second IPC mechanism — the control channel between CLI client and supervisor.

---

### Screenshot 5 — Soft-limit warning
`dmesg` output filtered for `container_monitor` shows soft-limit warning events. For example: `SOFT LIMIT container=beta pid=17071 rss=67764224 limit=67108864` — the kernel module detected that beta's RSS exceeded its soft threshold of 64 MiB and logged a warning.

---

### Screenshot 6 — Hard-limit enforcement
The same `dmesg` output shows hard-limit enforcement. `HARD LIMIT container=alpha pid=17066 rss=84410368 limit=83886080` — the kernel module sent SIGKILL to alpha after its RSS exceeded the hard limit of 80 MiB. Similarly for beta: `HARD LIMIT container=beta pid=17071 rss=101318656 limit=100663296`. The memtest container (PID 23135) also shows both soft and hard limit events with a 10 MiB soft / 20 MiB hard configuration.

---

### Screenshot 7 — Scheduling experiment
Two CPU-bound workloads run using `cpu_hog`: one at normal priority (`time sudo ./engine run cpu_normal ... /cpu_hog`) and one at lower priority (`time nice -n 19 sudo ./engine run cpu_low ... /cpu_hog`). The normal priority run completes in 0.031s real time while the lower priority run takes 0.035s real time, showing the effect of the CFS scheduler weighting. The supervisor terminal confirms both containers were tracked and completed.

---

### Screenshot 8 — Clean teardown
After stopping all containers and running `sudo rmmod monitor` and `sudo pkill -f engine`, the final `ps aux | grep engine` shows only the grep process itself — no engine supervisor or container processes remain. This confirms all containers were reaped, logging threads exited cleanly, and no zombie processes remain.


---

## 🔹 Engineering Analysis

### 🔹 Process Isolation

* Containers use Linux namespaces
* Independent process trees
* Isolated execution environments

---

### 🔹 Supervisor Design

* Central controller for container lifecycle
* Manages process creation and termination
* Coordinates logging and execution

---

### 🔹 Logging System

* Each container writes to its own log file
* Output captured using pipes
* Logs persist after container termination

---

### 🔹 Kernel Monitoring

* Implemented as a Loadable Kernel Module (LKM)
* Demonstrates kernel-user interaction
* Tracks container-related activity

---

### 🔹 Scheduling Behavior

* CPU-bound processes utilize high CPU
* I/O-bound processes yield CPU frequently
* Demonstrates Linux scheduling behavior

---

## 🔹 Design Decisions & Tradeoffs

### Container Isolation

* **Approach:** Namespace-based
* **Tradeoff:** Lightweight but less secure than full container runtimes

### Supervisor

* **Approach:** Single controller process
* **Tradeoff:** Easy to manage but introduces a single point of failure

### Logging

* **Approach:** File-based logging
* **Tradeoff:** Simple but not highly scalable

### Kernel Monitoring

* **Approach:** LKM-based tracking
* **Tradeoff:** Powerful but increases system complexity

---

## 🔹 Observations

* CPU-intensive processes dominate CPU usage
* Logging system works consistently
* Containers start and stop correctly
* Kernel module loads successfully

---

## 🔹 Notes

* All screenshots captured from a Linux VM environment
* Some containers may exit quickly depending on workload

---

## 🔹 Conclusion

This project demonstrates a functional container runtime integrating:

* Process isolation
* Logging pipeline
* Kernel interaction
* Scheduling behavior

It provides practical insight into how operating systems manage processes, resources, and execution environments.
