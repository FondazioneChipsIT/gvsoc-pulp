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


# magia-v4 PULP cluster core: CV32E40P on the iss_v2 modular core (Marco Paci's
# model). Same FP recipe as the control core (zfinx + Xf16/Xf16alt + CoreV).
#
# Same personality as the control core too: with the cluster Event Unit
# (magia_cluster_wrap.sv) every synchronisation goes through the EU, and the
# cores park on it with cv.elw -- hence elw=True and irq_external=True (the
# irq_req/irq_ack + clock-gating handshake the event-unit model drives). In RTL
# the EU cause is OR'd onto irq_i[11] with the ack tied to 0 and no software
# ever unmasks it, so the vectored handshake is only a modelling detail.
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
                         io_v2=False, irq_external=True, elw=True)
