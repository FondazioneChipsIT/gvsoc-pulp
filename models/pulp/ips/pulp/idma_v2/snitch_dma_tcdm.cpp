/*
 * Copyright (C) 2024 ETH Zurich and University of Bologna
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Authors: Lorenzo Zuolo, Chips-IT (lorenzo.zuolo@chips.it)
 *          Germain Haugou, ETH Zurich (germain.haugou@iis.ee.ethz.ch)
 *          - derived from snitch_dma.cpp
 */

#include <vp/vp.hpp>
#include "fe/idma_fe_xdma.hpp"
#include "me/idma_me_2d.hpp"
#include "be/idma_be.hpp"
#include "be/idma_be_axi.hpp"
#include "be/idma_be_tcdm.hpp"



/**
 * @brief Snitch DMA with a private TCDM port (IO v2)
 *
 * Same component as snitch_dma.cpp plus a direct port to the local memory, i.e.
 * the shape the magia tile iDMAs have in RTL: one AXI master pair towards the
 * NoC and one TCDM master pair towards the tile's L1 interconnect. This is the
 * io_v2 counterpart of pulp/idma/snitch_dma.cpp.
 *
 * This puts together:
 *   - Xdma front-end to handle xdma custom instructions (or a memory-mapped
 *     controller driving the same offload interface)
 *   - 2D middle end to add support for 2D transfers
 *   - AXI and TCDM back-end protocols; the top back-end picks between them per
 *     burst, by comparing the address against [loc_base, loc_base + loc_size)
 */
class SnitchDmaTcdm : public vp::Component
{
public:
    SnitchDmaTcdm(vp::ComponentConf &config);

private:
    IDmaFeXdma fe;
    IDmaMe2D me;
    IDmaBeAxi be_axi_read;
    IDmaBeAxi be_axi_write;
    IDmaBeTcdm be_tcdm_read;
    IDmaBeTcdm be_tcdm_write;
    IDmaBe be;
};



SnitchDmaTcdm::SnitchDmaTcdm(vp::ComponentConf &config)
    : vp::Component(config),
    fe(this, &this->me),
    me(this, &this->fe, &this->be),
    be_axi_read(this, "axi_read", &this->be), be_axi_write(this, "axi_write", &this->be),
    be_tcdm_read(this, "tcdm_read", &this->be), be_tcdm_write(this, "tcdm_write", &this->be),
    // External (AXI) pair first, local (TCDM) pair second
    be(this, &this->me, &this->be_axi_read, &this->be_axi_write,
        &this->be_tcdm_read, &this->be_tcdm_write)
{
}


extern "C" vp::Component *gv_new(vp::ComponentConf &config)
{
    return new SnitchDmaTcdm(config);
}
