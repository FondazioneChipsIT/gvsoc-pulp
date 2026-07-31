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

import gvsoc.systree
import memory.memory as memory
import vp.clock_domain
import utils.loader.loader

from pulp.chips.magia_v2.tile import MagiaV2Tile
from pulp.chips.magia_v2.arch import *

if MagiaArch.ENABLE_PCIE_VFIO:
    import pulp.pcie_vfio_bridge.pcie_vfio_mem_bridge
from pulp.floonoc.floonoc import *
from pulp.chips.magia_v2.fractal_sync.fractal_tree import build_fractal_tree
from pulp.chips.magia_v2.kill_module.kill_module import *
from typing import List

class MagiaV2Soc(gvsoc.systree.Component):
    def __init__(self, parent, name, tree, parser, binary=None):
        super().__init__(parent, name)

        self.set_attributes(tree)

        # Bin Loader
        if not MagiaArch.ENABLE_PCIE_VFIO:
            loader=utils.loader.loader.ElfLoader(self, f'loader', binary=binary)
            self.loader = loader

        # Simulation engine killer
        killer=KillModule(self,'kill-module',kill_addr_base=MagiaArch.TEST_END_ADDR_START,kill_addr_size=MagiaArch.TEST_END_SIZE,nb_cores_to_wait=tree.nb_clusters,
                          done_irq_enable=MagiaArch.ENABLE_PCIE_VFIO)

        # Single clock domain
        clock = vp.clock_domain.Clock_domain(self, 'tile-clock',
                                             frequency=MagiaArch.TILE_CLK_FREQ)
        clock.o_CLOCK(self.i_CLOCK())

        # Create Tiles
        cluster:List[MagiaV2Tile] = []
        for id in range(0,tree.nb_clusters):
            cluster.append(MagiaV2Tile(self, f'magia-tile-{id}', tree, parser, id))

        l2_mem = memory.Memory(self, f'L2-mem', size=MagiaArch.L2_SIZE,latency=MagiaDSE.SOC_L2_LATENCY)

        if MagiaArch.ENABLE_PCIE_VFIO:
            pcie_ep = pulp.pcie_vfio_bridge.pcie_vfio_mem_bridge.PCIeVfioMemBridge(
                     self,
                     'l2-vfio-bridge',
                     socket_path='/tmp/gvsoc.sock',
                     bar0_size=0x1000,
                     dma_chunk_bytes=16
                    )

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
        # with the io_v2 description in soc_v2.py)
        build_fractal_tree(self, tree, cluster, tile_matrix)

        #Connect NoC to tiles and L2    
        noc = FlooNoc2dMeshNarrowWide(self,
                                    name='magia-noc',
                                    narrow_width=4,
                                    wide_width=32,
                                    ni_outstanding_reqs=8, #need to double check this with RTL
                                    router_input_queue_size=4, #need to double check this with RTL
                                    dim_x=tree.n_tiles_x+1, dim_y=tree.n_tiles_y)
        

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
                cluster[id].o_KILLER_OUTPUT(killer.i_INPUT())
                cluster[id].o_NARROW_OUTPUT(noc.i_NARROW_INPUT(x,y))
                noc.o_NARROW_MAP(cluster[id].i_NARROW_INPUT(),name=f'narrow-tile-{id}-l1-mem',base=MagiaArch.L1_ADDR_START+(id*MagiaArch.L1_TILE_OFFSET),size=MagiaArch.L1_SIZE,x=x,y=y,rm_base=False)
                cluster[id].o_WIDE_OUTPUT(noc.i_WIDE_INPUT(x,y))
                noc.o_WIDE_MAP(cluster[id].i_WIDE_INPUT(),name=f'wide-tile-{id}-l1-mem',base=MagiaArch.L1_ADDR_START+(id*MagiaArch.L1_TILE_OFFSET),size=MagiaArch.L1_SIZE,x=x,y=y,rm_base=False)
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
            noc.o_NARROW_BIND(l2_mem.i_INPUT(), x=0, y=y)
            noc.o_WIDE_BIND(l2_mem.i_INPUT(), x=0, y=y)
        
        noc.o_MAP_DIR(base=MagiaArch.L2_ADDR_START,size=MagiaArch.L2_SIZE, dir=FlooNocDirection.LEFT,name=f'mem_left', rm_base=True)

        if MagiaArch.ENABLE_PCIE_VFIO:
            pcie_ep.o_MEM(l2_mem.i_INPUT())
            killer.o_IRQ_DONE(pcie_ep.i_IRQ_DONE())

        # Bind loader or PCIe bridge to clusters
        for id in range(0,tree.nb_clusters):
            if MagiaArch.ENABLE_PCIE_VFIO:
                pcie_ep.o_FETCH_ENABLE(cluster[id].i_FETCHEN())
                pcie_ep.o_ENTRY_ADDR(cluster[id].i_ENTRY())
            else:
                if (id == 0):
                    loader.o_OUT(cluster[id].i_LOADER()) #only cluster connected to the corner loads the elf
                loader.o_START(cluster[id].i_FETCHEN())
                loader.o_ENTRY(cluster[id].i_ENTRY())