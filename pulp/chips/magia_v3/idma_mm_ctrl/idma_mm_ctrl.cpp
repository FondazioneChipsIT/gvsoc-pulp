/*
 * Copyright (C) 2026 Fondazione Chips-IT
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
 * Authors: Lorenzo Zuolo, Fondazione Chips-IT (lorenzo.zuolo@chips.it)
 */

#include <vp/vp.hpp>
#include <vp/itf/io.hpp>
#include <vp/itf/wire.hpp>
#include <vp/register.hpp>

/*
 * Offset of the second channel inside the window. Below it lies the register file of the AXI2OBI
 * channel, which moves data from L2 to L1, above it the one of the OBI2AXI channel, which moves it
 * the other way. This mirrors IDMA_BASE_AXI2OBI and IDMA_BASE_OBI2AXI in the SDK.
 */
#define IDMA_MM_DIRECTION_OFFSET 0x200

/*
 * Offset of the next_id register of stream 0 inside a channel. Reading it is what launches a
 * transfer, so it is the point where the transfer starts being timed. It has to match the register
 * map of the iDMA the tile instantiates, v0.6.4, which reserves sixteen entries per multireg.
 */
#define IDMA_MM_NEXT_ID_0_OFFSET 0x44


/**
 * @brief Memory-mapped window on the iDMA register files of a MAGIA tile
 *
 * The tile has two independent transfer channels, one per direction, each built around its own
 * reg32_3d register file. This is the equivalent of idma_obi_ctrl_decoder in the RTL: it forwards
 * a memory-mapped access to the register file of the channel the address selects, and adds nothing
 * of its own to the register map.
 *
 * On top of that it reports completion. A channel notifies its transfers are done through the
 * event port of its front-end; this block turns that into the interrupt line the tile expects and
 * logs how long the transfer took, which the previous implementation also did.
 */
class iDMA_mm_ctrl : public vp::Component
{
public:
    iDMA_mm_ctrl(vp::ComponentConf &config);

private:
    // Handle an access to the window and forward it to the right channel
    static vp::IoReqStatus req(vp::Block *__this, vp::IoReq *req);
    // Called when a channel completes a transfer
    static void done_sync(vp::Block *__this, bool value, int channel);

    // Trace for this block
    vp::Trace trace;
    // Window where the two register files are mapped
    vp::IoSlave input_itf;
    // One configuration port per channel, bound to the register file of that channel
    vp::IoMaster cfg_itf[2];
    // Completion coming from each channel
    vp::WireSlave<bool> done_itf[2];
    // Interrupt raised towards the tile for each channel
    vp::WireMaster<bool> irq_itf[2];
    // Cycle at which the last transfer of each channel was launched, only used for the trace
    int64_t transfer_start[2];
};



iDMA_mm_ctrl::iDMA_mm_ctrl(vp::ComponentConf &config)
    : vp::Component(config)
{
    this->traces.new_trace("trace", &this->trace, vp::DEBUG);

    this->input_itf.set_req_meth(&iDMA_mm_ctrl::req);
    this->new_slave_port("input", &this->input_itf, this);

    for (int i = 0; i < 2; i++)
    {
        this->new_master_port("cfg_" + std::to_string(i), &this->cfg_itf[i], this);

        this->done_itf[i].set_sync_meth_muxed(&iDMA_mm_ctrl::done_sync, i);
        this->new_slave_port("done_" + std::to_string(i), &this->done_itf[i], this);

        this->new_master_port("idma" + std::to_string(i) + "_done_irq", &this->irq_itf[i], this);

        this->transfer_start[i] = 0;
    }
}



vp::IoReqStatus iDMA_mm_ctrl::req(vp::Block *__this, vp::IoReq *req)
{
    iDMA_mm_ctrl *_this = (iDMA_mm_ctrl *)__this;

    uint64_t offset = req->get_addr();

    // The address picks the channel, the rest of it is the offset inside its register file
    int channel = offset >= IDMA_MM_DIRECTION_OFFSET;
    offset -= channel * IDMA_MM_DIRECTION_OFFSET;

    _this->trace.msg(vp::Trace::LEVEL_TRACE,
        "Forwarding access to channel (channel: %d, offset: 0x%lx, size: 0x%x, is_write: %d)\n",
        channel, offset, req->get_size(), req->get_is_write());

    // Reading next_id launches the transfer, so this is where it starts being timed
    if (!req->get_is_write() && offset == IDMA_MM_NEXT_ID_0_OFFSET)
    {
        _this->transfer_start[channel] = _this->clock.get_cycles();
    }

    req->set_addr(offset);

    return _this->cfg_itf[channel].req(req);
}



void iDMA_mm_ctrl::done_sync(vp::Block *__this, bool value, int channel)
{
    iDMA_mm_ctrl *_this = (iDMA_mm_ctrl *)__this;

    _this->trace.msg(vp::Trace::LEVEL_TRACE, "Transfer completed (channel: %d, cycles: %ld)\n",
        channel, _this->clock.get_cycles() - _this->transfer_start[channel]);

    if (_this->irq_itf[channel].is_bound())
    {
        _this->irq_itf[channel].sync(value);
    }
}



extern "C" vp::Component *gv_new(vp::ComponentConf &config)
{
    return new iDMA_mm_ctrl(config);
}
