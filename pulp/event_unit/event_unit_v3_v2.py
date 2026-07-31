# SPDX-FileCopyrightText: 2026 ETH Zurich, University of Bologna and EssilorLuxottica SAS
#
# SPDX-License-Identifier: Apache-2.0
#
# Authors: Lorenzo Zuolo, Chips-IT (lorenzo.zuolo@chips.it)
#          - io_v2 port of event_unit_v3

import gvsoc.systree as st
from gvsoc.signature import IoV2SingleReq


class Event_unit(st.Component):
    """io_v2 sibling of :class:`pulp.event_unit.event_unit_v3.Event_unit`.

    Same event / mutex / dispatch / barrier / soc-event behaviour; only the
    memory-mapped ports move to the v2 IO protocol
    (``pulp.event_unit.eu_v3_impl_v2``).

    The ports are IoV2SingleReq, not IoV2Sync: an event wait (or a mutex /
    dispatch sleep) parks the request and replies later from the wake-up event,
    i.e. the unit answers IO_REQ_GRANTED + a deferred ``resp()``. Every access
    is a single 32-bit word, so a single-beat response always covers it.

    No ``width`` is declared on the ports: the model rejects anything but a
    32-bit access with a warning, exactly like the v1 model, instead of having
    the framework insert a width adapter that would silently split wider
    accesses.
    """

    def __init__(self, parent, name, config):

        super().__init__(parent, name)

        self.set_component('pulp.event_unit.eu_v3_impl_v2')

        self.add_properties(config)

    def i_INPUT(self) -> st.SlaveItf:
        """Shared memory-mapped input (the whole event-unit register space)."""
        return st.SlaveItf(self, 'input', signature=IoV2SingleReq())

    def i_DEMUX_INPUT(self, core: int) -> st.SlaveItf:
        """Per-core demultiplexed input (private core alias of the register space)."""
        return st.SlaveItf(self, f'demux_in_{core}', signature=IoV2SingleReq())
