# Copyright (C) 2025 Fondazione Chips-IT

# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at

#     http://www.apache.org/licenses/LICENSE-2.0

# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.



# Authors: Lorenzo Zuolo, Chips-IT (lorenzo.zuolo@chips.it)

from gvrun.attribute import Tree, Value
from gvrun.parameter import TargetParameter

class MagiaArch:
    # Single tile address map from magia_tile_pkg.sv
    #
    # Convention: *_SIZE is the real size of the window in bytes and *_ADDR_END
    # is its last byte, so a window covers [START, START + SIZE - 1] and the
    # next one opens at END + 1. Every START / END below holds exactly the same
    # value it always had (the previous code stored SIZE - 1 in *_SIZE and
    # computed END = START + SIZE, which lands on the same address); what
    # changes is that a mapping declared with SIZE now really reaches the last
    # byte of its window instead of stopping one byte short.
    #
    # This matters beyond tidiness: with true sizes the tile L1 windows
    # (0xE_0000 each) and the L2 window are whole, page-aligned multiples of the
    # 4 KiB AXI page, which is what lets the io_v2 NoC keep its burst-legality
    # check enabled (see soc_v2.py) — a burst can then never straddle two
    # targets. Addresses seen by software are unchanged, so the SDK's own copy
    # of the map stays valid.
    REDMULE_CTRL_ADDR_START = 0x0000_0100
    REDMULE_CTRL_SIZE       = 0x0000_0100
    REDMULE_CTRL_ADDR_END   = REDMULE_CTRL_ADDR_START + REDMULE_CTRL_SIZE - 1
    IDMA_CTRL_ADDR_START    = REDMULE_CTRL_ADDR_END + 1
    IDMA_CTRL_SIZE          = 0x0000_0400
    IDMA_CTRL_ADDR_END      = IDMA_CTRL_ADDR_START + IDMA_CTRL_SIZE - 1
    FSYNC_CTRL_ADDR_START   = IDMA_CTRL_ADDR_END + 1
    FSYNC_CTRL_SIZE         = 0x0000_0100
    FSYNC_CTRL_ADDR_END     = FSYNC_CTRL_ADDR_START + FSYNC_CTRL_SIZE - 1
    EVENT_UNIT_ADDR_START   = FSYNC_CTRL_ADDR_END + 1
    EVENT_UNIT_SIZE         = 0x0000_1000
    EVENT_UNIT_ADDR_END     = EVENT_UNIT_ADDR_START + EVENT_UNIT_SIZE - 1
    SPATZ_CTRL_START        = EVENT_UNIT_ADDR_END + 1
    SPATZ_CTRL_SIZE         = 0x0000_0100
    SPATZ_CTRL_END          = SPATZ_CTRL_START + SPATZ_CTRL_SIZE - 1
    RESERVED_ADDR_START     = SPATZ_CTRL_END + 1
    RESERVED_SIZE           = 0x0000_E800
    RESERVED_ADDR_END       = RESERVED_ADDR_START + RESERVED_SIZE - 1
    STACK_ADDR_START        = RESERVED_ADDR_END + 1
    STACK_SIZE              = 0x0001_0000
    STACK_ADDR_END          = STACK_ADDR_START + STACK_SIZE - 1
    L1_ADDR_START           = STACK_ADDR_END + 1
    L1_SIZE                 = 0x000E_0000
    L1_ADDR_END             = L1_ADDR_START + L1_SIZE - 1
    L1_TILE_OFFSET          = 0x0010_0000
    L2_ADDR_START           = 0xC000_0000
    L2_SIZE                 = 0x0CFF_0000 # here in RTL we have 0x4000_0000 but the end address (TEST_END_ADDR_START) then will fall in L2... no sense to me
    L2_ADDR_END             = L2_ADDR_START + L2_SIZE - 1
    TEST_END_ADDR_START     = L2_ADDR_END + 1
    TEST_END_SIZE           = 0x400
    STDOUT_ADDR_START       = 0xFFFF_0004
    STDOUT_SIZE             = 0x100

    # Snitch_Spatz
    SPATZ_ENABLE               = True
    SPATZ_BOOTROM_ADDR         = 0x1000_0000
    SPATZ_BOOTROM_SIZE         = 0x100
    SPATZ_ROMFILE              = ''
    USE_NEW_SPATZ              = True

    # From magia_pkg.sv
    N_MEM_BANKS         = 32        # Number of TCDM banks
    N_WORDS_BANK        = 8192      # Number of words per TCDM bank

    # Extra
    BYTES_PER_WORD      = 4
    TILE_CLK_FREQ       = 200 * (10 ** 6)
    TILE_WIDE_WIDTH     = BYTES_PER_WORD * 8

    # Default mesh, used when the target name does not customize it (see
    # MagiaTree: "magia_v2:n_tiles_x=2,n_tiles_y=2").
    N_TILES_X           = 4
    N_TILES_Y           = 4

    ENABLE_PCIE_VFIO            = False

    # Chip-wide IO protocol selection.
    #
    # False -> the whole chip is described with the legacy io (v1) models
    #          (soc.py / tile.py, router.Router, memory.Memory, ...).
    # True  -> the whole chip is described with the io_v2 models
    #          (soc_v2.py / tile_v2.py, interco.router_v2, memory.memory_v3,
    #           floonoc_v2, cache_v4, the *_v2 magia models, ...).
    #
    # There is no v1<->v2 bridge in gvsoc: the two protocols are
    # wire-incompatible, so this is deliberately an all-or-nothing switch for
    # the whole platform. board.py picks the soc description from it.
    ENABLE_IO_V2                = True

class MagiaTree(Tree):
    """Customizable knobs of the platform.

    The mesh size is a **target parameter**, i.e. it belongs to the target
    *name*, not to the run command line:

        make TARGETS="magia_v2:n_tiles_x=2,n_tiles_y=2" build
        gvrun --target=magia_v2:n_tiles_x=2,n_tiles_y=2 --param binary=<elf> run

    Same string for build and run, and several meshes can live side by side:

        make TARGETS="magia_v2:n_tiles_x=4,n_tiles_y=4;magia_v2:n_tiles_x=2,n_tiles_y=2" build

    The reason is that gvsoc compiles the whole component tree of a target
    into ``libplatform_tree_<target>.so``, baking in both the shape of the
    tree (how many tiles, which components) and every model's typed
    configuration. gvrun resolves that library from the target name and
    checks it against the systree it rebuilds at run time. Reshaping the
    tree from the run command line therefore invalidates the library, and
    the JSON fallback path cannot carry the typed configurations (a
    component only ever gets one from the compiled tree), so the io_v2
    models would come up with an uninitialized configuration and the
    simulation would die for no visible reason. Embedding the mesh in the
    target name makes the two coherent by construction: every customization
    gets its own compiled tree.

    Hence the mesh is a parameter and not a ``Value``: ``--attr
    magia_v2/n_tiles_x=...`` is now rejected outright instead of quietly
    producing a mismatch. Note that ``--param n_tiles_x=...`` feeds the very
    same registry as the target-name form, so it does reshape the tree —
    never use it, the target name would no longer describe what runs.

    ``spatz_romfile`` stays a plain run-time attribute (``--attr
    magia_v2/spatz_romfile=<path>``) on purpose: it feeds
    ``MemoryV3Config.stim_file``, which is ``Annotated[str, Runtime]``
    precisely because the path differs between the build and the run, so it
    is overlaid at run time and never enters the compiled tree.
    """

    def __init__(self, parent, name):
        super().__init__(parent, name)
        self.n_tiles_x = TargetParameter(parent, name='n_tiles_x',
            value=MagiaArch.N_TILES_X, cast=int,
            description='Number of tiles on X dimension').value
        self.n_tiles_y = TargetParameter(parent, name='n_tiles_y',
            value=MagiaArch.N_TILES_Y, cast=int,
            description='Number of tiles on Y dimension').value

        self.nb_clusters = self.n_tiles_x*self.n_tiles_y

        if MagiaArch.SPATZ_ENABLE:
            self.romfile = Value(self, 'spatz_romfile', MagiaArch.SPATZ_ROMFILE, cast=str,
                description='Snitch_Spatz rom file')
            print("SNITCH_SPATZ complex enabled")

class MagiaDSE:
    # ------------------------------------------------------------------
    # Read by both descriptions (io v1 and io_v2)
    # ------------------------------------------------------------------
    SOC_L2_LATENCY              = 2
    TILE_ICACHE_REFILL_LATENCY  = 2
    TILE_TCDM_LATENCY           = 1
    TILE_IDMA0_BQUEUE_SIZE      = 4
    TILE_IDMA0_B_SIZE           = 0
    TILE_IDMA1_BQUEUE_SIZE      = 4
    TILE_IDMA1_B_SIZE           = 0

    # ------------------------------------------------------------------
    # io (v1) tile x-bars only — read by tile.py
    #
    # The io_v2 tile drives its x-bars as *beat* routers, and the beat flavour
    # of interco.router_v2 reads neither latency nor bandwidth nor synchronous
    # (see its "Which fields each kind actually reads" table): there, timing
    # comes from per-cycle arbitration and beat streaming, not from a fixed
    # pipeline latency. Turning these knobs has no effect at all when
    # MagiaArch.ENABLE_IO_V2 is True.
    # ------------------------------------------------------------------
    TILE_AXI_XBAR_LATENCY       = 2
    TILE_AXI_XBAR_SYNC          = False
    TILE_OBI_XBAR_LATENCY       = 2
    TILE_OBI_XBAR_SYNC          = True

    # ------------------------------------------------------------------
    # io_v2 tile x-bars only — read by tile_v2.py
    #
    # Outstanding bursts allowed per input port of the beat x-bars (the RTL AXI
    # x-bars register all ports and allow a few outstanding transactions each).
    # Per-input, so one busy master cannot starve the others.
    # ------------------------------------------------------------------
    TILE_AXI_XBAR_MAX_BURSTS    = 4
    TILE_OBI_XBAR_MAX_BURSTS    = 4
    TILE_WIDE_XBAR_MAX_BURSTS   = 4