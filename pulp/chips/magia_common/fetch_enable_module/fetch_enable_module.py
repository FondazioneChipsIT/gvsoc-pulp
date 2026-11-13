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

class FetchEnableModule(gvsoc.systree.Component):

    def __init__(self,
                parent: gvsoc.systree.Component,
                name: str,
                fetch_en_addr_base: int,
                fetch_en_addr_size: int,
                fetch_en_pattern: int = 0xFFFFFFFF,
                poll_period: int = 1000,
                poll_sleep_us: int = 1000,
                memfd: str = '',
                uma_host_base: int = 0,
                entry_addr: int = 0):
        """fetch_en_pattern: value that enables the fetch when read at fetch_en_addr_base.
        poll_period: cycles between two reads of the fetch enable word.
        poll_sleep_us: wall-clock pause after each unsuccessful read (0 = none), bounds
        the number of reads while the host boots.
        memfd: shared memory file (host RAM, e.g. /tmp/magia_uma). When set, the module
        maps by itself only the page holding the fetch enable word and polls it.
        Empty disables the polling (e.g. PCIe VFIO mode).
        uma_host_base: host address of memfd offset 0 (e.g. 0xA000_0000), the word is
        at memfd offset fetch_en_addr_base - uma_host_base.
        entry_addr: host address of a 32-bit word holding the entry point, written by the
        host before the fetch enable pattern. When not 0, the module reads it once the
        pattern is found and sends it on the entry port (o_ENTRY) before raising fetch
        enable. 0 disables it (entry provided by someone else, e.g. the ELF loader)."""

        super().__init__(parent, name)

        self.add_properties({
            'fetch_en_addr_base' : fetch_en_addr_base,
            'fetch_en_addr_size' : fetch_en_addr_size,
            'fetch_en_pattern' : fetch_en_pattern,
            'poll_period' : poll_period,
            'poll_sleep_us' : poll_sleep_us,
            'memfd' : memfd,
            'uma_host_base' : uma_host_base,
            'entry_addr' : entry_addr,
        })

        self.add_sources(['pulp/chips/magia_common/fetch_enable_module/fetch_enable_module.cpp'])

    def i_INPUT(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'input', signature='io')

    def o_FETCH_ENABLE(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('fetch_en', itf, signature='wire<bool>')

    def o_ENTRY(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('entry', itf, signature='wire<uint64_t>')
