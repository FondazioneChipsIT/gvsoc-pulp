#
# Copyright (C) 2026 Fondazione Chips-IT
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
# Authors: Lorenzo Zuolo, Fondazione Chips-IT (lorenzo.zuolo@chips.it)
#

import gvsoc.systree


class iDMA_mm_ctrl(gvsoc.systree.Component):
    """
    Memory-mapped window on the iDMA register files of a MAGIA tile

    The tile has two independent transfer channels, one per direction, each with its own reg32_3d
    register file. This forwards an access to the register file of the channel the address selects,
    the equivalent of idma_obi_ctrl_decoder in the RTL, and turns the completion a channel reports
    into the interrupt line of the tile.

    Channel 0 is AXI2OBI, moving data from L2 to L1, and sits in the lower half of the window.
    Channel 1 is OBI2AXI and sits above the direction offset.
    """

    def __init__(self,
                parent: gvsoc.systree.Component,
                name: str):

        super().__init__(parent, name)

        self.add_sources(['pulp/chips/magia_v3/idma_mm_ctrl/idma_mm_ctrl.cpp'])

    def i_INPUT(self) -> gvsoc.systree.SlaveItf:
        """Returns the memory-mapped window covering both channels."""
        return gvsoc.systree.SlaveItf(self, 'input', signature='io')

    def o_CFG(self, channel: int, itf: gvsoc.systree.SlaveItf):
        """Binds the configuration port of one channel to its register file.

        Parameters
        ----------
        channel: int
            0 for AXI2OBI, 1 for OBI2AXI.
        itf: gvsoc.systree.SlaveItf
            Slave interface
        """
        self.itf_bind(f'cfg_{channel}', itf, signature='io')

    def i_DONE(self, channel: int) -> gvsoc.systree.SlaveItf:
        """Returns the port where one channel reports a completed transfer.

        Parameters
        ----------
        channel: int
            0 for AXI2OBI, 1 for OBI2AXI.
        """
        return gvsoc.systree.SlaveItf(self, f'done_{channel}', signature='wire<bool>')

    def o_IRQ_DMA0(self, itf: gvsoc.systree.SlaveItf):
        """Binds the interrupt line of the AXI2OBI channel."""
        self.itf_bind('idma0_done_irq', itf, signature='wire<bool>')

    def o_IRQ_DMA1(self, itf: gvsoc.systree.SlaveItf):
        """Binds the interrupt line of the OBI2AXI channel."""
        self.itf_bind('idma1_done_irq', itf, signature='wire<bool>')
