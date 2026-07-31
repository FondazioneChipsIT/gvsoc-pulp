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

# Authors: Lorenzo Zuolo, Chips-IT (lorenzo.zuolo@chips.it)
#          Germain Haugou, ETH Zurich (germain.haugou@iis.ee.ethz.ch)
#          - derived from snitch_dma.py

"""io_v2 Snitch-offload iDMA with a private TCDM port.

Generator for ``ips/pulp/idma_v2/snitch_dma_tcdm.cpp``: the io_v2 counterpart of
:class:`pulp.idma.snitch_dma.SnitchDma` (the v1 iDMA that owns both an AXI
master pair and a TCDM master pair), as used by the magia tiles.

Difference with :class:`ips.pulp.idma_v2.snitch_dma.SnitchDmaV2`: that one has a
single egress (the AXI pair) and expects local accesses to loop back through the
interconnect; this one keeps the v1 behaviour where a burst whose address falls
in ``[loc_base, loc_base + loc_size)`` is served directly on the TCDM port.
"""

import gvsoc.systree
from gvsoc.signature import IoV2Beat, IoV2SingleReq


class SnitchDmaTcdmV2(gvsoc.systree.Component):
    """Snitch-offload iDMA with AXI and TCDM back-ends, on the io_v2 protocol.

    Ports
    ~~~~~

    - **offload** / **offload_grant** — the Snitch accelerator-offload channel
      (also used by magia's memory-mapped iDMA controller, which drives the same
      wires).
    - **axi_read** / **axi_write** (master, ``IoV2Beat(axi_width)``) — system
      side. Beat-streaming, one beat per cycle at ``axi_width``.
    - **tcdm_read** / **tcdm_write** (master, ``IoV2SingleReq``) — local memory
      side. Line-sized inline accesses (``tcdm_width`` bytes per line), the
      timing model reading the cost annotated by the local interconnect.

    Each of the four is its own master because an io_v2 slave port binds exactly
    one master: bind them to four different interconnect input ports (this is
    the one structural difference from the v1 model, where ``o_AXI`` /
    ``o_TCDM`` could tie a read/write pair to a single slave port).

    Parameters
    ~~~~~~~~~~

    ``transfer_queue_size``
        Number of transfer descriptors queued in the front-end.
    ``burst_queue_size``
        Maximum number of outstanding bursts per back-end.
    ``burst_size``
        Optional cap on a logical burst's size in bytes (0 = no cap).
    ``loc_base`` / ``loc_size``
        Local area served by the TCDM back-end. ``loc_size = 0`` disables the
        TCDM path (every burst then goes to AXI).
    ``tcdm_width``
        Width of the local interconnect in bytes; TCDM accesses are split into
        lines of this size.
    ``axi_width``
        Width of the AXI bus in bytes, used as the beat size on the AXI side.
    """

    def __init__(self, parent: gvsoc.systree.Component, name: str,
            transfer_queue_size: int=8,
            burst_queue_size: int=8,
            burst_size: int=0,
            loc_base: int=0,
            loc_size: int=0,
            tcdm_width: int=0,
            axi_width: int=8):

        super().__init__(parent, name)

        self._axi_width = axi_width

        self.add_sources([
            'ips/pulp/idma_v2/snitch_dma_tcdm.cpp',
            'ips/pulp/idma_v2/fe/idma_fe_xdma.cpp',
            'ips/pulp/idma_v2/me/idma_me_2d.cpp',
            'ips/pulp/idma_v2/be/idma_be.cpp',
            'ips/pulp/idma_v2/be/idma_be_axi.cpp',
            'ips/pulp/idma_v2/be/idma_be_tcdm.cpp',
        ])

        self.add_properties({
            "transfer_queue_size": transfer_queue_size,
            "burst_queue_size": burst_queue_size,
            "burst_size" : burst_size,
            "loc_base": loc_base,
            "loc_size": loc_size,
            "tcdm_width": tcdm_width,
            "axi_width": axi_width,
        })

    def i_OFFLOAD(self) -> gvsoc.systree.SlaveItf:
        """Offload port: xdma instructions (or an mm controller driving them)."""
        return gvsoc.systree.SlaveItf(self, 'offload',
            signature='wire<IssOffloadInsn<uint32_t>*>')

    def o_OFFLOAD_GRANT(self, itf: gvsoc.systree.SlaveItf):
        """Grant of a previously blocked offload instruction."""
        self.itf_bind('offload_grant', itf,
            signature='wire<IssOffloadInsnGrant<uint32_t>*>')

    def o_AXI_READ(self, itf: gvsoc.systree.SlaveItf):
        """Read channel towards the system interconnect."""
        self.itf_bind('axi_read', itf, signature=IoV2Beat(self._axi_width))

    def o_AXI_WRITE(self, itf: gvsoc.systree.SlaveItf):
        """Write channel towards the system interconnect."""
        self.itf_bind('axi_write', itf, signature=IoV2Beat(self._axi_width))

    def o_TCDM_READ(self, itf: gvsoc.systree.SlaveItf):
        """Read channel towards the local memory interconnect."""
        self.itf_bind('tcdm_read', itf, signature=IoV2SingleReq())

    def o_TCDM_WRITE(self, itf: gvsoc.systree.SlaveItf):
        """Write channel towards the local memory interconnect."""
        self.itf_bind('tcdm_write', itf, signature=IoV2SingleReq())
