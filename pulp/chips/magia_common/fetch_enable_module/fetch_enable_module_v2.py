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



# Authors: Riccardo Repetto, Chips-IT (riccardo.repetto@chips.it)

import gvsoc.systree
from gvsoc.signature import IoV2Sync

class FetchEnableModuleV2(gvsoc.systree.Component):
    """Fetch enable module reading the host words through the L2 (io_v2).

    Same behaviour as FetchEnableModule, but the fetch enable word and the entry
    word are read through the ``mem`` port (o_MEM), bound to the L2 memory, instead
    of being mapped from the memfd: the module does not know about the memfd. The
    addresses are the absolute ones (e.g. 0xCCFF_0400): the port must be bound
    to a synchronous remapper stripping the L2 base address (interco.remapper_v2).
    """

    def __init__(self,
                parent: gvsoc.systree.Component,
                name: str,
                fetch_en_addr_base: int,
                fetch_en_addr_size: int,
                fetch_en_pattern: int = 0x1A2B3C4E,
                poll_period: int = 1000,
                entry_addr: int = 0):
        """fetch_en_pattern: value that enables the fetch when read at fetch_en_addr_base.
        poll_period: cycles between two reads of the fetch enable word (through o_MEM).
        entry_addr: address of a 32-bit word holding the entry point, written by the
        host before the fetch enable pattern. When not 0, the module reads it (through
        o_MEM) once the pattern is found and sends it on the entry port (o_ENTRY) before
        raising fetch enable. 0 disables it (entry provided by someone else, e.g. the
        ELF loader).
        The module polls only if o_MEM is bound (e.g. not in PCIe VFIO mode)."""

        super().__init__(parent, name)

        self.add_properties({
            'fetch_en_addr_base' : fetch_en_addr_base,
            'fetch_en_addr_size' : fetch_en_addr_size,
            'fetch_en_pattern' : fetch_en_pattern,
            'poll_period' : poll_period,
            'entry_addr' : entry_addr,
        })

        self.add_sources(['pulp/chips/magia_common/fetch_enable_module/fetch_enable_module_v2.cpp'])

    def o_MEM(self, itf: gvsoc.systree.SlaveItf):
        """Memory holding the fetch enable and entry words (L2, shared with the host)."""
        self.itf_bind('mem', itf, signature=IoV2Sync())

    def o_FETCH_ENABLE(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('fetch_en', itf, signature='wire<bool>')

    def o_ENTRY(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('entry', itf, signature='wire<uint64_t>')
