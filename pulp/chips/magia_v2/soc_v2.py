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

"""magia_v2 SoC described entirely with io_v2 models.

Sibling of :mod:`pulp.chips.magia_v2.soc`, selected by
``MagiaArch.ENABLE_IO_V2``. Same topology and same address map; the NoC is
``pulp.floonoc_v2``, the L2 is ``memory.memory_v3``, the loader is
``utils.loader.loader_v2`` and the tiles come from :mod:`tile_v2`.

Structural differences forced by io_v2 (an io_v2 slave port binds exactly one
master, where the v1 protocol allowed several):

- **L2 fan-in.** The v1 SoC binds every narrow NI *and* every wide NI directly
  onto the L2's single input port. Here they are joined by one *untimed* router,
  which decodes and forwards with no timing of its own — so, as in v1, no L2
  arbitration is modelled. Each NI channel crosses the beat/non-beat boundary
  through the framework's beat-to-single-req adapter.
- **Kill module.** One input port per tile instead of a single shared one.
Burst legality on the NoC
-------------------------

The NI enforces the AXI 4 KiB-page rule at build time: every target range must
be page-aligned and a whole number of pages, so that a burst — which the iDMA's
AXI back-end never lets cross a page — can only ever address one mesh position,
i.e. every wormhole packet has a single destination.

That holds here: the tile L1 windows (``L1_SIZE`` = 0xE_0000, at
``L1_TILE_OFFSET`` = 0x10_0000 apart) and the L2 window are page-aligned
multiples of the page, and ``MagiaDSE.TILE_IDMA*_B_SIZE = 0`` leaves the AXI
back-end's own bound in charge (4 KiB, 256 beats, never across a page). It only
holds because ``arch.py`` declares *true* sizes; with the older ``SIZE - 1``
convention the windows were one byte short of whole pages and the check had to
be disabled.
"""

import gvsoc.systree
import vp.clock_domain

import memory.memory_v3 as memory_v3
from memory.memory_v3 import MemoryV3Config
import interco.router_v2 as router_v2
from interco.router_v2 import RouterConfig, KIND_UNTIMED
import utils.loader.loader_v2

from pulp.chips.magia_v2.tile_v2 import MagiaV2Tile
from pulp.chips.magia_v2.arch import *

from pulp.floonoc_v2.floonoc_v2 import *
from pulp.chips.magia_v2.fractal_sync.fractal_tree import build_fractal_tree
from pulp.chips.magia_v2.kill_module.kill_module_v2 import KillModule
from typing import List


class MagiaV2Soc(gvsoc.systree.Component):
    def __init__(self, parent, name, tree, parser, binary=None):
        super().__init__(parent, name)

        self.set_attributes(tree)

        if MagiaArch.ENABLE_PCIE_VFIO:
            # The PCIe/VFIO bridge is an io (v1) model: it cannot bind to an
            # io_v2 fabric, and there is no v1<->v2 adapter in gvsoc. It has to be
            # ported before it can be used on this path.
            raise RuntimeError(
                'ENABLE_PCIE_VFIO is not supported with ENABLE_IO_V2: '
                'pulp.pcie_vfio_bridge.pcie_vfio_mem_bridge is still an io (v1) '
                'model. Port it to io_v2 or run with ENABLE_IO_V2 = False.')

        # Bin Loader
        loader = utils.loader.loader_v2.ElfLoader(self, f'loader', binary=binary)
        self.loader = loader

        # Simulation engine killer. One input port per tile (io_v2 fan-in).
        killer = KillModule(self, 'kill-module', kill_addr_base=MagiaArch.TEST_END_ADDR_START,
                          kill_addr_size=MagiaArch.TEST_END_SIZE,
                          nb_cores_to_wait=tree.nb_clusters,
                          done_irq_enable=MagiaArch.ENABLE_PCIE_VFIO,
                          nb_inputs=tree.nb_clusters)

        # Single clock domain
        clock = vp.clock_domain.Clock_domain(self, 'tile-clock',
                                             frequency=MagiaArch.TILE_CLK_FREQ)
        clock.o_CLOCK(self.i_CLOCK())

        # Create Tiles
        cluster:List[MagiaV2Tile] = []
        for id in range(0,tree.nb_clusters):
            cluster.append(MagiaV2Tile(self, f'magia-tile-{id}', tree, parser, id))

        # L2. No address truncation: L2_SIZE is not a power of two, and the NoC
        # already rebases the accesses (rm_base on the L2 mapping).
        l2_mem = memory_v3.Memory(self, f'L2-mem', config=MemoryV3Config(
            size=MagiaArch.L2_SIZE, latency=MagiaDSE.SOC_L2_LATENCY, truncate=False))

        # L2 fan-in: the NoC exposes one narrow and one wide output per row and
        # the memory has a single input port, so the 2*n_tiles_y channels have to
        # be joined. One *untimed* router does it: it decodes and forwards with
        # zero timing of its own, which is exactly the v1 behaviour (there all
        # the NI ports poked the memory directly, with no arbitration at all).
        #
        # It also keeps the io_v2 plumbing minimal. Its inputs are single-req, so
        # each NI channel crosses through the framework's beat-to-single-req
        # adapter — the calibrated beat/non-beat boundary, the same one the iDMA
        # uses onto a bandwidth router — and the router binds the sync memory
        # directly. In particular no beat-width adapter is involved: folding the
        # 4-byte narrow channels into a 32-byte beat router was the only width
        # mismatch in the whole chip.
        l2_xbar = router_v2.Router(self, 'L2-xbar', config=RouterConfig(
            kind=KIND_UNTIMED))

        # Create Tile matrix for IDs
        # --------------> X direction
        # | 0  1  2  3
        # | 4  5  6  7
        # | 8  9 10 11
        # |12 13 14 15
        # |
        # V
        # Y direction

        # Init matrix:
        tile_matrix: List[List[int]] = [[0 for _ in range(tree.n_tiles_x)] for _ in range(tree.n_tiles_y)]
        # Populate matrix
        id=0
        for y in range(0,tree.n_tiles_y):
            for x in range(0,tree.n_tiles_x):
                tile_matrix[y][x] = id
                id = id +1

        for row in tile_matrix:
            print(row)

        # Create and wire the fractal sync tree (pure wire plumbing, shared
        # with the v1 description in soc.py)
        build_fractal_tree(self, tree, cluster, tile_matrix)

        #Connect NoC to tiles and L2
        noc = FlooNocV22dMeshNarrowWide(self,
                                    name='magia-noc',
                                    narrow_width=MagiaArch.BYTES_PER_WORD,
                                    wide_width=MagiaArch.TILE_WIDE_WIDTH,
                                    ni_outstanding_reqs=8, #need to double check this with RTL
                                    router_input_queue_size=4, #need to double check this with RTL
                                    dim_x=tree.n_tiles_x+1, dim_y=tree.n_tiles_y,
                                    # AXI page. Keeps the NI's burst-legality
                                    # check on: see "Burst legality on the NoC"
                                    # in the module docstring.
                                    max_burst_size=4096)


        # Create noc routers
        for y in range(0,tree.n_tiles_y):
            for x in range(1,tree.n_tiles_x+1):
                print(f"[NoC] Adding router and NI at position x={x} y={y}")
                noc.add_router(x, y)
                noc.add_network_interface(x, y)

        for y in range(0,tree.n_tiles_y):
            print(f"[NoC] L2-NI at position x={0} y={y}")
            noc.add_network_interface(0, y)

        # Bind clusters to noc. E.g. for 4x4
        # {1.0}----{2.0}----{3.0}----{4.0}
        #   | 0      |  1     |  2     |  3
        #   |        |        |        |
        # {1.1}----{2.1}----{3.1}----{4.1}
        #   | 4      |  5     |  6     |  7
        #   |        |        |        |
        # {1.2}----{2.2}----{3.2}----{4.2}
        #   | 8      |  9     |  10    |  11
        #   |        |        |        |
        # {1.3}----{2.3}----{3.3}----{4.3}
        #     12        13       14       15

        id = 0
        for y in range(0,tree.n_tiles_y):
            for x in range(1,tree.n_tiles_x+1):
                print(f"[NoC] Adding cluster {id} at position x={x} y={y}")
                cluster[id].o_KILLER_OUTPUT(killer.i_INPUT(id))
                cluster[id].o_NARROW_OUTPUT(noc.i_NARROW_INPUT(x,y))
                # The NI mapping table is shared by both channels, so the tile's
                # L1 range is declared once (with the narrow binding) and the
                # wide output only needs its binding.
                noc.o_NARROW_MAP(cluster[id].i_NARROW_INPUT(),name=f'tile-{id}-l1-mem',base=MagiaArch.L1_ADDR_START+(id*MagiaArch.L1_TILE_OFFSET),size=MagiaArch.L1_SIZE,x=x,y=y,rm_base=False)
                cluster[id].o_WIDE_OUTPUT(noc.i_WIDE_INPUT(x,y))
                noc.o_WIDE_BIND(cluster[id].i_WIDE_INPUT(),x=x,y=y)
                id += 1

        # Bind memory to noc
        # {0.0}----{1.0}----{2.0}----{3.0}----{4.0}
        #   | L2     | 0      |  1     |  2     |  3
        #   |        |        |        |        |
        # {0.1}----{1.1}----{2.1}----{3.1}----{4.1}
        #   | L2     | 4      |  5     |  6     |  7
        #   |        |        |        |        |
        # {0.2}----{1.2}----{2.2}----{3.2}----{4.2}
        #   | L2     | 8      |  9     |  10    |  11
        #   |        |        |        |        |
        # {0.3}----{1.3}----{2.3}----{3.3}----{4.3}
        #     L2       12        13       14       15

        for y in range(0,tree.n_tiles_y):
            print(f"[NoC] Adding L2 at position x={0} y={y}")
            noc.o_NARROW_BIND(l2_xbar.i_INPUT(2*y), x=0, y=y)
            noc.o_WIDE_BIND(l2_xbar.i_INPUT(2*y + 1), x=0, y=y)

        l2_xbar.o_MAP_DEFAULT(l2_mem.i_INPUT(), name='L2-mem')

        noc.o_MAP_DIR(base=MagiaArch.L2_ADDR_START,size=MagiaArch.L2_SIZE, dir=FlooNocV2Direction.LEFT,name=f'mem_left', rm_base=True)

        # Bind loader to clusters
        for id in range(0,tree.nb_clusters):
            if (id == 0):
                loader.o_OUT(cluster[id].i_LOADER()) #only cluster connected to the corner loads the elf
            loader.o_START(cluster[id].i_FETCHEN())
            loader.o_ENTRY(cluster[id].i_ENTRY())
