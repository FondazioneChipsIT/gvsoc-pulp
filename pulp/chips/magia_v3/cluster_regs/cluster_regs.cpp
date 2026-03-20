/*
 * Copyright (C) 2025 Fondazione Chips-IT
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
 */

#include <vp/vp.hpp>
#include <vp/itf/io.hpp>
#include <vp/itf/wire.hpp>
#include <stdio.h>
#include <iostream>
#include <sstream>
#include <string>
#include <cstring>
#include <stdint.h>

/*****************************************************
*                   Class Definition                 *
*****************************************************/


class ClusterRegs : public vp::Component
{

public:
    ClusterRegs(vp::ComponentConf &config);
    static void spatz_fsm_handler(vp::Block *__this, vp::ClockEvent *event);
    static void pulp_fsm_handler(vp::Block *__this, vp::ClockEvent *event);

protected:
    static vp::IoReqStatus req(vp::Block *__this, vp::IoReq *req);
    vp::IoSlave         input_itf;

    vp::reg_32 spatz_clock_en_reg;
    vp::reg_32 spatz_ready_reg;
    vp::reg_32 spatz_start_irq_reg;
    vp::reg_32 spatz_taskbin_reg;
    vp::reg_32 spatz_data_reg;
    vp::reg_32 spatz_return_reg;
    vp::reg_32 spatz_done_reg;

    vp::WireMaster<bool> spatz_clock_en;
    vp::WireMaster<bool> spatz_start_irq;
    vp::WireMaster<bool> spatz_done_irq;

    vp::reg_32 pulp_clock_en_reg;
    vp::reg_32 pulp_done_reg;
    vp::WireMaster<bool> pulp_clock_en;
    vp::WireMaster<bool> pulp_done_irq;

    vp::ClockEvent *spatz_fsm_eu_event;
    vp::ClockEvent *pulp_fsm_eu_event;

    int nb_pulp_cores_to_wait;
    int nb_recv_end_reqs;

    vp::Trace trace;
};

extern "C" vp::Component *gv_new(vp::ComponentConf &config)
{
    return new ClusterRegs(config);
}

ClusterRegs::ClusterRegs(vp::ComponentConf &config)
    : vp::Component(config)
{
    //Initialize interface
    this->traces.new_trace("trace", &this->trace, vp::DEBUG);

    this->input_itf.set_req_meth(&ClusterRegs::req);
    this->new_slave_port("input", &this->input_itf);

    this->spatz_clock_en_reg.set(0x00000000);
    this->spatz_ready_reg.set(0x00000000);
    this->spatz_start_irq_reg.set(0x00000000);
    this->spatz_taskbin_reg.set(0x00000000);
    this->spatz_data_reg.set(0x00000000);
    this->spatz_return_reg.set(0x00000000);
    this->spatz_done_reg.set(0x00000000);

    this->pulp_clock_en_reg.set(0x00000000);
    this->pulp_done_reg.set(0x00000000);

    this->nb_pulp_cores_to_wait = get_js_config()->get("nb_pulp_cores_to_wait")->get_int();
    this->nb_recv_end_reqs=0;
    
    this->new_master_port("spatz_clock_en", &this->spatz_clock_en, this);
    this->new_master_port("spatz_start_irq", &this->spatz_start_irq, this);
    this->new_master_port("spatz_done_irq", &this->spatz_done_irq, this);

    this->new_master_port("pulp_clock_en", &this->pulp_clock_en, this);
    this->new_master_port("pulp_done_irq", &this->pulp_done_irq, this);

    this->spatz_fsm_eu_event = this->event_new(&ClusterRegs::spatz_fsm_handler);
    this->pulp_fsm_eu_event = this->event_new(&ClusterRegs::pulp_fsm_handler);

    this->trace.msg(vp::Trace::LEVEL_TRACE,"[Magia Cluster regs] Instantiated\n");

}

void ClusterRegs::spatz_fsm_handler(vp::Block *__this, vp::ClockEvent *event) {
    ClusterRegs *_this = (ClusterRegs *)__this;

    _this->spatz_done_reg.set(0x00);
    _this->spatz_done_irq.sync(false);
    _this->trace.msg("[Magia Snitch Spatz Registers] Snitch Spatz done reg reset\n");
          
}

void ClusterRegs::pulp_fsm_handler(vp::Block *__this, vp::ClockEvent *event) {
    ClusterRegs *_this = (ClusterRegs *)__this;

    _this->pulp_done_reg.set(0x00);
    _this->pulp_done_irq.sync(false);
    _this->trace.msg("[Magia pulp Registers] PULP done reg reset\n");
          
}

vp::IoReqStatus ClusterRegs::req(vp::Block *__this, vp::IoReq *req)
{
    ClusterRegs *_this = (ClusterRegs *)__this;

    uint64_t offset = req->get_addr();
    uint8_t *data = req->get_data();
    uint64_t size = req->get_size();
    bool is_write = req->get_is_write();

    if (size!=4) {
         _this->trace.fatal("[Magia Cluster regs] Memory mapped interface supports only 32 bits (4 bytes) buses. (Addr is 0x%08x, size is %lu)\n",offset,size);
    }

    if (offset == 0x00) { //SPATZ_CLK_EN
        if (is_write == 1) {
            uint32_t cnf_w;
            memcpy((uint8_t*)&cnf_w,data,size);
            _this->spatz_clock_en_reg.set(cnf_w);
            if (cnf_w==0x01) {
                _this->spatz_clock_en.sync(true);
                _this->trace.msg("[Magia Snitch Spatz Registers][0x00] Snitch Spatz enable clock\n");
            }
            else if (cnf_w==0x00) {
                _this->spatz_clock_en.sync(false);
                _this->trace.msg("[Magia Snitch Spatz Registers][0x00] Snitch Spatz disable clock\n");
            }
            else {
                _this->trace.fatal("[Magia Snitch Spatz Registers][0x00] Snitch Spatz enable clock unsupported value\n");
            }
        }
        else {
            uint32_t cnf_r =  _this->spatz_clock_en_reg.get();
            memcpy((void *)data, (void *)&cnf_r, size);
            _this->trace.msg("[Magia Snitch Spatz Registers][0x00] Snitch Spatz read clock enable register (0x%08x)\n",cnf_r);
        }
    }
    else if (offset == 0x04) { //SPATZ_READY_REG
        if (is_write == 1) {
            uint32_t cnf_w;
            memcpy((uint8_t*)&cnf_w,data,size);
            _this->spatz_ready_reg.set(cnf_w);
            _this->trace.msg("[Magia Snitch Spatz Registers][0x04] Snitch Spatz written ready register (0x%08x)\n",cnf_w);
        }
        else {
            uint32_t cnf_r =  _this->spatz_ready_reg.get();
            memcpy((void *)data, (void *)&cnf_r, size);
            _this->trace.msg("[Magia Snitch Spatz Registers][0x04] Snitch Spatz read ready register (0x%08x)\n",cnf_r);
        }
    }
    else if (offset == 0x08) { //SPATZ_START
        if (is_write == 1) {
            uint32_t cnf_w;
            memcpy((uint8_t*)&cnf_w,data,size);
            _this->spatz_start_irq_reg.set(cnf_w);
            if (cnf_w==0x01) {
                _this->spatz_start_irq.sync(true);
                _this->trace.msg("[Magia Snitch Spatz Registers][0x08] Snitch Spatz start irq True\n");
            }
            else if (cnf_w==0x00) {
                _this->spatz_start_irq.sync(false);
                _this->trace.msg("[Magia Snitch Spatz Registers][0x08] Snitch Spatz start irq False\n");
            }
            else {
                _this->trace.fatal("[Magia Snitch Spatz Registers][0x08] Snitch Spatz start unsupported value\n");
            }
        }
        else {
            uint32_t cnf_r =  _this->spatz_start_irq_reg.get();
            memcpy((void *)data, (void *)&cnf_r, size);
            _this->trace.msg("[Magia Snitch Spatz Registers][0x08] Snitch Spatz read start register (0x%08x)\n",cnf_r);
        }
    }
    else if (offset == 0x0C) { //SPATZ_TASKBIN_REG
        if (is_write == 1) {
            uint32_t cnf_w;
            memcpy((uint8_t*)&cnf_w,data,size);
            _this->spatz_taskbin_reg.set(cnf_w);
            _this->trace.msg("[Magia Snitch Spatz Registers][0x0C] Snitch Spatz written taskbin register (0x%08x)\n",cnf_w);
        }
        else {
            uint32_t cnf_r =  _this->spatz_taskbin_reg.get();
            memcpy((void *)data, (void *)&cnf_r, size);
            _this->trace.msg("[Magia Snitch Spatz Registers][0x0C] Snitch Spatz read task register (0x%08x)\n",cnf_r);
        }
    }
    else if (offset == 0x10) { //SPATZ_DATA_REG
        if (is_write == 1) {
            uint32_t cnf_w;
            memcpy((uint8_t*)&cnf_w,data,size);
            _this->spatz_data_reg.set(cnf_w);
            _this->trace.msg("[Magia Snitch Spatz Registers][0x10] Snitch Spatz written data register (0x%08x)\n",cnf_w);
        }
        else {
            uint32_t cnf_r =  _this->spatz_data_reg.get();
            memcpy((void *)data, (void *)&cnf_r, size);
            _this->trace.msg("[Magia Snitch Spatz Registers][0x10] Snitch Spatz read data register (0x%08x)\n",cnf_r);
        }
    }
    else if (offset == 0x14) { //SPATZ_RETURN_REG
        if (is_write == 1) {
            uint32_t cnf_w;
            memcpy((uint8_t*)&cnf_w,data,size);
            _this->spatz_return_reg.set(cnf_w);
            _this->trace.msg("[Magia Snitch Spatz Registers][0x14] Snitch Spatz written return register (0x%08x)\n",cnf_w);
        }
        else {
            uint32_t cnf_r =  _this->spatz_return_reg.get();
            memcpy((void *)data, (void *)&cnf_r, size);
            _this->trace.msg("[Magia Snitch Spatz Registers][0x14] Snitch Spatz read return register (0x%08x)\n",cnf_r);
        }
    }
    else if (offset == 0x18) { //SPATZ_DONE
        if (is_write == 1) {
            uint32_t cnf_w;
            memcpy((uint8_t*)&cnf_w,data,size);
            _this->spatz_done_reg.set(cnf_w);
            _this->trace.msg("[Magia Snitch Spatz Registers][0x18] Snitch Spatz done reg set\n");
            _this->spatz_done_irq.sync(true);
            //trigger fsm
            _this->event_enqueue(_this->spatz_fsm_eu_event, 1);
        }
        else {
            _this->trace.fatal("[Magia Snitch Spatz Registers][0x18] Snitch Spatz done unsupported read to register\n");
        }
    }
    else if (offset == 0x40) { //PULP_CLK_EN
        if (is_write == 1) {
            uint32_t cnf_w;
            memcpy((uint8_t*)&cnf_w,data,size);
            _this->pulp_clock_en_reg.set(cnf_w);
            if (cnf_w==0x01) {
                _this->pulp_clock_en.sync(true);
                _this->trace.msg("[Magia pulp Registers][0x40] PULP enable clock\n");
            }
            else if (cnf_w==0x00) {
                _this->pulp_clock_en.sync(false);
                _this->trace.msg("[Magia pulp Registers][0x40] PULP disable clock\n");
            }
            else {
                _this->trace.fatal("[Magia pulp Registers][0x40] PULP enable clock unsupported value\n");
            }
        }
        else {
            uint32_t cnf_r =  _this->pulp_clock_en_reg.get();
            memcpy((void *)data, (void *)&cnf_r, size);
            _this->trace.msg("[Magia pulp Registers][0x40] PULP read clock enable register (0x%08x)\n",cnf_r);
        }
    }
    else if (offset == 0x44) { //PULP_DONE
        if (is_write == 1) {
            uint32_t cnf_w;
            memcpy((uint8_t*)&cnf_w,data,size);
            _this->pulp_done_reg.set(cnf_w);
            _this->trace.msg("[Magia pulp Registers][0x44] PULP done reg set\n");
            _this->nb_recv_end_reqs++;
            if (_this->nb_recv_end_reqs==_this->nb_pulp_cores_to_wait) {
                _this->nb_recv_end_reqs=0;
                _this->pulp_done_irq.sync(true);
                //trigger fsm
                _this->event_enqueue(_this->pulp_fsm_eu_event, 1);
            }
        }
        else {
            _this->trace.fatal("[Magia pulp Registers][0x44] PULP done unsupported read to register\n");
        }
    }

    return vp::IO_REQ_OK;
}
