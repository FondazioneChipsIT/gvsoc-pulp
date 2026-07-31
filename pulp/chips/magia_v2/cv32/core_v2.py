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

"""io_v2 CV32 control core for the magia_v2 tile.

Sibling of :mod:`pulp.chips.magia_v2.cv32.core` built on the **iss_v2** ISS
(``cpu.iss_v2.riscv``) instead of the v1 one, so that its fetch and data ports
speak the io_v2 protocol (``IoV2SingleReq``).

What changes with respect to the v1 core, and why:

- **Module-based composition.** iss_v2 assembles the core from explicit
  modules instead of a ``CONFIG_ISS_CORE_DIR`` + ``class.hpp`` include set. The
  CV32-class modules already exist in-tree as the ``ri5ky`` family
  (``pulp.cpu.iss.ri5ky``): ``Ri5kyExec`` (in-order commit), ``Ri5kyLsu``
  (io_v2 LSU with the ``p.elw`` park/wake path), ``Ri5kyCsr`` (PULP PCCR/PCER
  counters) and ``Ri5kyEvent``, plus the generic ``Hwloop`` module that
  replaces the v1 ``CONFIG_GVSOC_ISS_HWLOOP`` flag. That is exactly the set the
  v1 core built by hand through its ``class.hpp``.
- **Same ISA.** ``rv32imfc`` + ``Xf16alt`` + ``Xf16`` + ``PulpV2`` (hwloop and
  ``p.elw`` enabled, which are PulpV2's defaults), ``zfinx``, same ``misa`` and
  same debug handler as the v1 core. The smallfloat subsets are listed before
  PulpV2 because ``pulp_v2.hpp`` relies on macros pulled in by the
  ``rvXf16.hpp`` header.
- **No ``o_DATA_DEBUG`` port.** In iss_v2 the gdbserver reaches memory through
  the out-of-band ``debug_mem`` backdoor resolved from the data port, so there
  is no second data port to bind (``tile_v2.py`` binds one data port only).
- **HTIF off**, as in the v1 core: magia reports through its own stdout
  peripheral and kill module, not through a host interface.

One ISA instance is shared by every core of the chip (16 tiles would otherwise
each regenerate the same ISA sources).
"""

from gvsoc.systree import Component

from cpu.iss_v2.riscv import (Arch, Hwloop, IrqExternal, IssModule, Regfile,
                              RiscvCommon)
from cpu.iss.isa_gen.isa_gen import Isa
from cpu.iss.isa_gen.isa_riscv_gen import RiscvIsa
from cpu.iss.isa_gen.isa_pulpv2 import PulpV2
from cpu.iss.isa_gen.isa_smallfloats import Xf16, Xf16alt
from pulp.cpu.iss.ri5ky import (Ri5kyConfig, Ri5kyCsr, Ri5kyEvent, Ri5kyExec,
                                Ri5kyLsu)


# ISA instances shared across cores, keyed by ISA string.
_isa_instances: dict[str, Isa] = {}


class CV32CoreTest(RiscvCommon):
    """Basic rv32 control core of a magia tile, on the io_v2 protocol."""

    def __init__(self, parent: Component, name: str, binaries: list=[],
                 fetch_enable: bool=False, boot_addr: int=0, timed: bool=True,
                 core_id: int=0):

        # Properties (same values as the v1 core)
        isa_str = 'rv32imfc'
        misa = 0x40000000
        debug_handler = 0x1a190800
        fetch_enable = False
        riscv_exceptions = False
        zfinx = True

        isa = _isa_instances.get(isa_str)
        if isa is None:
            # Extension order matters here, and it is NOT the v1 order.
            #
            # 'rvXf16alt.hpp' uses the float helpers (float_madd_16alt, ...) but
            # does not include 'isa_lib/float.h' itself: on iss v1 it got them
            # through 'iss_core.hpp', which its CONFIG_GVSOC_ISS_V2 branch does
            # not include. 'rvXf16.hpp' is the header that pulls float.h in, so
            # Xf16 must come before Xf16alt or the build fails with
            # "'float_madd_16alt' was not declared in this scope". PulpV2 comes
            # last because 'pulp_v2.hpp' relies on macros the smallfloat headers
            # bring in. Same order as pulp.cpu.iss.ri5ky and Spatz.
            #
            # The two subsets have disjoint encodings (the 'ah' vs 'h' format
            # field), so the order has no effect on decoding.
            isa = RiscvIsa(f'cv32-base-io-v2_{isa_str}', isa_str,
                extensions=[Xf16(), Xf16alt(), PulpV2(hwloop=True, elw=True)])
            _isa_instances[isa_str] = isa

        config = Ri5kyConfig(isa=isa_str, fetch_enable=fetch_enable,
            boot_addr=boot_addr, hart_id=core_id, htif=False)

        modules: dict[str, IssModule] = {
            'arch': Arch('Ri5ky', source='cpu/iss_v2/src/cores/ri5ky/ri5ky.cpp'),
            # External interrupt controller: the tile's event unit drives the
            # 'irq_req' / 'irq_ack' wire pair, exactly as in the v1 tile (the
            # alternative 'Irq' module implements the RISC-V CLINT-style
            # per-line i_IRQ(n) wires and forces riscv exceptions on).
            'irq': IrqExternal(),
            'event': Ri5kyEvent(),
            'csr': Ri5kyCsr(),
            'exec': Ri5kyExec(),
            # Single-issue at the request level, like the v1 core (which had
            # PIPELINE_STALL_THRESHOLD=1).
            'lsu': Ri5kyLsu(nb_outstanding=1),
            'regfile': Regfile(scoreboard=True),
            'hwloop': Hwloop(),
        }

        super().__init__(parent, name, config=config, isa=isa, modules=modules,
                         misa=misa, debug_handler=debug_handler,
                         riscv_exceptions=riscv_exceptions, zfinx=zfinx,
                         timed=timed, binaries=binaries)
