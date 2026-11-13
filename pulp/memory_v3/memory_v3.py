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

"""UMA variant of the io_v2 memory.

Copy of ``memory.memory_v3`` (gvsoc/core/models/memory/memory_v3.py) whose
backing array can be mapped from a shared memory file (memfd, e.g. the host
RAM of QEMU) instead of being allocated, with the memfd handling of
``pulp.memory.memory``. Without ``memfd`` it behaves as ``memory.memory_v3``.

The config struct is :class:`UmaMemoryV3Config` and not ``MemoryV3Config``,
since the config structs of all the components of a target are compiled in the
same file: it has all the fields of ``MemoryV3Config`` plus the UMA ones.
"""

from __future__ import annotations

from typing import Annotated

import gvsoc.systree
import gvrun.timing
from config_tree import Config, cfg_field, HasSize
from gvrun.runtime import Runtime
from gvsoc.signature import IoV2Sync
from memory.memory_v3 import MemoryV3PowerConfig
from vp.power_config import consume_power_model


class UmaMemoryV3Config(Config, HasSize):
    """Configuration of the UMA io_v2 ``Memory``.

    Same fields as ``memory.memory_v3.MemoryV3Config`` (see there), plus:

    Attributes
    ----------
    memfd: str
        Path of the shared memory file (e.g. /tmp/magia_uma). When set, the
        backing array is mapped from it (no allocation, no poisoning).
        Empty string for a private memory.
    uma_host_base: int
        Host address of memfd offset 0 (start of the host RAM), must be <=
        uma_window_start.
    uma_window_start: int
        Address at which the memory is seen in the system (e.g. the MAGIA L2
        base). The memfd is mapped from offset uma_window_start -
        uma_host_base, so that the host and the memory use the same physical
        addresses. Requests in [uma_window_start, + uma_window_size) are
        converted to memory offsets, other addresses are used as they are.
    uma_window_size: int
        Size of the window above (normally the memory size). 0 disables the
        conversion and maps the memfd from offset 0.
    """

    size: int = cfg_field(default=0, fmt="hex", dump=True, desc=(
        "Memory size in bytes"
    ))

    atomics: bool = cfg_field(default=False, dump=True, desc=(
        "True if the memory should support riscv atomics"
    ))

    latency: int = cfg_field(default=1, dump=True, desc=(
        "Specify extra latency which will be added to any incoming request"
    ))

    write_latency: int = cfg_field(default=-1, dump=True, desc=(
        "Latency of the write requests, -1 (default) for the one of the other requests"
    ))

    truncate: int = cfg_field(default=True, dump=True, desc=(
        "If true, this truncates the global input address with the "
        "memory size to make it relative to the memory"
    ))

    align: int = cfg_field(default=0, dump=True, desc=(
        "Alignment in bytes requested from aligned_alloc for the backing buffer"
    ))

    check: bool = cfg_field(default=False, dump=True, desc=(
        "Allocate a bitmap for tracking uninitialised accesses"
    ))

    init: bool = cfg_field(default=True, dump=True, desc=(
        "True to poison the backing buffer with 0x57 bytes at construction "
        "(never done on the memfd mapping)"
    ))

    stim_file: Annotated[str, Runtime] = cfg_field(default="", dump=True, desc=(
        "Path to a raw-binary file preloaded into the backing buffer "
        "(empty string means no preload)"
    ))

    power_trigger: bool = cfg_field(default=False, dump=True, desc=(
        "Enable power-capture start/stop triggers on magic writes to offset 0"
    ))

    power_model: str = cfg_field(default="", desc=(
        "Path of a YAML power model file, resolved from PYTHONPATH. Empty = no "
        "power numbers (all sources inert)."
    ))

    power: MemoryV3PowerConfig = cfg_field(default_factory=MemoryV3PowerConfig,
        init=False, desc=(
        "Per-source power tables, filled from 'power_model'."
    ))

    memfd: str = cfg_field(default="", dump=True, desc=(
        "Shared memory file backing the memory (UMA), empty for a private memory"
    ))

    uma_host_base: int = cfg_field(default=0, fmt="hex", dump=True, desc=(
        "Host address of memfd offset 0 (start of the host RAM)"
    ))

    uma_window_start: int = cfg_field(default=0, fmt="hex", dump=True, desc=(
        "Address at which the memory is seen in the system"
    ))

    uma_window_size: int = cfg_field(default=0, fmt="hex", dump=True, desc=(
        "Size of the UMA window, 0 to map the memfd from offset 0"
    ))


class Memory(gvsoc.systree.Component):
    """UMA io_v2 memory: ``memory.memory_v3.Memory`` with a memfd backing.

    Same ports and behaviour as ``memory.memory_v3.Memory``.

    Parameters
    ----------
    parent : Component
        Parent component this memory is instantiated under.
    name : str
        Local name of the memory within ``parent``.
    config : UmaMemoryV3Config
        Full configuration.
    """

    def __init__(self, parent: gvsoc.systree.Component, name: str,
                 config: UmaMemoryV3Config):
        super().__init__(parent, name, config=config)

        level = self.get_timing_level(
            supported=[gvrun.timing.FUNCTIONAL, gvrun.timing.TIMED])
        if level == gvrun.timing.FUNCTIONAL:
            config.latency = 0
            self.add_property('latency', 0)
        self.add_property('timing_level', level)

        if config.memfd and config.uma_window_size > 0 and \
                config.uma_window_start < config.uma_host_base:
            raise RuntimeError(f'UMA window start ({config.uma_window_start:#x}) below the '
                f'host RAM base ({config.uma_host_base:#x})')

        self.add_sources(['pulp/memory_v3/memory_v3.cpp'])

        if config.atomics:
            self.add_c_flags(['-DCONFIG_ATOMICS=1'])

    def configure(self):
        consume_power_model(self.get_config())

    def i_INPUT(self) -> gvsoc.systree.SlaveItf:
        """Returns the io_v2 input port (see ``memory.memory_v3.Memory.i_INPUT``)."""
        return gvsoc.systree.SlaveItf(self, 'input', signature=IoV2Sync())

    def i_MEMINFO(self) -> gvsoc.systree.SlaveItf:
        """Returns the port giving the backing array of the memory."""
        return gvsoc.systree.SlaveItf(self, 'meminfo', signature='wire<void *>')

    def gen_gui(self, parent_signal):
        import gvsoc.gui
        top = gvsoc.gui.Signal(self, parent_signal, name=self.name, path="req_addr",
            groups=['regmap', 'memory', 'active'])

        gvsoc.gui.Signal(self, top, "req_size",     path="req_size",     groups=['regmap'])
        gvsoc.gui.Signal(self, top, "req_is_write", path="req_is_write", groups=['regmap'])
