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

"""magia_v2 tile described entirely with io_v2 models.

Sibling of :mod:`pulp.chips.magia_v2.tile`, selected by
``MagiaArch.ENABLE_IO_V2``. Same architecture, same address map and same DSE
knobs; every model is the io_v2 one (there is no v1<->v2 bridge in gvsoc, so the
switch is all-or-nothing).

Model substitutions
-------------------

============================  ==========================================
v1                            io_v2
============================  ==========================================
``interco.router``            ``interco.router_v2``, ``beat`` kind (per-cycle
                              arbitration + beat streaming; see the note in
                              the x-bar instantiation about the DSE latency)
``memory.memory``             ``memory.memory_v3``
``cache.cache`` hierarchies   ``cache.cache_v4`` hierarchies
L1/DMA/HWPE interleavers      ``SpatzTcdmInterco`` (contention model)
``pulp.idma.snitch_dma``      ``ips.pulp.idma_v2.snitch_dma_tcdm``
``light_redmule``             ``light_redmule_v2``
``event_unit_v3``             ``event_unit_v3_v2``
``stdout_v3``                 ``stdout_v3_v2``
magia mm controllers          their ``*_v2`` siblings
``cpu.iss`` CV32 / Spatz      ``cpu.iss_v2`` CV32 (``core_v2``) / Spatz
                              with ``vlsu_v2=True``
============================  ==========================================

Structural differences forced by io_v2
--------------------------------------

An io_v2 slave port binds **exactly one** master, where the v1 protocol allowed
several. Wherever the v1 tile relied on that, the fan-in is made explicit here.
None of it changes the modelled behaviour:

- **TCDM.** The v1 tile drives the 32 banks from three separate interleavers.
  Here a single ``SpatzTcdmInterco`` owns the bank ports and exposes one input
  per master (narrow, one bank per access; wide, spanning several banks with
  priority). Same decode as the v1 interleavers, but bank contention is now
  modelled — see the class docstring.
- **Wide NoC egress.** The two iDMAs have four AXI masters (read + write each)
  which the v1 tile ties to the single ``wide_output`` port. Here they are
  arbitrated by a beat router at the NoC wide width, which is also what the
  hardware needs to mux them.
- **Collapsed off-tile mappings.** The v1 xbars declare one mapping per remote
  tile, all bound to the same downstream port. Here everything that leaves the
  tile goes through one catch-all mapping — same routing, one master port.
  (Consequence: an address that had no mapping at all in v1, and would be
  reported as a decode error, is now forwarded off-tile instead.)
- **Two TCDM ports for the OBI xbar.** The stack window and the local-L1 window
  were two mappings onto the same TCDM port; they are two crossbar inputs here.
- **No ``o_DATA_DEBUG``.** In iss_v2 the gdbserver reaches memory through the
  out-of-band ``debug_mem`` backdoor resolved from the data port, so there is no
  second data port to wire.
"""

import json
import math
import gvsoc.systree
import gdbserver.gdbserver

import memory.memory_v3 as memory_v3
from memory.memory_v3 import MemoryV3Config
import interco.router_v2 as router_v2
from interco.router_v2 import RouterConfig, RouterMapping, KIND_BEAT
from gvsoc.signature import IoV2Beat, IoV2SingleReq

from pulp.stdout.stdout_v3_v2 import StdoutV2
from pulp.cpu.iss.spatz import Spatz
from pulp.cpu.iss.spatz_config import SpatzConfig
from pulp.snitch.hierarchical_cache_v2 import Hierarchical_cache
from pulp.chips.magia_v2.cv32.hierarchical_cache_v2 import CV32_Hierarchical_cache

from pulp.chips.magia_v2.arch import *
from pulp.chips.magia_v2.cv32.core_v2 import CV32CoreTest
from pulp.snitch.snitch_cluster.spatz.spatz_tcdm_interco import (SpatzTcdmInterco,
                                                                SpatzTcdmIntercoConfig)
from pulp.light_redmule.light_redmule_v2 import LightRedmule
from ips.pulp.idma_v2.snitch_dma_tcdm import SnitchDmaTcdmV2
from pulp.chips.magia_v2.fractal_sync_mm_ctrl.fractal_sync_mm_ctrl_v2 import FSync_mm_ctrl
from pulp.chips.magia_v2.idma_mm_ctrl.idma_mm_ctrl_v2 import iDMA_mm_ctrl
from pulp.chips.magia_v2.spatz.snitch_spatz_regs_v2 import SnitchSpatzRegs
from pulp.event_unit.event_unit_v3_v2 import Event_unit


class MagiaTileTcdm(gvsoc.systree.Component):
    """Tile TCDM: a contention-modelling io_v2 crossbar over ``memory_v3`` banks.

    The crossbar is ``SpatzTcdmInterco``, the model Germain wrote for the RTL
    spatz cluster: it folds a word-interleaved ``stream_xbar`` (round-robin, one
    master per bank per cycle, a conflict costs the loser a cycle through the
    deny/retry handshake) and a ``mem_wide_narrow_mux`` (wide masters reserve
    every bank their access spans for the tick and preempt the narrow ones).
    That is the same shape as magia's TCDM, so it is used as-is rather than
    forked — its address decode is *identical* to the one the three v1
    interleavers computed.

    This is a deliberate accuracy upgrade over the v1 tile, not a port: the v1
    interleavers were transparent and modelled **no** bank contention at all, so
    TCDM timing came only from the bank latency. Expect L1 timing to change and
    the DSE numbers to need re-measuring.

    Two consequences worth knowing:

    - **Masters must honour the synchronous-retry rule.** A denied master has to
      re-issue *inside* its ``retry()`` callback: the crossbar's election window
      is open only for the duration of that call (see the io_v2 manual,
      "Retry must be serviced synchronously"). A master that defers to the next
      cycle live-locks.
    - **No offset mask on the Spatz ports.** The v1 Spatz interleaver masked the
      address with ``L1_TILE_OFFSET - 1``; here that is redundant, because
      ``L1_TILE_OFFSET / (nb_banks * bank_width) * bank_width`` is exactly the
      bank depth (0x8000), so the tile-index bits fall above the bank and the
      banks' ``truncate`` removes them. Verified identical for every tile.

    Input-port allocation:

    - narrow 0: OBI, stack window
    - narrow 1: OBI, local L1 window
    - narrow 2..5: Spatz VLSU ports (only when SPATZ_ENABLE)
    - wide 0: NoC wide channel
    - wide 1..4: iDMA0 read/write, iDMA1 read/write
    - wide 5: RedMulE
    """

    # Narrow ports
    NARROW_OBI_STACK = 0
    NARROW_OBI_L1    = 1
    NARROW_SPATZ     = 2
    NB_NARROW        = NARROW_SPATZ + (4 if MagiaArch.SPATZ_ENABLE else 0)

    # Wide ports
    WIDE_NOC     = 0
    WIDE_IDMA0_R = 1
    WIDE_IDMA0_W = 2
    WIDE_IDMA1_R = 3
    WIDE_IDMA1_W = 4
    WIDE_REDMULE = 5
    NB_WIDE      = 6

    def __init__(self, parent, name, tree, parser):
        super().__init__(parent, name)

        nb_banks = MagiaArch.N_MEM_BANKS
        bank_size = MagiaArch.N_WORDS_BANK * MagiaArch.BYTES_PER_WORD

        ico = SpatzTcdmInterco(self, 'ico', config=SpatzTcdmIntercoConfig(
            nb_masters=self.NB_NARROW,
            nb_slaves=nb_banks,
            interleaving_width=int(math.log2(MagiaArch.BYTES_PER_WORD)),
            nb_wide_masters=self.NB_WIDE))

        for i in range(nb_banks):
            # Same geometry and latency as the v1 banks; truncate=True is the v1
            # truncate_size=bank_size (the size is a power of two, so the mask is
            # exact).
            bank = memory_v3.Memory(self, f'bank_{i}', config=MemoryV3Config(
                size=bank_size, atomics=True, latency=MagiaDSE.TILE_TCDM_LATENCY,
                truncate=True))
            ico.o_OUTPUT(i, bank.i_INPUT())

        # Bind external ports (input->[internal]output->crossbar)
        for i in range(self.NB_NARROW):
            self.bind(self, f'narrow_input_{i}', ico, f'input_{i}')

        for i in range(self.NB_WIDE):
            self.bind(self, f'wide_input_{i}', ico, f'wide_input_{i}')

    def i_INPUT(self, id: int) -> gvsoc.systree.SlaveItf:
        """OBI-side narrow port (0 = stack window, 1 = local L1 window)."""
        return gvsoc.systree.SlaveItf(self, f'narrow_input_{id}',
            signature=IoV2SingleReq())

    def i_SNITCH_SPATZ(self, id: int) -> gvsoc.systree.SlaveItf:
        """Spatz VLSU port ``id``."""
        return gvsoc.systree.SlaveItf(self, f'narrow_input_{self.NARROW_SPATZ + id}',
            signature=IoV2SingleReq())

    def i_WIDE_INPUT(self, id: int) -> gvsoc.systree.SlaveItf:
        """Wide (bank-splitting) port ``id``."""
        return gvsoc.systree.SlaveItf(self, f'wide_input_{id}',
            signature=IoV2SingleReq())


class MagiaV2Tile(gvsoc.systree.Component):
    def ev_unit_config(self):
        event_unit_json = {
            "event_unit": {
                "version": "4",
                "mapping": {
                    "base": MagiaArch.EVENT_UNIT_ADDR_START,
                    "size": MagiaArch.EVENT_UNIT_SIZE,
                    "remove_offset": MagiaArch.EVENT_UNIT_ADDR_START
                },
                "config": {
                    "nb_core": 1,
                    "properties": {
                        "dispatch": {"size": 8},
                        "mutex": {"nb_mutexes": 0},
                        "barriers": {"nb_barriers": 0},
                        "soc_event": {"nb_fifo_events": 8, "fifo_event": 8},
                        "events": {
                            "dispatch": 8,
                            "mutex": 0,
                            "barrier": 0
                        }
                    }
                }
            }
        }
        json_data = json.dumps(event_unit_json)
        return json.loads(json_data)

    def __init__(self, parent, name, tree, parser, tid: int=0):
        super().__init__(parent, name)

        # Core model from pulp cores (iss_v2: fetch and data ports on io_v2)
        core_cv32 = CV32CoreTest(self, f'tile-{tid}-cv32-core', core_id=tid)

        if MagiaArch.SPATZ_ENABLE:

            # Snitch Spatz boot rom file (latency 0, as in the v1 tile)
            snitch_spatz_rom = memory_v3.Memory(self, 'snitch-spatz-rom',
                config=MemoryV3Config(size=MagiaArch.SPATZ_BOOTROM_SIZE, latency=0,
                    stim_file=self.get_file_path(tree.romfile)))

            # Snitch Spatz cores (fetch_enable is set to false as we control the boot
            # sequence. The core automatically starts from the rom and the corresponding
            # boot address as soon as we issue the fetch enable). vlsu_v2 puts the data
            # and VLSU ports on io_v2.
            config = SpatzConfig(isa="rv32imfdcav", fetch_enable=False,
                boot_addr=MagiaArch.SPATZ_BOOTROM_ADDR, hart_id=tid + tree.nb_clusters,
                htif=False, nb_lanes=4, lane_width=4, vlsu_v2=True)

            snitch_spatz = Spatz(self, f'tile-{tid}-snitch-spatz', config=config)

            # Instruction cache (from snitch cluster model, on cache_v4)
            snitch_spatz_i_cache = Hierarchical_cache(self, f'tile-{tid}-snitch-spatz-icache',
                nb_cores=1, has_cc=0, l1_line_size_bits=7)

            # Snitch Spatz CC control registers
            snitch_spatz_regs = SnitchSpatzRegs(self, f'tile-{tid}-snitch-spatz-regs')

        # Instruction cache (from snitch cluster model, on cache_v4)
        cv32_i_cache = CV32_Hierarchical_cache(self, f'tile-{tid}-cv32-icache', nb_cores=1,
            has_cc=0, l1_line_size_bits=4)

        # Data scratchpad
        l1_tcdm = MagiaTileTcdm(self, f'tile-{tid}-tcdm', tree, parser)

        # AXI and OBI x-bars, beat-streaming for maximum accuracy: one beat per
        # cycle per channel at the bus width, round-robin arbitration between
        # inputs with burst atomicity, and a bounded number of outstanding
        # bursts per input (the RTL x-bars register all ports).
        #
        # Note what this does *not* model, versus the v1 description: the beat
        # flavour of router_v2 reads neither `latency` nor `bandwidth`, so
        # MagiaDSE.TILE_{AXI,OBI}_XBAR_LATENCY has no effect here — the fixed
        # 2-cycle x-bar latency of the v1 model is replaced by the arbitration
        # and streaming behaviour, not added to it.
        #
        # Masters that speak single-req (the cores' data ports, the caches'
        # refill ports, the loader) cross onto this plane through the
        # framework's single-req-to-beat adapter; slaves that answer inline
        # (the register files, the memories) are reached through the
        # beat-to-sync / beat-to-single-req ones. All widths on the narrow side
        # are BYTES_PER_WORD, so no beat-width conversion is ever involved.
        tile_xbar = router_v2.Router(self, f'tile-{tid}-axi-xbar', config=RouterConfig(
            kind=KIND_BEAT, width=MagiaArch.BYTES_PER_WORD,
            max_pending_bursts_per_input=MagiaDSE.TILE_AXI_XBAR_MAX_BURSTS))
        obi_xbar = router_v2.Router(self, f'tile-{tid}-obi-xbar', config=RouterConfig(
            kind=KIND_BEAT, width=MagiaArch.BYTES_PER_WORD,
            max_pending_bursts_per_input=MagiaDSE.TILE_OBI_XBAR_MAX_BURSTS))

        # Wide fan-in of the two iDMAs towards the NoC wide channel.
        #
        # This has no counterpart in the RTL, where the tile drives a single wide
        # AXI port and the two iDMAs own disjoint channel sets of it: AR/R belong
        # to iDMA0 (which only ever reads, L2 -> L1) and AW/W/B to iDMA1 (which
        # only ever writes, L1 -> L2). It exists because the NI has a single wide
        # slave port and an io_v2 slave port binds exactly one master, while the
        # iDMA model exposes its read and write back-ends as separate masters —
        # four here, of which two carry traffic under the convention above.
        #
        # It does not add the arbitration the RTL lacks. With shared_rw_channel
        # left false the beat router keeps two independent channels and forwards
        # one read beat *and* one write beat per cycle, so iDMA0's reads never
        # queue behind iDMA1's writes: the AR/R versus AW/W/B split above is
        # preserved. What it does add is one cycle of head-of-burst registration
        # (a beat accepted on cycle N is forwarded from N+1 on, pipelined, so
        # only the latency of a burst grows, not its rate) plus the outstanding
        # burst budget below.
        #
        # A cheaper fan-in is not available on this plane: the untimed router
        # declares single-req inputs, so a beat master reaching it would go
        # through a beat-to-single-req adapter, losing beat streaming and the
        # NoC's 4 KiB burst-legality guarantee. That is why the L2 fan-in in
        # soc_v2.py can be untimed and this one cannot.
        wide_xbar = router_v2.Router(self, f'tile-{tid}-wide-xbar', config=RouterConfig(
            kind=KIND_BEAT, width=MagiaArch.TILE_WIDE_WIDTH,
            max_pending_bursts_per_input=MagiaDSE.TILE_WIDE_XBAR_MAX_BURSTS))

        # IDMA Controller
        idma_mm_ctrl = iDMA_mm_ctrl(self, f'tile-{tid}-idma-ctrl-mm')

        # IDMA
        idma0 = SnitchDmaTcdmV2(self, f'tile-{tid}-idma0',
            loc_base=(tid*MagiaArch.L1_TILE_OFFSET), loc_size=MagiaArch.L1_SIZE,
            tcdm_width=MagiaArch.TILE_WIDE_WIDTH, axi_width=MagiaArch.TILE_WIDE_WIDTH,
            transfer_queue_size=1, burst_queue_size=MagiaDSE.TILE_IDMA0_BQUEUE_SIZE,
            burst_size=MagiaDSE.TILE_IDMA0_B_SIZE)
        idma1 = SnitchDmaTcdmV2(self, f'tile-{tid}-idma1',
            loc_base=(tid*MagiaArch.L1_TILE_OFFSET), loc_size=MagiaArch.L1_SIZE,
            tcdm_width=MagiaArch.TILE_WIDE_WIDTH, axi_width=MagiaArch.TILE_WIDE_WIDTH,
            transfer_queue_size=1, burst_queue_size=MagiaDSE.TILE_IDMA1_BQUEUE_SIZE,
            burst_size=MagiaDSE.TILE_IDMA1_B_SIZE)

        # Redmule
        redmule = LightRedmule(self, f'tile-{tid}-redmule',
                                    tcdm_bank_width     = MagiaArch.BYTES_PER_WORD,
                                    tcdm_bank_number    = 8, # here we set 8 since tcdm_bank_width x tcdm_bank_number --> 8 x 4 = 32bytes, i.e., 256 bits. Please do not consider tcdm_bank_width and tcdm_bank_number as the boundaries of the TCDM, but rather the size of the port towards it.
                                    elem_size           = 2, # max number of bytes per element --> if FP16 then elem_size=2. This is the max number to accomodate any supported format which for now are 8bits and 16bits data types
                                    ce_height           = 8,
                                    ce_width            = 8,
                                    ce_pipe             = 1,
                                    queue_depth         = 1)

        # Fsync mm controller
        fsync_mm_ctrl = FSync_mm_ctrl(self, f'tile-{tid}-fs-ctrl-mm')

        # Event Unit
        self.add_properties(self.ev_unit_config())
        event_unit = Event_unit(self, f'tile-{tid}-event-unit', self.get_property('event_unit/config'))

        # UART
        stdout = StdoutV2(self, f'tile-{tid}-stdout', max_cluster=tree.nb_clusters,
            max_core_per_cluster=1, user_set_core_id=0, user_set_cluster_id=tid)

        #
        # OBI xbar input ports
        #
        OBI_IN_LOADER    = 0
        OBI_IN_CV32_DATA = 1
        OBI_IN_AXI       = 2
        OBI_IN_SPATZ_DATA   = 3
        OBI_IN_SPATZ_REFILL = 4

        # AXI (tile) xbar input ports
        AXI_IN_CV32_REFILL = 0
        AXI_IN_OBI         = 1
        AXI_IN_NOC         = 2

        # Bind: loader -> obi interconnect
        self.__o_LOADER(obi_xbar.i_INPUT(OBI_IN_LOADER))

        if MagiaArch.SPATZ_ENABLE:
            # Bind: snitch spatz core data -> obi interconnect
            snitch_spatz.o_DATA(obi_xbar.i_INPUT(OBI_IN_SPATZ_DATA))

            # Bind: snitch spatz core -> snitch spatz icache
            snitch_spatz.o_FETCH(snitch_spatz_i_cache.i_INPUT(0))
            snitch_spatz.o_FLUSH_CACHE(snitch_spatz_i_cache.i_FLUSH())
            snitch_spatz_i_cache.o_FLUSH_ACK(snitch_spatz.i_FLUSH_CACHE_ACK())

            # Bind: snitch spatz icache -> tile interconnect
            snitch_spatz_i_cache.o_REFILL(obi_xbar.i_INPUT(OBI_IN_SPATZ_REFILL))

            # Bind: snitch spatz TCDM
            for port in range(0, 4):
                snitch_spatz.o_VLSU(port, l1_tcdm.i_SNITCH_SPATZ(port))

            # Bind: snitch spatz core complex registers
            snitch_spatz_regs.o_CLK_EN(snitch_spatz.i_FETCHEN())
            snitch_spatz_regs.o_START(snitch_spatz.i_IRQ(11))

        # Bind: cv32 core data -> obi interconnect
        core_cv32.o_DATA(obi_xbar.i_INPUT(OBI_IN_CV32_DATA))

        # Bind: cv32 core -> icache
        core_cv32.o_FETCH(cv32_i_cache.i_INPUT(0))
        core_cv32.o_FLUSH_CACHE(cv32_i_cache.i_FLUSH())
        cv32_i_cache.o_FLUSH_ACK(core_cv32.i_FLUSH_CACHE_ACK())

        # Bind: icache -> tile interconnect
        cv32_i_cache.o_REFILL(tile_xbar.i_INPUT(AXI_IN_CV32_REFILL))

        # Bind obi xbar so that it can communicate with RedMule
        obi_xbar.o_MAP(redmule.i_INPUT_V2(), RouterMapping(
            base=MagiaArch.REDMULE_CTRL_ADDR_START,
            size=MagiaArch.REDMULE_CTRL_SIZE, remove_base=True),
            name=f'redmule-mm-{tid}-mem')

        # Bind obi xbar so that it can communicate with iDMA mmapped controller
        obi_xbar.o_MAP(idma_mm_ctrl.i_INPUT(), RouterMapping(
            base=MagiaArch.IDMA_CTRL_ADDR_START,
            size=MagiaArch.IDMA_CTRL_SIZE, remove_base=True),
            name=f'iDMA-ctrl-mm-{tid}-mem')

        # Bind obi xbar so that it can communicate with fsync mmapped controller
        obi_xbar.o_MAP(fsync_mm_ctrl.i_INPUT(), RouterMapping(
            base=MagiaArch.FSYNC_CTRL_ADDR_START,
            size=MagiaArch.FSYNC_CTRL_SIZE, remove_base=True),
            name=f'fs-ctrl-mm-{tid}-mem')

        # Bind obi xbar so that it can communicate with Event-Unit mmapped controller
        obi_xbar.o_MAP(event_unit.i_INPUT(), RouterMapping(
            base=MagiaArch.EVENT_UNIT_ADDR_START,
            size=MagiaArch.EVENT_UNIT_SIZE, remove_base=True),
            name='event_unit')

        # Bind obi xbar so that it can communicate with local stack
        obi_xbar.o_MAP(l1_tcdm.i_INPUT(MagiaTileTcdm.NARROW_OBI_STACK), RouterMapping(
            base=MagiaArch.STACK_ADDR_START,
            size=MagiaArch.STACK_SIZE, remove_base=False),
            name="local-stack")

        if MagiaArch.SPATZ_ENABLE:
            # Bind obi xbar so that it can communicate with snitch spatz bootrom
            obi_xbar.o_MAP(snitch_spatz_rom.i_INPUT(), RouterMapping(
                base=MagiaArch.SPATZ_BOOTROM_ADDR,
                size=MagiaArch.SPATZ_BOOTROM_SIZE, remove_base=True),
                name="snitch-spatz-bootrom")

            obi_xbar.o_MAP(snitch_spatz_regs.i_INPUT(), RouterMapping(
                base=MagiaArch.SPATZ_CTRL_START,
                size=MagiaArch.SPATZ_CTRL_SIZE, remove_base=True),
                name="snitch-spatz-regs")

        # Bind obi xbar so that it can communicate with local L1
        obi_xbar.o_MAP(l1_tcdm.i_INPUT(MagiaTileTcdm.NARROW_OBI_L1), RouterMapping(
            base=MagiaArch.L1_ADDR_START+(tid*MagiaArch.L1_TILE_OFFSET),
            size=MagiaArch.L1_SIZE, remove_base=False,
            remove_offset=(tid*MagiaArch.L1_TILE_OFFSET)),
            name="obi-to-l1-mem-local")

        # Bind tile xbar so that it can communicate with obi xbar l1 mem
        tile_xbar.o_MAP(obi_xbar.i_INPUT(OBI_IN_AXI), RouterMapping(
            base=MagiaArch.L1_ADDR_START+(tid*MagiaArch.L1_TILE_OFFSET),
            size=MagiaArch.L1_SIZE, remove_base=False),
            name="axi-to-obi-l1-mem")

        # Bind obi xbar so that it can write to kill_module
        obi_xbar.o_MAP(self.__i_KILLER_OUTPUT(), RouterMapping(
            base=MagiaArch.TEST_END_ADDR_START,
            size=MagiaArch.TEST_END_SIZE, remove_base=False),
            name="Kill-sim-mem")

        # Bind obi xbar so that it can write to uart
        obi_xbar.o_MAP(stdout.i_INPUT(), RouterMapping(
            base=MagiaArch.STDOUT_ADDR_START,
            size=MagiaArch.STDOUT_SIZE, remove_base=False),
            name="local-uart-mem")

        # Everything that is not local goes to the tile xbar: remote tiles' L1
        # and reserved memory, plus the off-tile L2. The v1 tile declares one
        # mapping per remote tile, all bound to this same port; io_v2 allows one
        # master per slave port, so they collapse into this catch-all (the
        # routing is identical, only unmapped addresses now leave the tile
        # instead of raising a decode error).
        obi_xbar.o_MAP_DEFAULT(tile_xbar.i_INPUT(AXI_IN_OBI), name="obi2axi-off-tile")

        # Same on the AXI side: local L1 is mapped above, everything else leaves
        # the tile on the narrow NoC channel.
        tile_xbar.o_MAP_DEFAULT(self.__i_NARROW_OUTPUT(), name="axi-to-off-tile")

        # Bind NoC wide channel so that it can communicate with local L1
        self.__o_WIDE_INPUT(l1_tcdm.i_WIDE_INPUT(MagiaTileTcdm.WIDE_NOC))

        self.__o_NARROW_INPUT(tile_xbar.i_INPUT(AXI_IN_NOC))

        # Bind: cv32 core enable ports -> matching composite ports
        self.__o_ENTRY(core_cv32.i_ENTRY())
        self.__o_FETCHEN(core_cv32.i_FETCHEN())

        # Bind: idma0
        idma0.o_AXI_READ(wide_xbar.i_INPUT(0))
        idma0.o_AXI_WRITE(wide_xbar.i_INPUT(1))
        idma0.o_TCDM_READ(l1_tcdm.i_WIDE_INPUT(MagiaTileTcdm.WIDE_IDMA0_R))
        idma0.o_TCDM_WRITE(l1_tcdm.i_WIDE_INPUT(MagiaTileTcdm.WIDE_IDMA0_W))
        idma_mm_ctrl.o_OFFLOAD_iDMA0_AXI2OBI(idma0.i_OFFLOAD())
        idma0.o_OFFLOAD_GRANT(idma_mm_ctrl.i_OFFLOAD_GRANT_iDMA0_AXI2OBI())

        # Bind: idma1
        idma1.o_AXI_READ(wide_xbar.i_INPUT(2))
        idma1.o_AXI_WRITE(wide_xbar.i_INPUT(3))
        idma1.o_TCDM_READ(l1_tcdm.i_WIDE_INPUT(MagiaTileTcdm.WIDE_IDMA1_R))
        idma1.o_TCDM_WRITE(l1_tcdm.i_WIDE_INPUT(MagiaTileTcdm.WIDE_IDMA1_W))
        idma_mm_ctrl.o_OFFLOAD_iDMA1_OBI2AXI(idma1.i_OFFLOAD())
        idma1.o_OFFLOAD_GRANT(idma_mm_ctrl.i_OFFLOAD_GRANT_iDMA1_OBI2AXI())

        # Bind: the wide fan-in towards the NoC wide channel
        wide_xbar.o_MAP_DEFAULT(self.__i_WIDE_OUTPUT(), name="wide-off-tile")

        # Bind: redmule
        redmule.o_TCDM(l1_tcdm.i_WIDE_INPUT(MagiaTileTcdm.WIDE_REDMULE))

        # Bind Event unit
        self.bind(event_unit, 'clock_0', core_cv32, 'clock')
        self.bind(core_cv32, 'irq_ack', event_unit, 'irq_ack_0')
        self.bind(event_unit, 'irq_req_0', core_cv32, 'irq_req')
        self.bind(idma_mm_ctrl, 'idma0_done_irq', event_unit, 'in_event_2_pe_0')
        self.bind(idma_mm_ctrl, 'idma1_done_irq', event_unit, 'in_event_3_pe_0')
        if MagiaArch.SPATZ_ENABLE:
            self.bind(snitch_spatz_regs, 'spatz_done_irq', event_unit, 'in_event_8_pe_0')
        self.bind(redmule, 'done_irq', event_unit, 'in_event_10_pe_0')
        self.bind(fsync_mm_ctrl, 'fsync_done_irq', event_unit, 'in_event_24_pe_0')

        # Bind fractal sync ports
        fsync_mm_ctrl.o_XIF_2_FRACTAL_EAST_WEST(self.__o_SLAVE_EAST_WEST_FRACTAL())
        self.__i_SLAVE_EAST_WEST_FRACTAL(fsync_mm_ctrl.i_FRACTAL_2_XIF_EAST_WEST())

        fsync_mm_ctrl.o_XIF_2_FRACTAL_NORD_SUD(self.__o_SLAVE_NORD_SUD_FRACTAL())
        self.__i_SLAVE_NORD_SUD_FRACTAL(fsync_mm_ctrl.i_FRACTAL_2_XIF_NORD_SUD())

        fsync_mm_ctrl.o_XIF_2_NEIGHBOUR_FRACTAL_EAST_WEST(self.__o_SLAVE_EAST_WEST_NEIGHBOUR_FRACTAL())
        self.__i_SLAVE_EAST_WEST_NEIGHBOUR_FRACTAL(fsync_mm_ctrl.i_NEIGHBOUR_FRACTAL_2_XIF_EAST_WEST())

        fsync_mm_ctrl.o_XIF_2_NEIGHBOUR_FRACTAL_NORD_SUD(self.__o_SLAVE_NORD_SUD_NEIGHBOUR_FRACTAL())
        self.__i_SLAVE_NORD_SUD_NEIGHBOUR_FRACTAL(fsync_mm_ctrl.i_NEIGHBOUR_FRACTAL_2_XIF_NORD_SUD())

        # Enable debug
        gdbserver.gdbserver.Gdbserver(self, 'gdbserver')


    # east west port to fractalsync
    def __o_SLAVE_EAST_WEST_FRACTAL(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'xif_2_east_west_fractal', signature='wire<PortReq<uint32_t>*>')

    def o_SLAVE_EAST_WEST_FRACTAL(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('xif_2_east_west_fractal', itf, signature='wire<PortReq<uint32_t>*>')

    def __i_SLAVE_EAST_WEST_FRACTAL(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('east_west_fractal_2_xif', itf, signature='wire<PortResp<uint32_t>*>',composite_bind=True)

    def i_SLAVE_EAST_WEST_FRACTAL(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'east_west_fractal_2_xif', signature='wire<PortResp<uint32_t>*>')

    # nord sud to fractalsync
    def __o_SLAVE_NORD_SUD_FRACTAL(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'xif_2_nord_sud_fractal', signature='wire<PortReq<uint32_t>*>')

    def o_SLAVE_NORD_SUD_FRACTAL(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('xif_2_nord_sud_fractal', itf, signature='wire<PortReq<uint32_t>*>')

    def __i_SLAVE_NORD_SUD_FRACTAL(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('nord_sud_fractal_2_xif', itf, signature='wire<PortResp<uint32_t>*>',composite_bind=True)

    def i_SLAVE_NORD_SUD_FRACTAL(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'nord_sud_fractal_2_xif', signature='wire<PortResp<uint32_t>*>')

    # east west port to neighbour fractalsync
    def __o_SLAVE_EAST_WEST_NEIGHBOUR_FRACTAL(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'xif_2_east_west_neighbour_fractal', signature='wire<PortReq<uint32_t>*>')

    def o_SLAVE_EAST_WEST_NEIGHBOUR_FRACTAL(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('xif_2_east_west_neighbour_fractal', itf, signature='wire<PortReq<uint32_t>*>')

    def __i_SLAVE_EAST_WEST_NEIGHBOUR_FRACTAL(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('east_west_neighbour_fractal_2_xif', itf, signature='wire<PortResp<uint32_t>*>',composite_bind=True)

    def i_SLAVE_EAST_WEST_NEIGHBOUR_FRACTAL(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'east_west_neighbour_fractal_2_xif', signature='wire<PortResp<uint32_t>*>')

    # nord sud to neighbour fractalsync
    def __o_SLAVE_NORD_SUD_NEIGHBOUR_FRACTAL(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'xif_2_nord_sud_neighbour_fractal', signature='wire<PortReq<uint32_t>*>')

    def o_SLAVE_NORD_SUD_NEIGHBOUR_FRACTAL(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('xif_2_nord_sud_neighbour_fractal', itf, signature='wire<PortReq<uint32_t>*>')

    def __i_SLAVE_NORD_SUD_NEIGHBOUR_FRACTAL(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('nord_sud_neighbour_fractal_2_xif', itf, signature='wire<PortResp<uint32_t>*>',composite_bind=True)

    def i_SLAVE_NORD_SUD_NEIGHBOUR_FRACTAL(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'nord_sud_neighbour_fractal_2_xif', signature='wire<PortResp<uint32_t>*>')

    # Output (master) narrow ports to off-tile L2 memory. The NoC narrow channel
    # is beat-native at its own width, so the composite ports carry the beat
    # signature (a mismatch would make the framework insert a useless adapter).
    def o_NARROW_OUTPUT(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('narrow_output', itf, signature=IoV2Beat(MagiaArch.BYTES_PER_WORD))

    def __i_NARROW_OUTPUT(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'narrow_output',
            signature=IoV2Beat(MagiaArch.BYTES_PER_WORD))

    def i_NARROW_INPUT(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'narrow_input',
            signature=IoV2Beat(MagiaArch.BYTES_PER_WORD))

    def __o_NARROW_INPUT(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('narrow_input', itf, signature=IoV2Beat(MagiaArch.BYTES_PER_WORD),
            composite_bind=True)

    # Output (master) wide port to off-tile L2 memory
    def o_WIDE_OUTPUT(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('wide_output', itf, signature=IoV2Beat(MagiaArch.TILE_WIDE_WIDTH))

    def __i_WIDE_OUTPUT(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'wide_output', signature=IoV2Beat(MagiaArch.TILE_WIDE_WIDTH))

    def i_WIDE_INPUT(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'wide_input', signature=IoV2Beat(MagiaArch.TILE_WIDE_WIDTH))

    def __o_WIDE_INPUT(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('wide_input', itf, signature=IoV2Beat(MagiaArch.TILE_WIDE_WIDTH),
            composite_bind=True)

    # Input port for the loader.
    #
    # The declared width is not cosmetic: behind this port sits the OBI beat
    # plane, whose beats are BYTES_PER_WORD wide, so the largest granule a
    # single access may cover really is one word. The ELF loader issues writes
    # of up to 64 KiB (its internal MAX_CHUNK) and declares no width of its
    # own, so the framework inserts an IoV2SingleReqWidthAdapter here, which
    # splits every section chunk into word-aligned sub-accesses.
    #
    # Without it, a wide loader write reaches the beat x-bar as a single
    # over-width write beat and is rejected with IO_RESP_INVALID (router_v2's
    # beat flavour checks `is_write && size > width`), which shows up as
    # "Received error during copy" from the loader — the binary never lands.
    # The single-req-to-beat adapter does not chop wide writes itself: it
    # forwards them as one beat (it does reassemble wide *reads*), so the
    # width has to be declared where the narrow plane starts.
    def i_LOADER(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'loader',
            signature=IoV2SingleReq(width=MagiaArch.BYTES_PER_WORD))

    def __o_LOADER(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('loader', itf,
            signature=IoV2SingleReq(width=MagiaArch.BYTES_PER_WORD),
            composite_bind=True)

    def i_FETCHEN(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'fetchen', signature='wire<bool>')

    def __o_FETCHEN(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('fetchen', itf, signature='wire<bool>', composite_bind=True)

    def i_ENTRY(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'entry', signature='wire<uint64_t>')

    def __o_ENTRY(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('entry', itf, signature='wire<uint64_t>', composite_bind=True)

    # Killer port
    def o_KILLER_OUTPUT(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('killer_output', itf, signature=IoV2SingleReq())

    def __i_KILLER_OUTPUT(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'killer_output', signature=IoV2SingleReq())
