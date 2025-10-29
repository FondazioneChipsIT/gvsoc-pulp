#
# Copyright (C) 2024 ETH Zurich and University of Bologna
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

# Author: Chi Zhang <chizhang@iis.ee.ethz.ch>
#         Lorenzo Zuolo, Chips-IT <lorenzo.zuolo@chips.it>

import gvsoc.systree

class LightRedmule(gvsoc.systree.Component):

    def __init__(self,
                parent: gvsoc.systree.Component,
                name: str,
                tcdm_bank_width: int,
                tcdm_bank_number: int,
                elem_size: int,
                ce_height: int,
                ce_width: int,
                ce_pipe: int,
                queue_depth: int=128,
                fold_tiles_mapping: int=0,
                loc_base=0): #here we might add also local size to check that tcdm requests do not overflow...

        super().__init__(parent, name)

        self.add_sources(['pulp/light_redmule/light_redmule.cpp'])

        self.add_properties({
            'tcdm_bank_width'   : tcdm_bank_width,
            'tcdm_bank_number'  : tcdm_bank_number,
            'elem_size'         : elem_size,
            # 'ce_height'         : ce_height,
            # 'ce_width'          : ce_width,
            # WARNING!!! Perform a height width inversion as per Chi's suggestion. Orignal Light_RedMulE model was based on a temp RedMulE architecture. 
            'ce_height'         : ce_width,
            'ce_width'          : ce_height,
            'ce_pipe'           : ce_pipe,
            'queue_depth'       : queue_depth,
            'fold_tiles_mapping': fold_tiles_mapping,
            'loc_base'          : loc_base,
        })

    def i_INPUT(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'input', signature='io')

    def i_OFFLOAD(self) -> gvsoc.systree.SlaveItf:
        """Returns the offload port.

        This is used by the core to offload instructions.\n

        Returns
        ----------
        gvsoc.systree.SlaveItf
            The slave interface
        """
        return gvsoc.systree.SlaveItf(self, 'offload', signature='wire<IssOffloadInsn<uint32_t>*>')

    def o_OFFLOAD_GRANT(self, itf: gvsoc.systree.SlaveItf):
        """Binds the offload grant port.

        This port is used for granting instruction which was previously blocked because
        the queue was full.\n

        Parameters
        ----------
        slave: gvsoc.systree.SlaveItf
            Slave interface
        """
        self.itf_bind('offload_grant', itf, signature='wire<IssOffloadInsnGrant<uint32_t>*>')

    def o_TCDM(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('tcdm', itf, signature='io')

    def o_IRQ(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('done_irq', itf, signature='wire<bool>')