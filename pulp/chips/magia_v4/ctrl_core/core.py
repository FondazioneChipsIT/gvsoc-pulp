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
from cpu.iss.isa_gen.isa_smallfloats import Xf16, Xf16alt
from pulp.cpu.iss.cv32e40p_v2 import Cv32e40p, Cv32e40pConfig


# magia-v4 control core: CV32E40P on the iss_v2 modular core (Marco Paci's
# model). Same FP recipe as the previous v1 core (zfinx + the half-float
# extensions Xf16/Xf16alt) plus the PULP/CoreV extensions. Smallfloats are
# passed as extra_extensions so the Cv32e40p wrapper emits them before
# CoreV2 (rvXf16.hpp pulls in the iss_v2 macros that pulp_v2.hpp needs).
class CV32CtrlCore(Cv32e40p):

    # Own ISA-cache tag: the magia cores add the half-float extensions on top
    # of the vanilla cv32e40p_v2 ISA, so they must not share its cache entry.
    isa_name: str = 'magia_cv32e40p'

    def __init__(self, parent: gvsoc.systree.Component, name: str, binaries: list=[],
                 fetch_enable: bool=False, boot_addr: int=0, timed: bool=True,
                 core_id: int=0):

        config = Cv32e40pConfig(isa='rv32imfc', boot_addr=boot_addr,
                                hart_id=core_id, fetch_enable=fetch_enable,
                                htif=False)

        # irq_external=True -> PULP vectored irq_req/irq_ack handshake + event
        # load (cv.elw): the magia programming model has the ctrl core wait for
        # events from the tile's PULP event unit via cv.elw, so it needs the
        # IrqExternal personality (like the old v1 ctrl core, riscv_exceptions
        # =False). io_v2=False keeps the io_v1 memory ports.
        super().__init__(parent, name, config=config,
                         fpu=False, zfinx=True, pulp=True,
                         extra_extensions=[Xf16(), Xf16alt()],
                         io_v2=False, irq_external=True, elw=True)
