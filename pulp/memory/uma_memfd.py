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

import gvsoc.systree


class UmaMemfd(gvsoc.systree.Component):
    """UMA backing of an io_v2 memory (memory.memory_v3).

    Maps a window of a shared memory file (memfd, e.g. the host RAM of QEMU)
    and gives it, through the meminfo port, to the memory as its backing
    array, so that the memory and the host share the same data.

    The memfd offset 0 is at host address ``host_base`` and the memory is seen
    at ``window_start`` (e.g. the MAGIA L2 base): the window starts at memfd
    offset ``window_start - host_base`` and covers ``size`` bytes. Memory
    offset x is therefore host address ``window_start + x``, so the host and
    the memory use the same physical addresses.

    Attributes
    ----------
    parent: gvsoc.systree.Component
        The parent component where this one should be instantiated.
    name: str
        The name of the component within the parent space.
    memfd: str
        Path of the shared memory file (e.g. /tmp/magia_uma).
    host_base: int
        Host address of memfd offset 0 (start of the host RAM).
    window_start: int
        Address at which the memory is mapped in the system, >= host_base.
    size: int
        Size of the window, the size of the memory.
    """

    def __init__(self, parent: gvsoc.systree.Component, name: str, memfd: str,
            host_base: int, window_start: int, size: int):

        super().__init__(parent, name)

        if window_start < host_base:
            raise RuntimeError(f'UMA window start ({window_start:#x}) below the host RAM '
                f'base ({host_base:#x})')

        self.add_sources(['pulp/memory/uma_memfd.cpp'])

        self.add_properties({
            'memfd': memfd,
            'host_base': host_base,
            'window_start': window_start,
            'size': size,
        })

    def o_MEMINFO(self, itf: gvsoc.systree.SlaveItf):
        """Binds the meminfo port to the memory (memory_v3 ``i_MEMINFO``)."""
        self.itf_bind('meminfo', itf, signature='wire<void *>')
