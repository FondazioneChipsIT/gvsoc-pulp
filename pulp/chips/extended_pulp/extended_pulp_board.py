#
# Copyright (C) 2020 ETH Zurich and University of Bologna
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

import gsystree as st
from pulp.chips.extended_pulp.extended_pulp import Extended_pulp
from devices.hyperbus.hyperflash import Hyperflash
from devices.spiflash.spiflash import Spiflash
from devices.spiflash.atxp032 import Atxp032
from devices.hyperbus.hyperram import Hyperram
from devices.testbench.testbench import Testbench
from devices.uart.uart_checker import Uart_checker
import gv.gvsoc_runner
from gapylib.chips.pulp.flash import *


class Extended_pulp_board(gv.gvsoc_runner.Runner):

    def __init__(self, parser, options, parent=None, name='top'):

        super(Extended_pulp_board, self).__init__(parent, name, parser=parser, options=options)

        # Pulp
        pulp = Extended_pulp(self, 'chip', parser)

        # Flash
        hyperflash = Hyperflash(self, 'hyperflash')
        hyperram = Hyperram(self, 'ram')

        self.register_flash(
            DefaultFlashRomV2(self, 'hyperflash', image_name=hyperflash.get_image_path(),
                flash_attributes={
                    "flash_type": 'hyper'
                },
                size=8*1024*1024
            ))

        self.bind(pulp, 'hyper0_cs1_data', hyperflash, 'input')

        self.bind(pulp, 'hyper0_cs0_data', hyperram, 'input')

        uart_checker = Uart_checker(self, 'uart_checker')
        self.bind(pulp, 'uart0', uart_checker, 'input')
