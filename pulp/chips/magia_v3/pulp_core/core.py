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


# magia-v3 PULP core: CV32E40P on the iss_v2 modular core (Marco Paci's
# model). Same FP recipe as the control core (zfinx + Xf16/Xf16alt + CoreV),
# but keeps the RISC-V interrupt model (Cv32e40pIrq): these cores are driven
# through i_IRQ(11)=mei, not the PULP event-unit handshake, so irq_external
# stays False (the old v1 pulp core used riscv_exceptions=True). That makes
# the irq module differ from the control core, hence a distinct ISA instance.
class CV32PulpCore(Cv32e40p):

    isa_name: str = 'magia_cv32e40p'

    def __init__(self, parent: gvsoc.systree.Component, name: str, binaries: list=[],
                 fetch_enable: bool=False, boot_addr: int=0, timed: bool=True,
                 core_id: int=0):

        config = Cv32e40pConfig(isa='rv32imfc', boot_addr=boot_addr,
                                hart_id=core_id, fetch_enable=fetch_enable,
                                htif=False)

        super().__init__(parent, name, config=config,
                         fpu=False, zfinx=True, pulp=True,
                         extra_extensions=[Xf16(), Xf16alt()],
                         io_v2=False)
