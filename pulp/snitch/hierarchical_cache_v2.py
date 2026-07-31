#
# Copyright (C) 2020 GreenWaves Technologies, SAS, ETH Zurich and University of Bologna
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# Authors: Lorenzo Zuolo, Chips-IT (lorenzo.zuolo@chips.it)
#          - io_v2 sibling, rebuilt on cache_v4

import gvsoc.systree
from cache.cache_v4 import Cache, CacheConfig
from gvsoc.signature import IoV2SingleReq
from utils.common_cells import And


class Hierarchical_cache(gvsoc.systree.Component):
    """io_v2 sibling of :class:`pulp.snitch.hierarchical_cache.Hierarchical_cache`.

    Same hierarchy — one private L0 per core in front of one shared L1 — rebuilt
    on ``cache.cache_v4`` (the io_v2 cache leaf).

    Two structural differences, both behaviour-preserving:

    - **Geometry is expressed in bytes.** The values below are the exact
      translation of the v1 ``nb_sets_bits`` / ``nb_ways_bits`` /
      ``line_size_bits`` ones (L0: 1 set x 1 way x 32 B = 32 B; L1: 128 sets x
      2 ways x 32 B = 8 KiB).
    - **No L1 interleaver.** With ``nb_l1_banks == 1`` the v1
      ``interco.interleaver`` was a pure pass-through, so the L0 refill ports
      bind the L1 input directly.
    """

    def __init__(self, parent: gvsoc.systree.Component, name: str, nb_cores: int=0, has_cc: int=0, l1_line_size_bits: int=7):

        super().__init__(parent, name)

        #
        # Properties
        #

        self.add_property('nb_cores', nb_cores)
        self.add_property('has_cc', has_cc)
        self.add_property('l1_line_size_bits', l1_line_size_bits)

        nb_l1_banks = 1
        nb_pes = nb_cores - 1 if has_cc else nb_cores

        # v1 geometry, translated to bytes
        l0_line_size = 1 << 5           # line_size_bits = 5
        l0_sets      = 1 << 0           # nb_sets_bits   = 0
        l0_ways      = 1 << 0           # nb_ways_bits   = 0
        l1_line_size = 1 << 5           # line_size_bits = 5
        l1_sets      = 1 << 7           # nb_sets_bits   = 7
        l1_ways      = 1 << 1           # nb_ways_bits   = 1

        #
        # Components
        #

        # L0 caches
        l0_caches = []
        for i in range(0, nb_pes):
            l0_caches.append(Cache(self, 'l0_bank%d' % i, config=CacheConfig(
                size=l0_sets * l0_ways * l0_line_size, line_size=l0_line_size,
                ways=l0_ways, refill_latency=0, enabled=True)))

        # L1 caches
        l1_caches = []
        for i in range(0, nb_l1_banks):
            l1_caches.append(Cache(self, 'l1_bank%d' % i, config=CacheConfig(
                size=l1_sets * l1_ways * l1_line_size, line_size=l1_line_size,
                ways=l1_ways, refill_latency=2, enabled=True)))

        # Use an And to gather flush ack from all banks and report a single signal outside
        flush_ack = And(self, 'flush_ack', nb_input=nb_l1_banks+nb_cores)

        #
        # Bindings
        #

        # L0 caches
        for i in range(0, nb_cores):
            self.__o_INPUT(i, l0_caches[i].i_INPUT())
            # Single L1 bank: refill goes straight to it (the v1 interleaver was
            # a pass-through in this configuration).
            l0_caches[i].o_REFILL(l1_caches[0].i_INPUT())
            self.bind(self, 'enable', l0_caches[i], 'enable')
            self.bind(self, 'flush', l0_caches[i], 'flush')

        # L1 cache
        for i in range(0, nb_l1_banks):
            l1_caches[i].o_REFILL( self.__i_REFILL())
            self.bind(self, 'enable', l1_caches[i], 'enable')
            self.__o_FLUSH( l1_caches[i].i_FLUSH() )

        # Flush ack
        for i in range(0, nb_cores):
            self.bind(l0_caches[i], 'flush_ack', flush_ack, f'input_{i}')
        for i in range(0, nb_l1_banks):
            self.bind(l1_caches[i], 'flush_ack', flush_ack, f'input_{i + nb_cores}')

        flush_ack.o_OUTPUT( self.__i_FLUSH_ACK() )

    def __i_REFILL(self) -> gvsoc.systree.SlaveItf:
            return gvsoc.systree.SlaveItf(self, 'refill', signature=IoV2SingleReq())

    def o_REFILL(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('refill', itf, signature=IoV2SingleReq())

    def i_INPUT(self, id:int ) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, f'input_{id}', signature=IoV2SingleReq())

    def __o_INPUT(self, id: int, itf: gvsoc.systree.SlaveItf):
        self.itf_bind(f'input_{id}', itf, signature=IoV2SingleReq(), composite_bind=True)

    def i_FLUSH(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'flush', signature='wire<bool>')

    def __o_FLUSH(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('flush', itf, signature='wire<bool>', composite_bind=True)

    def __i_FLUSH_ACK(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'flush_ack', signature='wire<bool>')

    def o_FLUSH_ACK(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('flush_ack', itf, signature='wire<bool>')
