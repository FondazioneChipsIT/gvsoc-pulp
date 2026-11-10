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
                poll_sleep_us: int = 1000):
        """fetch_en_pattern: value that enables the fetch when read at fetch_en_addr_base.
        poll_period: cycles between two reads of the fetch enable word (through o_MEM).
        poll_sleep_us: wall-clock pause after each unsuccessful read (0 = none), bounds
        the number of reads while the host boots."""

        super().__init__(parent, name)

        self.add_properties({
            'fetch_en_addr_base' : fetch_en_addr_base,
            'fetch_en_addr_size' : fetch_en_addr_size,
            'fetch_en_pattern' : fetch_en_pattern,
            'poll_period' : poll_period,
            'poll_sleep_us' : poll_sleep_us,
        })

        self.add_sources(['pulp/chips/magia_v2/fetch_enable_module/fetch_enable_module.cpp'])

    def i_INPUT(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'input', signature='io')

    def o_MEM(self, itf: gvsoc.systree.SlaveItf):
        """Memory polled for the fetch enable word (L2, shared with the host)."""
        self.itf_bind('mem', itf, signature='io')

    def o_FETCH_ENABLE(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('fetch_en', itf, signature='wire<bool>')