# MAGIA v3 – GVSoC Virtual Platform

MAGIA v3 is a **tile-based RISC-V manycore architecture** modeled in **GVSoC**.  
It extends MAGIA v2 with a **PULP multi-core cluster** per tile and targets scalable AI / HPC workloads.  
It provides a realistic simulation of:

- A 2D mesh of compute tiles
- Local L1 scratchpads (TCDM)
- A shared L2 memory accessed through a NoC
- Hardware accelerators (RedMulE)
- DMA engines (iDMA)
- A hierarchical **Fractal Synchronization Network**
- Optional **Snitch + Spatz** vector cores per tile
- Optional **PULP multi-core cluster** per tile (up to 8 RISC-V cores)
- Optional **PCIe VFIO bridge** for QEMU co-simulation

The platform is fully **memory-mapped**, configurable at runtime, and designed to be easily extended.

---

## Prerequisites

The following are assumed to be **already installed and working**:

- GVSoC
- A micromamba environment with **Python ≥ 3.12**
- All GVSoC dependencies correctly set up

If the PCIe VFIO bridge is enabled (`ENABLE_PCIE_VFIO = True` in `arch.py`), you additionally need **libvfio-user** — see the [PCIe VFIO Bridge section](#pcie-vfio-bridge-mode-enable_pcie_vfio) below.

This README focuses only on **MAGIA v3 usage and architecture**.

---

## Build the MAGIA v3 Platform

```bash
make build TARGETS=magia_v3
```

This builds the MAGIA v3 virtual platform and installs the `gvrun` executable under:

```
./install/bin/gvrun
```

---

## SDK

The companion software SDK for MAGIA v3 is available at:

**https://github.com/FondazioneChipsIT/magia-sdk/tree/lz/magia_v3/pulp_on_magia**

### Building the SDK

Build from the SDK root directory. Choose the configuration that matches your simulation:

**Spatz + PULP** (full configuration):
```bash
make clean build \
  target_platform=magia_v3 \
  tiles=4 \
  LLVM_INSTALL_DIR=/home/gvsoc/Documents/toolchain/llvm/ \
  pulp_cores=8
```

**PULP only** (no Spatz tests):
```bash
make clean build \
  target_platform=magia_v3 \
  tiles=4 \
  pulp_cores=8 \
  spatz_tests=0
```

**Spatz only** (no PULP tests):
```bash
make clean build \
  target_platform=magia_v3 \
  tiles=4 \
  LLVM_INSTALL_DIR=/home/gvsoc/Documents/toolchain/llvm/
```

SDK build parameters:

- **`target_platform`** — must be `magia_v3`
- **`tiles`** — number of tiles per dimension (e.g. `4` → 4×4 grid)
- **`pulp_cores`** — number of PULP cores in the cluster (default: 8)
- **`LLVM_INSTALL_DIR`** — path to LLVM/Clang toolchain, required for Spatz compilation
- **`spatz_tests=0`** — disable Spatz test compilation

---

## Running a Simulation (Base Configuration)

```bash
./install/bin/gvrun \
  --target magia_v3 \
  --work-dir /home/gvsoc/Documents/test \
  --param binary=/home/gvsoc/Documents/chips-magia-sdk/build/bin/<test_name> \
  run \
  --attr magia_v3/n_tiles_x=4 \
  --attr magia_v3/n_tiles_y=4
```

Command-line parameters:

- **`--work-dir`** — directory where GVSoC writes simulation outputs
- **`--param binary=...`** — path to the ELF binary to be executed (CV32 binary)
- **`--attr magia_v3/n_tiles_x`** — number of tiles in X dimension
- **`--attr magia_v3/n_tiles_y`** — number of tiles in Y dimension

The total number of tiles is:

```
NB_CLUSTERS = n_tiles_x × n_tiles_y
```

---

## Running with PULP Cluster Enabled

When the binary includes a PULP workload, pass the number of PULP cores via `nb_pulp_cores`:

```bash
./install/bin/gvrun \
  --target magia_v3 \
  --work-dir /home/gvsoc/Documents/test \
  --param binary=/home/gvsoc/Documents/chips-magia-sdk/build/bin/hello_pulp \
  run \
  --attr magia_v3/n_tiles_x=1 \
  --attr magia_v3/n_tiles_y=1 \
  --attr magia_v3/nb_pulp_cores=8
```

- **`--attr magia_v3/nb_pulp_cores`** — number of PULP cores to wait for at completion; must match the `pulp_cores` value used at SDK build time

---

## Running with Snitch + Spatz Enabled

If **Spatz** is enabled, provide the Spatz boot ROM:

```bash
./install/bin/gvrun \
  --target magia_v3 \
  --work-dir /home/gvsoc/Documents/test \
  --param binary=/home/gvsoc/Documents/chips-magia-sdk/build/bin/<spatz_test> \
  run \
  --attr magia_v3/n_tiles_x=4 \
  --attr magia_v3/n_tiles_y=4 \
  --attr magia_v3/nb_pulp_cores=8 \
  --attr magia_v3/spatz_romfile=/home/gvsoc/Documents/toolchain/spatz/bootrom/spatz_init.bin
```

- **`--attr magia_v3/spatz_romfile`** — path to the Snitch-Spatz boot ROM binary

---

## Enabling Trace Output

Add `--trace-level=trace` and optionally filter by component:

```bash
./install/bin/gvrun \
  --target magia_v3 \
  --work-dir /home/gvsoc/Documents/test \
  --param binary=/home/gvsoc/Documents/chips-magia-sdk/build/bin/hello_pulp \
  --trace-level=trace \
  run \
  --attr magia_v3/n_tiles_x=1 \
  --attr magia_v3/n_tiles_y=1 \
  --attr magia_v3/nb_pulp_cores=8 \
  --trace=tile-0-cluster-regs
```

---

## PCIe VFIO Bridge Mode (`ENABLE_PCIE_VFIO`)

When `ENABLE_PCIE_VFIO = True` is set in `arch.py`, MAGIA v3 exposes its L2 memory to an external **QEMU** virtual machine as a **PCIe endpoint** via the `vfio-user` protocol.

```
+-------------------+        vfio-user socket         +------------------+
|      GVSoC        |  <---------------------------->  |      QEMU        |
|                   |                                  |                  |
|  PCIe endpoint    |                                  |  PCIe root port  |
|  (vfio-user)      |                                  |  guest VM        |
+-------------------+                                  +------------------+
```

This enables a guest OS running inside QEMU to drive DMA transfers to and from the MAGIA v3 L2 memory, load ELF binaries through the PCIe BAR, and control accelerator startup — all without modifying the GVSoC model itself.

In this mode:
- The ELF binary is **not** loaded via `--param binary`. The guest software is responsible for loading and starting the accelerator.
- The `KillModule` fires a `done_irq` signal to the bridge when all tiles have completed, instead of calling `quit()` directly.
- The bridge receives the `done_irq`, forces `fetch_en` low, and triggers a full GVSoC reset.

### Enabling the Bridge

In `pulp/pulp/chips/magia_v3/arch.py`:

```python
ENABLE_PCIE_VFIO = True   # default: False
```

Then rebuild:

```bash
make build TARGETS=magia_v3
```

### Running GVSoC in VFIO Bridge Mode

```bash
./install/bin/gvrun \
  --target magia_v3 \
  --work-dir /home/gvsoc/Documents/test \
  run \
  --attr magia_v3/n_tiles_x=4 \
  --attr magia_v3/n_tiles_y=4 \
  --attr magia_v3/nb_pulp_cores=8
```

GVSoC will start and **block** waiting for QEMU to connect on `/tmp/gvsoc.sock`.

### Dependencies: libvfio-user

```bash
git clone https://github.com/nutanix/libvfio-user
cd libvfio-user
meson build
ninja -C build
sudo ninja -C build install
```

Expected install paths:

```
/usr/local/include/vfio-user/
/usr/local/lib/x86_64-linux-gnu/
```

### QEMU Command Line

The QEMU build must support the `vfio-user-pci` device. Launch QEMU from the QEMU build directory:

```bash
./build/qemu-system-x86_64 \
  -object memory-backend-memfd,id=mem,size=2G,share=on \
  -machine q35,memory-backend=mem \
  -nodefaults \
  -display none \
  -serial none \
  -monitor tcp:127.0.0.1:45454,server,nowait \
  -drive id=hd0,file=/home/gvsoc/Documents/toolchain/qemu-img/debian-12-nocloud-amd64.qcow2,format=qcow2,if=none \
  -device virtio-blk-pci,drive=hd0 \
  -device e1000,netdev=net0 \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 \
  -device pcie-root-port,id=rp1 \
  --device '{"driver":"vfio-user-pci","socket":{"type":"unix","path":"/tmp/gvsoc.sock"}}'
```

> **Note:** `gvsoc.sock` must match `socket_path` configured in `soc.py` for the bridge instance.

### Guest VM

The guest VM image is a **stock Debian 12 (Bookworm)** cloud image (no-cloud variant).  
Download `debian-12-nocloud-amd64.qcow2` from:

https://cloud.debian.org/images/cloud/bookworm/latest/

To connect to the running guest over SSH (forwarded to host port 2222):

```bash
ssh -p 2222 root@127.0.0.1
```

### Host-side Test Environment

The host-side software stack (kernel module, DMA test, ELF loader) is available at:

https://github.com/TheSSDGuy/gvsoc-vfio-test

---

## High-Level Architecture Overview

MAGIA v3 is organized in three main layers:

```
+---------------------------+
|        Board Layer        |
|     (magia_v3_board)      |
+---------------------------+
|         SoC Layer         |
|       (MagiaV3Soc)        |
+---------------------------+
|        Tile Layer         |
|      (MagiaV3Tile)        |
+---------------------------+
```

---

## Board Layer (`MagiaV3Board`)

The **board** is the GVSoC entry point:

- Declares runtime parameters (e.g. `binary`)
- Instantiates the SoC
- Connects GVSoC runner logic to the model

This is the component bound to `--target magia_v3`.

---

## SoC Layer (`MagiaV3Soc`)

The SoC is responsible for:

- Creating the **tile mesh**
- Instantiating **L2 memory**
- Building the **2D NoC (FlooNoC)**
- Connecting tiles to memory and NoC
- Instantiating the **Fractal Synchronization Tree**
- Managing simulation termination via `KillModule`
- Optionally instantiating the **PCIe VFIO bridge** and connecting it to L2

### Tile Placement

Tiles are arranged in a row-major 2D grid:

```
X →
0   1   2   3
4   5   6   7
8   9  10  11
12 13  14  15
↓
Y
```

Each tile has:
- A private L1 address space
- Access to remote L1s and shared L2 via NoC

---

## Tile Architecture (`MagiaV3Tile`)

Each tile is a **self-contained compute cluster** composed of:

### Compute Cores
- **CV32** RISC-V core (always present) — acts as the tile controller
- Optional **Snitch + Spatz** vector core
- Optional **PULP cluster** — up to 8 RISC-V cores sharing a local L1

### Local Memory
- **TCDM (L1 scratchpad)**
  - 32 banks
  - Multi-ported via interleavers
  - Shared by cores, DMA, accelerators, and PULP cluster

### Accelerators
- **RedMulE** (matrix / tensor engine)
- Memory-mapped control interface

### DMA Engines
- Two **Snitch DMA** engines per tile
- Controlled through a memory-mapped iDMA controller
- Support local and remote transfers

### Synchronization
- **Fractal Sync MM Controller**
- Hardware synchronization via a hierarchical fractal tree
- Neighbor and multi-level synchronization supported

### Interconnect
- OBI crossbar (control / local memory)
- AXI crossbar (remote accesses, L2)
- Narrow+Wide NoC channels

### Event & Debug
- Event Unit (interrupt routing, including PULP done IRQ)
- UART (stdout)
- GDB server support

---

## PULP Cluster

### Overview

Each tile can optionally host a **PULP multi-core cluster**: up to 8 RISC-V cores sharing the tile L1 TCDM.

The **CV32** core acts as the cluster controller:
1. Embeds the PULP binary in its own ELF (via a dedicated linker section)
2. Writes the PULP binary entry point address to the cluster control register
3. Enables the PULP cluster clock
4. Waits for a completion interrupt (PULP done IRQ)

The PULP cores are booted directly from the address written to `PULP_BINARY`, without any separate ELF loader.

### PULP Cluster Control Registers

The cluster control register block (`ClusterRegs`) is memory-mapped at **`PULP_CTRL_BASE = 0x1740`** (tile-relative).

#### Spatz sub-block — offsets `[0x00, 0x18]`

- **`0x00` `SPATZ_CLK_EN`** (R/W) — write `1` to enable Snitch+Spatz clock, `0` to disable
- **`0x04` `SPATZ_READY`** (R/W) — Snitch+Spatz ready status
- **`0x08` `SPATZ_START`** (R/W) — write `1` to assert start IRQ to Spatz
- **`0x0C` `SPATZ_TASKBIN`** (R/W) — task binary descriptor for Spatz
- **`0x10` `SPATZ_DATA`** (R/W) — data descriptor for Spatz
- **`0x14` `SPATZ_RETURN`** (R/W) — return value from Spatz task
- **`0x18` `SPATZ_DONE`** (W) — write `1` when Spatz is done; fires `spatz_done_irq`

#### PULP sub-block — offsets `[0x40, 0x4C]`

- **`0x40` `PULP_CLK_EN`** (R/W) — one-hot bitmask; bit N set to `1` enables PULP core N independently
- **`0x44` `PULP_BINARY`** (R/W) — PULP binary entry point; CV32 writes `_pulp_binary_start` here before enabling the clock
- **`0x48` `PULP_NB_CORES_TO_WAIT`** (R/W) — number of PULP harts CV32 expects to wait for; written by firmware before enabling cores
- **`0x4C` `PULP_DONE`** (W) — each PULP hart writes `1` here on completion; fires `pulp_done_irq` after `PULP_NB_CORES_TO_WAIT` writes received

### PULP Binary Delivery

The PULP binary is **embedded inside the CV32 ELF** rather than being loaded from a separate file:

1. The PULP task is compiled independently as **position-independent code (PIC)**
2. The raw binary is converted to a C array header (`<test_name>_bin.h`) and placed in the `.pulp_binary` linker section of the CV32 ELF
3. The `ctrl_core_loader` loads the entire CV32 ELF (including the embedded PULP binary) into instruction RAM at simulation start
4. At runtime, CV32 writes the `_pulp_binary_start` symbol address to `PULP_BINARY` (`0x1744`)
5. `ClusterRegs` captures this address and drives the `pulp_entry` wire (`wire<uint64_t>`) to all PULP cores
6. CV32 writes the number of cores to wait for to `PULP_NB_CORES_TO_WAIT` (`0x1748`)
7. CV32 writes a one-hot bitmask to `PULP_CLK_EN` (`0x1740`) — only the cores with their bit set begin fetching from `pulp_entry`
8. Each active PULP core writes `1` to `PULP_DONE` (`0x174C`) on completion
9. After `PULP_NB_CORES_TO_WAIT` writes are received, `ClusterRegs` fires `pulp_done_irq` to the CV32 Event Unit

### GVSoC Ports (`ClusterRegs`)

- **`input`** (slave, IO) — MMIO register access
- **`spatz_clock_en`** (master, `wire<bool>`) — drives Spatz clock enable
- **`spatz_start_irq`** (master, `wire<bool>`) — asserts Spatz start interrupt
- **`spatz_done_irq`** (master, `wire<bool>`) — pulses when Spatz done
- **`pulp_clock_en_0` … `pulp_clock_en_N-1`** (master, `wire<bool>`) — one port per PULP core; each driven by the corresponding bit of the `PULP_CLK_EN` bitmask
- **`pulp_done_irq`** (master, `wire<bool>`) — pulses when all waited PULP cores have written to `PULP_DONE`
- **`pulp_entry`** (master, `wire<uint64_t>`) — PULP binary entry point, driven from `PULP_BINARY` write

### Configuration Property

- **`nb_pulp_cores`** — number of per-core `pulp_clock_en_i` ports to allocate; must match the `nb_pulp_cores` attribute passed at simulation runtime and the `pulp_cores` value used at SDK build time

The number of cores to actually wait for is no longer a build-time constant: CV32 firmware programs it at runtime by writing to `PULP_NB_CORES_TO_WAIT` before enabling the clock, derived from the popcount of the `PULP_CLK_EN` bitmask.

---

## Fractal Synchronization Network

MAGIA v3 implements a **hierarchical fractal synchronization tree**:

- Level 0: tile-to-fractal connections
- Intermediate levels: aggregation and propagation
- Root level: global synchronization

This allows:
- Fast barrier-like synchronization
- Scalable coordination across large meshes
- Explicit modeling of sync latency and topology

---

## Memory Map (Per Tile – Simplified)

Exact addresses are defined in `arch.py`.

- **`0x0100`** RedMulE CTRL — accelerator control
- **`0x0200`** iDMA CTRL — DMA control
- **`0x0600`** FSYNC CTRL — fractal sync control
- **`0x0700`** Event Unit — interrupts and events
- **`0x1740`** Cluster CTRL — cluster control regs (Spatz + PULP)
- Stack — tile-relative, per-core stack area
- L1 (TCDM) — tile-relative, tile private scratchpad
- **`0xC000_0000`** L2 — shared memory via NoC
- **`0xFFFF_0004`** STDOUT — simulation UART
- TEST_END — simulation termination (L2 end + 1)

---

## Configuration and Extension Points

### Architecture Parameters (`arch.py`)

- **`N_TILES_X`** (default `4`) — tile grid width
- **`N_TILES_Y`** (default `4`) — tile grid height
- **`TILE_CLK_FREQ`** (default `200 MHz`) — tile clock frequency
- **`SPATZ_ENABLE`** (default `True`) — enable Snitch+Spatz vector core per tile
- **`PULP_ENABLE`** (default `True`) — enable PULP multi-core cluster per tile
- **`NB_PULP_CORES`** (default `8`) — number of PULP cores per cluster
- **`ENABLE_PCIE_VFIO`** (default `False`) — enable PCIe VFIO bridge for QEMU co-simulation

Memory sizes, latencies, and DSE parameters are defined in `MagiaArch` and `MagiaDSE`.

### Easy Extensions

You can extend MAGIA v3 by:
- Adding a new accelerator inside `MagiaV3Tile`
- Adding new MMIO controllers in `ClusterRegs`
- Changing NoC topology or parameters
- Modifying Fractal Sync behavior
- Tuning latency / bandwidth parameters

The design intentionally keeps **clear separation** between:
- Architecture description (`arch.py`)
- SoC composition (`soc.py`)
- Tile micro-architecture (`tile.py`)
- Cluster register model (`cluster_regs/`)

---

## Simulation Termination

### Standard mode (`ENABLE_PCIE_VFIO = False`)

When all tiles write to the **TEST_END** address range, the `KillModule` calls `quit()` with the exit code written by the last tile, stopping the GVSoC engine.

### VFIO bridge mode (`ENABLE_PCIE_VFIO = True`)

When all tiles write to **TEST_END**, the `KillModule` fires a `done_irq` signal to the PCIe bridge. The bridge then:
1. Forces `fetch_en` low (stops accelerator fetch)
2. Triggers a full **GVSoC reset** by asserting and releasing the top-level reset hierarchy

This allows the QEMU guest to observe completion through the BAR0 status bits and MSI-X, and to restart a new run without restarting the simulation.

---

## Source Layout (relevant files)

```
pulp/pulp/chips/magia_v3/
├── arch.py                        # Architecture constants and parameters
├── board.py                       # GVSoC board entry point
├── soc.py                         # SoC composition (tiles, NoC, L2, bridge)
├── tile.py                        # Tile micro-architecture
├── cluster_regs/                  # Cluster control registers (Spatz + PULP)
│   ├── cluster_regs.cpp           # C++ model implementation
│   └── cluster_regs.py            # Python systree binding
├── fractal_sync/                  # Fractal synchronization module
├── kill_module/                   # Simulation termination module
│   ├── kill_module.py
│   └── kill_module.cpp
└── README.md

pulp/pulp/pcie_vfio_bridge/
├── CMakeLists.txt
├── pcie_vfio_mem_bridge.cpp       # C++ model implementation
└── pcie_vfio_mem_bridge.py        # Python systree binding
```

---

## Summary

MAGIA v3 is a **scalable, realistic, and extensible** GVSoC platform designed for:

- Research on tiled AI architectures with heterogeneous compute
- Memory hierarchy exploration
- Synchronization mechanisms
- Accelerator / DMA co-design
- PULP multi-core cluster offload from a CV32 controller
- Optional Snitch+Spatz vector processing
- QEMU co-simulation via PCIe VFIO bridge

It trades simplicity for **explicitness**: everything is visible, configurable, and hackable.

Happy hacking
