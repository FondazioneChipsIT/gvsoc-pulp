# MAGIA v4 – GVSoC Virtual Platform

MAGIA v4 is a **tile-based RISC-V manycore architecture** modeled in **GVSoC**.  
It extends MAGIA v3: **every core** — the control core and each PULP cluster core — now reaches L1
directly through its own dedicated HCI port instead of the OBI crossbar, and the cluster owns a
**second, private Event Unit** with the pulp-sdk-style fork/barrier/mutex programming model.  
It provides a realistic simulation of:

- A 2D mesh of compute tiles
- Local L1 scratchpads (TCDM)
- A shared L2 memory accessed through a NoC
- Hardware accelerators (RedMulE)
- DMA engines (iDMA)
- A hierarchical **Fractal Synchronization Network**
- Optional **Snitch + Spatz** vector cores per tile
- Optional **PULP multi-core cluster** per tile (up to 8 RISC-V cores) with its own Event Unit
- Optional **PCIe VFIO bridge** for QEMU co-simulation

The platform is fully **memory-mapped**, configurable at runtime, and designed to be easily extended.

---

## Prerequisites

The following are assumed to be **already installed and working**:

- GVSoC
- A micromamba environment with **Python ≥ 3.12**
- All GVSoC dependencies correctly set up
- A clone of the **MAGIA RTL repository** (branch `lb/ClusterEventUnit` or later) — it provides the
  bare-metal test software, since there is no SDK for v4 yet (see [Test Software](#test-software))
- The **CoreV GCC toolchain**, providing `riscv64-unknown-elf-gcc`, to compile that software

Paths in this README are written as environment variables, so substitute your own:

| Variable | Meaning |
|----------|---------|
| `$GVSOC_ROOT` | this gvsoc checkout (where `install/bin/gvrun` ends up) |
| `$MAGIA_ROOT` | the MAGIA RTL repository clone |
| `$CV32_TOOLCHAIN` | `bin/` directory of the CoreV GCC toolchain |
| `$WORK_DIR` | scratch directory for simulation outputs |
| `$SPATZ_BOOTROM` | directory holding `spatz_init.bin` (Spatz tests only) |
| `$QEMU_IMG` | directory holding the guest disk image (VFIO bridge only) |

If the PCIe VFIO bridge is enabled (`ENABLE_PCIE_VFIO = True` in `arch.py`), you additionally need **libvfio-user** — see the [PCIe VFIO Bridge section](#pcie-vfio-bridge-mode-enable_pcie_vfio) below.

This README focuses only on **MAGIA v4 usage and architecture**.

---

## Build the MAGIA v4 Platform

The mesh size (`n_tiles_x`, `n_tiles_y`) and the number of PULP cores per tile
(`nb_pulp_cores`) are **target parameters**: they are part of the target *name*
and are baked into the compiled platform tree at build time. Pass them after the
target name, `key=value` separated by commas:

```bash
make build TARGETS="magia_v4:n_tiles_x=4,n_tiles_y=4,nb_pulp_cores=8"
```

Defaults (`4×4`, `nb_pulp_cores=8`) are used if omitted, i.e. `TARGETS=magia_v4` — and 4x4 is
also what the MAGIA bare-metal tests expect (see [Mesh size](#mesh-size)).
Several meshes can be built side by side (`;`-separated):

```bash
make build TARGETS="magia_v4:n_tiles_x=4,n_tiles_y=4,nb_pulp_cores=8;magia_v4:n_tiles_x=2,n_tiles_y=2,nb_pulp_cores=8"
```

This installs the `gvrun` executable under `./install/bin/gvrun`.

> **Important — build and run must use the SAME target string.**
> gvsoc compiles the whole component tree (its shape *and* every model's typed
> config) into `libplatform_tree_magia_v4.so` and checks it against the tree it
> rebuilds at run time. Reshaping the tree from a run-time `--attr` invalidates
> that library, drops to the JSON fallback (which cannot carry the typed io_v2 /
> iss_v2 configs), and the simulation dies with `std::bad_alloc`. So the mesh /
> core count live in the target name, not in `--attr`: pass the *identical*
> `--target=magia_v4:n_tiles_x=..,n_tiles_y=..,nb_pulp_cores=..` at run time.
> `--attr magia_v4/n_tiles_x=...` is now rejected on purpose.
> Only `spatz_romfile` stays a run-time `--attr` (it is a path overlaid at run
> time, never baked into the tree).

---

## Test Software

> **There is no magia-sdk port for MAGIA v4 yet.** The v3 SDK flow does not know about the new
> cluster Event Unit or the reworked `PULP_CTRL` register map, so it cannot be used here.
> Until an SDK lands, the software to run on this platform is the **bare-metal test suite of the
> MAGIA RTL repository**, which is exactly what this model was aligned against.

```bash
export MAGIA_ROOT=/path/to/MAGIA                 # MAGIA RTL repo, branch lb/ClusterEventUnit
export CV32_TOOLCHAIN=/path/to/corev/bin         # must provide riscv64-unknown-elf-gcc
export PATH=$CV32_TOOLCHAIN:$PATH
```

### Compiling a test

From the MAGIA repo root:

```bash
cd $MAGIA_ROOT
make all test=<test_name>
```

The test is located automatically by a recursive search under `sw/tests/`, so no extra flag is
needed to distinguish plain tests from cluster ones. Cluster tests live in
`sw/tests/cluster_tests/` (`hello_pulp`, `parallel_groups`, `vector_add`, ...).

Only the toolchain is required: neither the RTL build nor the python venv of the MAGIA repo is
needed to compile the software.

The build produces, for a cluster test:

| Artifact | Path |
|----------|------|
| CV32 ELF (this is what you feed to gvrun) | `sw/tests/cluster_tests/<test>/build/verif` |
| Disassembly / objdump / itb | `sw/tests/cluster_tests/<test>/build/verif.{dump,objdump,itb}` |
| PULP task ELF (PIC, compiled separately) | `sw/kernel_pulp/bin/<test>_pulp_task_bin.elf` |
| PULP task blob as a C array | `sw/kernel_pulp/headers_bin/<test>_pulp_task_bin.h` |

The PULP task is **embedded inside the CV32 ELF**: the blob header is `#include`d by the test's
`main.c` and the linker keeps it in the `.pulp_binary` section (see
[PULP Binary Delivery](#pulp-binary-delivery)). There is a single ELF to load, no separate cluster
binary.

Use `make clean test=<test_name>` when switching tests.

> **`vector_add` needs an extra repository.** Its task sources `#include` files from PLAY
> (`PLAY/source/vector_add/...`), which used to be a submodule of the MAGIA repo and was dropped
> from `.gitmodules`, leaving an empty `PLAY/` directory. Clone it before building that test:
> `git clone https://github.com/FondazioneChipsIT/PLAY $MAGIA_ROOT/PLAY`.
> The other cluster tests only use headers from `sw/utils/` and build as-is.

### Mesh size

The MAGIA RTL and its test software are **fixed at 4x4**: `N_TILES_X`/`N_TILES_Y` are localparams
in `hw/mesh/magia_pkg.sv`, and `MESH_X_TILES`/`MESH_Y_TILES`/`PULP_HARTID_BASE` are hard-coded in
`sw/utils/magia_utils.h` and `sw/utils/magia_tile_utils.h` (no `-D` override exists). So a binary
built from that repo must be run on a **4x4** target.

To run on a smaller mesh those defines have to be edited by hand, keeping the invariant
`PULP_HARTID_BASE == 2 * MESH_X_TILES * MESH_Y_TILES` — it is the offset `cluster_core_id()` uses
to derive a core's index within its cluster from `mhartid`.

---

## Running a Simulation

Running a test is always two steps: **compile the bare-metal test in the MAGIA repo**, then point
gvrun at the CV32 ELF it produced.

```bash
# 1) compile the test (in the MAGIA RTL repo)
cd $MAGIA_ROOT
make all test=hello_pulp

# 2) run it (in the gvsoc repo)
cd $GVSOC_ROOT
./install/bin/gvrun \
  --target magia_v4:n_tiles_x=4,n_tiles_y=4,nb_pulp_cores=8 \
  --work-dir $WORK_DIR \
  --param binary=$MAGIA_ROOT/sw/tests/cluster_tests/hello_pulp/build/verif \
  run
```

Command-line parameters:

- **`--target magia_v4:...`** — target string; **must be identical to the one used at build time**.
  Carries the tree-reshaping knobs `n_tiles_x`, `n_tiles_y`, `nb_pulp_cores`
- **`--work-dir`** — directory where GVSoC writes simulation outputs
- **`--param binary=...`** — the CV32 ELF, i.e. the `build/verif` produced by `make all test=...`.
  The PULP task is embedded in it, so there is nothing else to load

Notes on matching model and software:

- **`n_tiles_x=4,n_tiles_y=4`** — the MAGIA test software is fixed at 4x4 (see
  [Mesh size](#mesh-size)); a binary from that repo will misbehave on any other shape
- **`nb_pulp_cores=8`** — must match `PULP_CORE_COUNT` in `sw/utils/magia_tile_utils.h` (8 by
  default, and the default of `sw/kernel_pulp/Makefile`). The model uses it to size the cluster
  cores, their dedicated L1 ports and the cluster Event Unit slices and barriers
- the number of tiles is `NB_CLUSTERS = n_tiles_x × n_tiles_y`

For non-cluster tests the ELF is under `sw/tests/<test>/build/verif` instead (single-source tests
such as `hello_world` are compiled the same way).

---

## Running with Snitch + Spatz Enabled

Tests that use Spatz additionally need the Spatz boot ROM, passed as a run-time attribute:

```bash
./install/bin/gvrun \
  --target magia_v4:n_tiles_x=4,n_tiles_y=4,nb_pulp_cores=8 \
  --work-dir $WORK_DIR \
  --param binary=$MAGIA_ROOT/sw/tests/cluster_tests/hello_spatz_pulp/build/verif \
  run \
  --attr magia_v4/spatz_romfile=$SPATZ_BOOTROM/spatz_init.bin
```

- **`--attr magia_v4/spatz_romfile`** — path to the Snitch-Spatz boot ROM binary. Unlike the mesh
  knobs, this stays a run-time `--attr`: it is a stim-file path overlaid at run time and never
  baked into the compiled tree

---

## Enabling Trace Output

Add `--trace-level=trace` and optionally filter by component:

```bash
./install/bin/gvrun \
  --target magia_v4:n_tiles_x=4,n_tiles_y=4,nb_pulp_cores=8 \
  --work-dir $WORK_DIR \
  --param binary=$MAGIA_ROOT/sw/tests/cluster_tests/parallel_groups/build/verif \
  --trace-level=trace \
  run \
  --trace=tile-0-cluster-regs
```

Components that are useful to trace when bringing up cluster software:

| `--trace=` | What it shows |
|------------|---------------|
| `tile-0-cluster-regs` | the `PULP_CTRL` mailbox: boot, doorbell, ACK, DONE, return value |
| `tile-0-cluster-event-unit` | the cluster Event Unit: dispatch FIFO, barriers, mutex, SW events, per-core sleep/wake |
| `tile-0-event-unit` | the tile Event Unit of the control core (cluster DONE arrives on bit 12) |
| `tile-0-pulp-cv32-core-0` | the dispatcher core (`cv.elw` park/wake) |
| `tile-0-pulp-0-demux` | one cluster core's data demux: which accesses take L1, the Event Unit or the OBI xbar |
| `tile-0-core-demux` | the control core's data demux, same three-way split |

---

## PCIe VFIO Bridge Mode (`ENABLE_PCIE_VFIO`)

When `ENABLE_PCIE_VFIO = True` is set in `arch.py`, MAGIA v4 exposes its L2 memory to an external **QEMU** virtual machine as a **PCIe endpoint** via the `vfio-user` protocol.

```
+-------------------+        vfio-user socket         +------------------+
|      GVSoC        |  <---------------------------->  |      QEMU        |
|                   |                                  |                  |
|  PCIe endpoint    |                                  |  PCIe root port  |
|  (vfio-user)      |                                  |  guest VM        |
+-------------------+                                  +------------------+
```

This enables a guest OS running inside QEMU to drive DMA transfers to and from the MAGIA v4 L2 memory, load ELF binaries through the PCIe BAR, and control accelerator startup — all without modifying the GVSoC model itself.

In this mode:
- The ELF binary is **not** loaded via `--param binary`. The guest software is responsible for loading and starting the accelerator.
- The `KillModule` fires a `done_irq` signal to the bridge when all tiles have completed, instead of calling `quit()` directly.
- The bridge receives the `done_irq`, forces `fetch_en` low, and triggers a full GVSoC reset.

### Enabling the Bridge

In `pulp/pulp/chips/magia_v4/arch.py`:

```python
ENABLE_PCIE_VFIO = True   # default: False
```

Then rebuild:

```bash
make build TARGETS=magia_v4
```

### Running GVSoC in VFIO Bridge Mode

```bash
./install/bin/gvrun \
  --target=magia_v4:n_tiles_x=4,n_tiles_y=4,nb_pulp_cores=8 \
  --work-dir $WORK_DIR \
  run
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
  -drive id=hd0,file=$QEMU_IMG/debian-12-nocloud-amd64.qcow2,format=qcow2,if=none \
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

MAGIA v4 is organized in three main layers:

```
+---------------------------+
|        Board Layer        |
|     (magia_v4_board)      |
+---------------------------+
|         SoC Layer         |
|       (MagiaV4Soc)        |
+---------------------------+
|        Tile Layer         |
|      (MagiaV4Tile)        |
+---------------------------+
```

---

## Board Layer (`MagiaV4Board`)

The **board** is the GVSoC entry point:

- Declares runtime parameters (e.g. `binary`)
- Instantiates the SoC
- Connects GVSoC runner logic to the model

This is the component bound to `--target magia_v4`.

---

## SoC Layer (`MagiaV4Soc`)

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

## Tile Architecture (`MagiaV4Tile`)

Each tile is a **self-contained compute cluster** composed of:

### Compute Cores

- **CV32CtrlCore** (`ctrl_core/`) — tile controller; always present; `irq_external=True` +
  `elw=True`, i.e. `irq_req`/`irq_ack` ports wired to the tile Event Unit for vectored event
  delivery, and `cv.elw` to sleep on it. Its data port goes through a **data demux**
  (`core_data_demux.sv`) that gives it a dedicated L1 port and a direct path to its Event Unit
- Optional **Snitch + Spatz** vector core
- Optional **PULP cluster** — up to 8 **CV32PulpCore** workers (`pulp_core/`); **same
  personality as the control core** (`irq_external=True, elw=True`), each wired to the
  *cluster* Event Unit. In RTL the EU cause is OR'd onto `irq_i[11]` with the ack tied to 0 and
  no software ever unmasks it: all synchronisation goes through the EU and `cv.elw`

### Local Memory
- **TCDM (L1 scratchpad)**
  - 32 banks
  - Multi-ported via interleavers
  - Shared by cores, DMA, accelerators, and PULP cluster
  - **One dedicated L1 port per core**, control core included: accesses to the tile L1 window go
    straight to the interleaver from the core's own data demux, bypassing the OBI crossbar. Port
    map mirrors `hci_core_if[]` in `magia_tile.sv`:

    | Port | Master |
    |------|--------|
    | `0` | OBI crossbar — external/mesh L1 traffic, plus stack and reserved from any core |
    | `1 .. nb_pulp_cores` | one per PULP cluster core |
    | last | the control core |

    Stack, reserved, remote-tile L1 and everything peripheral still take the OBI path

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
- Per-core **data demux** (`core_data_demux.sv`), one for the control core and one per cluster
  core: L1 window to the core's own TCDM port, Event Unit window to its Event Unit, everything
  else to the OBI crossbar
- OBI crossbar (peripherals, stack/reserved, remote L1, L2 gateway, external/mesh L1 traffic)
- AXI crossbar (remote accesses, L2)
- Narrow+Wide NoC channels

### Event & Debug
- **Two Event Units** (same `event_unit_flex`-derived model, different configuration):
  - tile Event Unit (`0x0700`) — 1 core, serves CV32CtrlCore; cluster DONE at `in_event_12_pe_0`
  - cluster Event Unit — `nb_pulp_cores` cores, private to the cluster; per-core demux window at
    `0x1800` (what the cores use, incl. `cv.elw`) and memory-mapped window at `0x2800`.
    Event bits follow the IP mapping: barrier 16, mutex 17, dispatch 18, SW events at `[11:4]`
    (`sw_base = 4`), dispatch doorbell from the control core at bit 13
- UART (stdout)
- GDB server support

---

## PULP Cluster

### Overview

Each tile can optionally host a **PULP multi-core cluster**: up to 8 RISC-V cores sharing the
tile L1 TCDM through dedicated ports, plus a **cluster-private Event Unit**.

The programming model is **single-dispatcher**: the control core rings *one* doorbell, cluster
core 0 takes it and may then fan the work out to the other cores itself (pulp-sdk-style
`pi_cl_team_fork`). The control core never talks to cores 1..N-1.

**Init phase** (`cluster_boot` / `pulp_init`):
1. Writes the PULP binary entry point to `PULP_BINARY`
2. Broadcasts clock enable to **all** PULP cores via `PULP_CLK_EN` (write `1`)
3. Polls `PULP_READY` until all cores have booted and armed themselves

Every core boots into `_start`, sets up its stack, installs its trap handler and unmasks
barrier/mutex/dispatch (bits 16/17/18) in its own Event Unit slice, then splits by role:
- **core 0 (dispatcher)** also unmasks the doorbell (bit 13), reports `PULP_READY`, and parks on
  `cv.elw` over `EU_CORE_EVENT_WAIT_CLEAR`
- **cores 1..N-1 (workers)** report `PULP_READY` and park on `cv.elw` over the Event Unit
  **dispatch FIFO**

**Dispatch phase** (`cluster_dispatch_task` / `pulp_run_task`):
1. Writes the task function address to `PULP_TASKBIN` and an optional data pointer to `PULP_DATA`
2. Writes non-zero to `PULP_START` — `ClusterRegs` fires a 1-cycle doorbell into **core 0's**
   Event Unit slice (`EU_OTHER_CLUSTER_START`, bit 13) and latches the request
3. Polls `PULP_START` until it clears to `0` — core 0 ACKs by writing `0` *before* calling the task
4. Waits for the DONE event on its own Event Unit (bit 12)

Core 0, once woken:
1. Reads `PULP_DATA` (first argument) and `PULP_TASKBIN` (function pointer)
2. **ACKs** by writing `0` to `PULP_START`
3. Calls the task, which may itself fork onto the other cores via the cluster Event Unit
   (dispatch FIFO + barrier)
4. Writes the exit code to `PULP_RETURN`, then `1` to `PULP_DONE`, then goes back to `cv.elw`

There is **no quorum**: one `PULP_DONE` write is one completed dispatch, and one `PULP_READY`
write is one booted core. If the task traps, the trap handler writes `mcause | 0x8000_0000` to
`PULP_RETURN` and still signals DONE, so the control core sees the crash instead of hanging.

### PULP Cluster Control Registers

The cluster control register block (`ClusterRegs`) is memory-mapped at **`CLUSTER_CTRL_BASE = 0x1700`** (tile-relative). All offsets below are from `CLUSTER_CTRL_BASE`.

#### Spatz sub-block — offsets `[0x00, 0x18]`

- **`0x00` `SPATZ_CLK_EN`** (R/W) — write `1` to enable Snitch+Spatz clock, `0` to disable
- **`0x04` `SPATZ_READY`** (R/W) — Snitch+Spatz ready status
- **`0x08` `SPATZ_START`** (R/W) — write `1` to assert start IRQ to Spatz (1-cycle pulse)
- **`0x0C` `SPATZ_TASKBIN`** (R/W) — task binary descriptor for Spatz
- **`0x10` `SPATZ_DATA`** (R/W) — data descriptor for Spatz
- **`0x14` `SPATZ_RETURN`** (R/W) — return value from Spatz task
- **`0x18` `SPATZ_DONE`** (W) — write `1` when Spatz is done; fires `spatz_done_irq`

#### PULP sub-block — offsets `[0x40, 0x5C]`

Mirrors `obi_slave_ctrl_cluster.sv`, instantiated at `TILE_CSR_START + 0x40 = 0x1740`.

- **`0x40` `PULP_CLK_EN`** (R/W) — write non-zero to broadcast clock enable to **all** PULP cores
  simultaneously, `0` to disable; a write also resets the READY counter
- **`0x44` `PULP_BINARY`** (R/W) — PULP binary entry point (boot vector) of every cluster core;
  drives the `pulp_entry` wire. Reset value `0xCC00_0080`, as in RTL
- **`0x48` `PULP_DONE`** (R/W) — the dispatcher core writes `1` when the task returns; each write
  fires the DONE event towards the tile Event Unit (`in_event_12_pe_0`). No quorum
- **`0x4C` `PULP_TASKBIN`** (R/W) — per-dispatch task function address, read by core 0
- **`0x50` `PULP_DATA`** (R/W) — opaque context pointer passed as first argument to the task
- **`0x54` `PULP_START`** (R/W) — the control core writes non-zero to ring the doorbell (1-cycle
  pulse into core 0's Event Unit slice) and latch the request; core 0 writes `0` to ACK, which
  unblocks the control core's poll
- **`0x58` `PULP_READY`** (R/W) — reads `1` once `nb_pulp_cores` cores have reported; each core
  writes `1` after boot (saturating counter)
- **`0x5C` `PULP_RETURN`** (R/W) — task exit code, written by the dispatcher right before DONE.
  Bit 31 set means the task trapped, and the low bits carry `mcause`

> Note the offsets differ from MAGIA v3: `PULP_NB_CORES_TO_WAIT` is gone (no quorum) and
> `PULP_RETURN` is new, so everything from `0x48` on has shifted.

#### Cluster Event Unit windows

- **`0x1800` – `0x27FF`** — per-core demux (`eu_direct_link`) window. Each cluster core reaches it
  through its own data demux, never through the OBI crossbar. Layout: core regs `0x00..0x3C`
  (incl. `EVENT_WAIT_CLEAR` at `0x3C` and `MASK_OR` at `0x08`), dispatch `0x80` (+ team config at
  `0x84`), mutex `0xC0`, SW events `0x100`, barriers `0x200` with stride `0x20`
- **`0x2800` – `0x37FF`** — memory-mapped (speriph) window, reachable from the OBI crossbar

### PULP Binary Delivery

The PULP binary is **embedded inside the CV32 ELF** rather than being loaded from a separate file:

1. The PULP task is compiled independently as position-independent code (PIC)
2. The raw binary is converted to a C array header and placed in the `.pulp_binary` linker section of the CV32 ELF
3. At simulation start, the `ctrl_core_loader` loads the entire CV32 ELF (including the embedded PULP binary) into instruction RAM
4. **Init phase:**
   - CV32 writes `_pulp_binary_start` to `PULP_BINARY` (`0x1744`) → drives `pulp_entry` to all cores
   - CV32 writes `1` to `PULP_CLK_EN` (`0x1740`) → all cores begin fetching from `pulp_entry`
   - All cores execute `_start`: compute local hart ID, set up per-hart stack, clear BSS, install
     the trap handler (256-byte aligned — CV32E40P hardwires `mtvec[7:2] = 0`), unmask
     barrier/mutex/dispatch in their Event Unit slice, write `1` to `PULP_READY` (`0x1758`)
   - Core 0 also unmasks the doorbell (bit 13) and parks on `cv.elw` over `EU_CORE_EVENT_WAIT_CLEAR`;
     cores 1..N-1 park on `cv.elw` over the Event Unit dispatch FIFO
   - CV32 polls `PULP_READY` until it reads `1`
5. **Dispatch phase (per `cluster_dispatch_task` call):**
   - CV32 writes task address to `PULP_TASKBIN` (`0x174C`) and data pointer to `PULP_DATA` (`0x1750`)
   - CV32 writes non-zero to `PULP_START` (`0x1754`)
   - `ClusterRegs` fires a 1-cycle doorbell into core 0's Event Unit slice; deasserts after 1 cycle
   - Core 0 wakes from `cv.elw`: reads DATA and TASKBIN → writes `0` to `PULP_START` (ACK) → calls
     the task (which may `pi_cl_team_fork` onto the other cores) → writes `PULP_RETURN` → writes
     `1` to `PULP_DONE` (`0x1748`) → back to `cv.elw`
   - `ClusterRegs`: the ACK clears `PULP_START` → CV32 exits `while (PULP_START != 0)`
   - `ClusterRegs`: the DONE write fires `pulp_done_irq` → tile Event Unit wakes the CV32 wait

### GVSoC Ports (`ClusterRegs`)

- **`input`** (slave, IO) — MMIO register access
- **`spatz_clock_en`** (master, `wire<bool>`) — drives Spatz clock enable
- **`spatz_start_irq`** (master, `wire<bool>`) — asserts Spatz start interrupt (1-cycle pulse)
- **`spatz_done_irq`** (master, `wire<bool>`) — pulses when Spatz done
- **`pulp_clock_en`** (master, `wire<bool>`) — single broadcast wire; fan-out to all PULP cores' fetch-enable simultaneously
- **`pulp_start_irq`** (master, `wire<bool>`) — **single** doorbell port; fires a 1-cycle pulse on a
  non-zero `PULP_START` write, wired to `in_event_13_pe_0` of the **cluster** Event Unit (core 0's
  slice). Replaces the per-core `pulp_start_irq_i` ports of MAGIA v3
- **`pulp_done_irq`** (master, `wire<bool>`) — pulses on every `PULP_DONE` write; wired to
  `in_event_12_pe_0` of the tile (CV32) Event Unit
- **`pulp_entry`** (master, `wire<uint64_t>`) — PULP binary entry point, driven from `PULP_BINARY` write

### Configuration Property

- **`nb_pulp_cores`** — number of PULP cores (`CV32PulpCore` instances), of dedicated L1 ports, of
  per-core demux routers and of cluster Event Unit slices/barriers to allocate; set via the
  `nb_pulp_cores` **target parameter** (same string at platform build and at run) and it must match the `pulp_cores` value at SDK build time

---

## Fractal Synchronization Network

MAGIA v4 implements a **hierarchical fractal synchronization tree**:

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
- **`0x1700`** Cluster CTRL — cluster control regs (Spatz `0x1700`, PULP `0x1740`)
- **`0x1800`** Cluster EU (demux) — per-core view of the cluster Event Unit, cluster cores only
- **`0x2800`** Cluster EU (mmap) — memory-mapped view of the cluster Event Unit
- Stack — tile-relative, per-core stack area (PULP cluster stacks included)
- L1 (TCDM) — tile-relative, tile private scratchpad
- **`0xC000_0000`** L2 — shared memory via NoC
- **`0xFFFF_0004`** STDOUT — simulation UART
- TEST_END — simulation termination (L2 end + 1)

---

## Configuration and Extension Points

### Architecture Parameters (`arch.py`)

- **`N_TILES_X`** (default `4`) — tile grid width; **default** for the `n_tiles_x` target parameter
- **`N_TILES_Y`** (default `4`) — tile grid height; **default** for the `n_tiles_y` target parameter
- **`NB_PULP_CORES`** (default `8`) — PULP cores per cluster; **default** for the `nb_pulp_cores` target parameter
- **`TILE_CLK_FREQ`** (default `200 MHz`) — tile clock frequency
- **`SPATZ_ENABLE`** (default `True`) — enable Snitch+Spatz vector core per tile
- **`PULP_ENABLE`** (default `True`) — enable PULP multi-core cluster per tile
- **`ENABLE_PCIE_VFIO`** (default `False`) — enable PCIe VFIO bridge for QEMU co-simulation

> `N_TILES_X`/`N_TILES_Y`/`NB_PULP_CORES` are only the **build-time defaults**.
> To run a different shape, override them in the target string at both build and
> run — e.g. `--target=magia_v4:n_tiles_x=2,n_tiles_y=2,nb_pulp_cores=4` — not
> with `--attr` (see the Build section for why).

Memory sizes, latencies, and DSE parameters are defined in `MagiaArch` and `MagiaDSE`.

### Easy Extensions

You can extend MAGIA v4 by:
- Adding a new accelerator inside `MagiaV4Tile`
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
pulp/pulp/chips/magia_v4/
├── arch.py                        # Architecture constants and parameters
├── board.py                       # GVSoC board entry point
├── soc.py                         # SoC composition (tiles, NoC, L2, bridge)
├── tile.py                        # Tile micro-architecture
├── cluster_regs/                  # Cluster control registers (Spatz + PULP)
│   ├── cluster_regs.cpp           # C++ model implementation
│   └── cluster_regs.py            # Python systree binding
├── ctrl_core/                     # CV32 control core (irq_external + elw, tile Event Unit)
│   ├── core.py                    # CV32CtrlCore class
│   └── hierarchical_cache.py      # Instruction cache for the control core
├── pulp_core/                     # PULP cluster cores (irq_external + elw, cluster Event Unit)
│   ├── core.py                    # CV32PulpCore class
│   └── hierarchical_cache.py      # Shared instruction cache for the cluster
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

MAGIA v4 is a **scalable, realistic, and extensible** GVSoC platform designed for:

- Research on tiled AI architectures with heterogeneous compute
- Memory hierarchy exploration
- Synchronization mechanisms
- Accelerator / DMA co-design
- PULP multi-core cluster offload from a CV32 controller, with pulp-sdk-style fork/barrier inside
  the cluster
- Optional Snitch+Spatz vector processing
- QEMU co-simulation via PCIe VFIO bridge

It trades simplicity for **explicitness**: everything is visible, configurable, and hackable.

Happy hacking
