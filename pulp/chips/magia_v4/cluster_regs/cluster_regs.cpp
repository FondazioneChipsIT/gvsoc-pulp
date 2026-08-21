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
#include <cstring>
#include <stdint.h>
#include <string>
#include <vector>

/*****************************************************
*                   Class Definition                 *
*****************************************************/

class ClusterRegs : public vp::Component
{

public:
    ClusterRegs(vp::ComponentConf &config);
    static void spatz_fsm_handler(vp::Block *__this, vp::ClockEvent *event);
    static void pulp_fsm_handler(vp::Block *__this, vp::ClockEvent *event);
    static void pulp_start_deassert_handler(vp::Block *__this, vp::ClockEvent *event);

protected:
    static vp::IoReqStatus req(vp::Block *__this, vp::IoReq *req);
    vp::IoSlave         input_itf;

    /* Spatz registers — offsets [0x00, 0x18] */
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

    /* PULP cluster registers — offsets [0x40, 0x5C], mirroring
     * obi_slave_ctrl_cluster.sv (instantiated at TILE_CSR_START + 0x40 = 0x1740).
     *
     * Single-dispatcher programming model: the control core rings one doorbell,
     * cluster core 0 takes it from its own Event Unit and may fork the work onto
     * the other cores itself (pi_cl_team_fork). Hence no per-core start IRQ and
     * no ACK/DONE quorum: one write is one dispatch.
     *
     *   0x40: PULP_CLK_EN — broadcast enable: write !=0 starts ALL cores, 0 stops all;
     *                       a write also resets the READY counter
     *   0x44: PULP_BINARY — entry point (boot vector) of every cluster core
     *   0x48: PULP_DONE   — the dispatcher core writes 1 when the task returns → DONE event
     *   0x4C: PULP_TASKBIN— per-dispatch task function address, read by core 0
     *   0x50: PULP_DATA   — context pointer passed to the task as first argument
     *   0x54: PULP_START  — CV32 writes !=0 → 1-cycle doorbell into core 0's Event Unit
     *                       (EU_OTHER_CLUSTER_START, bit 13) and latches the request;
     *                       core 0 writes 0 to ACK, unblocking CV32's poll
     *   0x58: PULP_READY  — R: 1 once nb_pulp_cores cores have booted; W: per-core boot report
     *   0x5C: PULP_RETURN — task exit code; the dispatcher writes it right before DONE
     *                       (bit 31 = crashed, mcause in the low bits)
     */
    vp::reg_32 pulp_clock_en_reg;
    vp::reg_32 pulp_binary_reg;
    vp::reg_32 pulp_done_reg;
    vp::reg_32 pulp_taskbin_reg;
    vp::reg_32 pulp_data_reg;
    vp::reg_32 pulp_start_reg;
    vp::reg_32 pulp_ready_reg;
    vp::reg_32 pulp_return_reg;

    vp::WireMaster<bool>              pulp_clock_en;  /* broadcast: single port, all cores via GVSoC fan-out */
    vp::WireMaster<bool>              pulp_start_irq; /* doorbell towards cluster core 0's Event Unit */
    vp::WireMaster<bool>              pulp_done_irq;
    vp::WireMaster<uint64_t>          pulp_entry;

    vp::ClockEvent *spatz_fsm_eu_event;
    vp::ClockEvent *pulp_fsm_eu_event;
    vp::ClockEvent *pulp_start_deassert_event;

    int nb_pulp_cores;
    int nb_recv_ready_reqs; /* counts PULP_READY=1 writes (one per booted core) */

    vp::Trace trace;
};

extern "C" vp::Component *gv_new(vp::ComponentConf &config)
{
    return new ClusterRegs(config);
}

ClusterRegs::ClusterRegs(vp::ComponentConf &config)
    : vp::Component(config)
{
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
    /* Same reset value as obi_slave_ctrl_cluster.sv: the entry point reads back
     * meaningfully even before the control core programs it. Not synced out, the
     * cores take their boot address from the first actual write. */
    this->pulp_binary_reg.set(0xCC000080);
    this->pulp_done_reg.set(0x00000000);
    this->pulp_taskbin_reg.set(0x00000000);
    this->pulp_data_reg.set(0x00000000);
    this->pulp_start_reg.set(0x00000000);
    this->pulp_ready_reg.set(0x00000000);
    this->pulp_return_reg.set(0x00000000);

    this->nb_pulp_cores      = get_js_config()->get("nb_pulp_cores")->get_int();
    this->nb_recv_ready_reqs = 0;

    this->new_master_port("spatz_clock_en",  &this->spatz_clock_en,  this);
    this->new_master_port("spatz_start_irq", &this->spatz_start_irq, this);
    this->new_master_port("spatz_done_irq",  &this->spatz_done_irq,  this);

    /* Single broadcast clock-enable port — GVSoC fan-out via linked list */
    this->new_master_port("pulp_clock_en", &this->pulp_clock_en, this);

    /* Single doorbell port: it feeds the cluster Event Unit slice of core 0 */
    this->new_master_port("pulp_start_irq", &this->pulp_start_irq, this);

    this->new_master_port("pulp_done_irq", &this->pulp_done_irq, this);
    this->new_master_port("pulp_entry",    &this->pulp_entry,    this);

    this->spatz_fsm_eu_event       = this->event_new(&ClusterRegs::spatz_fsm_handler);
    this->pulp_fsm_eu_event        = this->event_new(&ClusterRegs::pulp_fsm_handler);
    this->pulp_start_deassert_event = this->event_new(&ClusterRegs::pulp_start_deassert_handler);

    this->trace.msg(vp::Trace::LEVEL_TRACE, "[Magia Cluster regs] Instantiated\n");
}

void ClusterRegs::spatz_fsm_handler(vp::Block *__this, vp::ClockEvent *event)
{
    ClusterRegs *_this = (ClusterRegs *)__this;
    _this->spatz_done_reg.set(0x00);
    _this->spatz_done_irq.sync(false);
    _this->trace.msg("[Spatz Regs] Done reg reset\n");
}

void ClusterRegs::pulp_fsm_handler(vp::Block *__this, vp::ClockEvent *event)
{
    ClusterRegs *_this = (ClusterRegs *)__this;
    _this->pulp_done_reg.set(0x00);
    _this->pulp_done_irq.sync(false);
    _this->trace.msg("[PULP Regs] Done IRQ deasserted\n");
}

void ClusterRegs::pulp_start_deassert_handler(vp::Block *__this, vp::ClockEvent *event)
{
    ClusterRegs *_this = (ClusterRegs *)__this;
    _this->pulp_start_irq.sync(false);
    _this->trace.msg("[PULP Regs] Start doorbell auto-deasserted (edge pulse done)\n");
}

vp::IoReqStatus ClusterRegs::req(vp::Block *__this, vp::IoReq *req)
{
    ClusterRegs *_this = (ClusterRegs *)__this;

    uint64_t offset   = req->get_addr();
    uint8_t *data     = req->get_data();
    uint64_t size     = req->get_size();
    bool     is_write = req->get_is_write();

    if (size != 4) {
        _this->trace.fatal("[Cluster Regs] Only 32-bit accesses supported (addr=0x%08lx size=%lu)\n", offset, size);
    }

    /* ------------------------------------------------------------------ */
    /* Spatz registers                                                      */
    /* ------------------------------------------------------------------ */

    if (offset == 0x00) { /* SPATZ_CLK_EN */
        if (is_write) {
            uint32_t val; memcpy(&val, data, 4);
            _this->spatz_clock_en_reg.set(val);
            if (val == 0x01) {
                _this->spatz_clock_en.sync(true);
                _this->trace.msg("[Spatz Regs][0x00] Clock enabled\n");
            } else if (val == 0x00) {
                _this->spatz_clock_en.sync(false);
                _this->trace.msg("[Spatz Regs][0x00] Clock disabled\n");
            } else {
                _this->trace.fatal("[Spatz Regs][0x00] Unsupported clock enable value\n");
            }
        } else {
            uint32_t val = _this->spatz_clock_en_reg.get();
            memcpy(data, &val, 4);
            _this->trace.msg("[Spatz Regs][0x00] Read clock enable (0x%08x)\n", val);
        }
    }
    else if (offset == 0x04) { /* SPATZ_READY */
        if (is_write) {
            uint32_t val; memcpy(&val, data, 4);
            _this->spatz_ready_reg.set(val);
            _this->trace.msg("[Spatz Regs][0x04] Write ready (0x%08x)\n", val);
        } else {
            uint32_t val = _this->spatz_ready_reg.get();
            memcpy(data, &val, 4);
            _this->trace.msg("[Spatz Regs][0x04] Read ready (0x%08x)\n", val);
        }
    }
    else if (offset == 0x08) { /* SPATZ_START */
        if (is_write) {
            uint32_t val; memcpy(&val, data, 4);
            _this->spatz_start_irq_reg.set(val);
            if (val == 0x01) {
                _this->spatz_start_irq.sync(true);
                _this->trace.msg("[Spatz Regs][0x08] Start IRQ asserted\n");
            } else if (val == 0x00) {
                _this->spatz_start_irq.sync(false);
                _this->trace.msg("[Spatz Regs][0x08] Start IRQ deasserted\n");
            } else {
                _this->trace.fatal("[Spatz Regs][0x08] Unsupported start value\n");
            }
        } else {
            uint32_t val = _this->spatz_start_irq_reg.get();
            memcpy(data, &val, 4);
            _this->trace.msg("[Spatz Regs][0x08] Read start (0x%08x)\n", val);
        }
    }
    else if (offset == 0x0C) { /* SPATZ_TASKBIN */
        if (is_write) {
            uint32_t val; memcpy(&val, data, 4);
            _this->spatz_taskbin_reg.set(val);
            _this->trace.msg("[Spatz Regs][0x0C] Write taskbin (0x%08x)\n", val);
        } else {
            uint32_t val = _this->spatz_taskbin_reg.get();
            memcpy(data, &val, 4);
            _this->trace.msg("[Spatz Regs][0x0C] Read taskbin (0x%08x)\n", val);
        }
    }
    else if (offset == 0x10) { /* SPATZ_DATA */
        if (is_write) {
            uint32_t val; memcpy(&val, data, 4);
            _this->spatz_data_reg.set(val);
            _this->trace.msg("[Spatz Regs][0x10] Write data (0x%08x)\n", val);
        } else {
            uint32_t val = _this->spatz_data_reg.get();
            memcpy(data, &val, 4);
            _this->trace.msg("[Spatz Regs][0x10] Read data (0x%08x)\n", val);
        }
    }
    else if (offset == 0x14) { /* SPATZ_RETURN */
        if (is_write) {
            uint32_t val; memcpy(&val, data, 4);
            _this->spatz_return_reg.set(val);
            _this->trace.msg("[Spatz Regs][0x14] Write return (0x%08x)\n", val);
        } else {
            uint32_t val = _this->spatz_return_reg.get();
            memcpy(data, &val, 4);
            _this->trace.msg("[Spatz Regs][0x14] Read return (0x%08x)\n", val);
        }
    }
    else if (offset == 0x18) { /* SPATZ_DONE */
        if (is_write) {
            uint32_t val; memcpy(&val, data, 4);
            _this->spatz_done_reg.set(val);
            _this->spatz_done_irq.sync(true);
            _this->event_enqueue(_this->spatz_fsm_eu_event, 1);
            _this->trace.msg("[Spatz Regs][0x18] Done signalled\n");
        } else {
            _this->trace.fatal("[Spatz Regs][0x18] Done register is write-only\n");
        }
    }

    /* ------------------------------------------------------------------ */
    /* PULP cluster registers                                               */
    /* ------------------------------------------------------------------ */

    else if (offset == 0x40) { /* PULP_CLK_EN — broadcast: !=0 enable all, 0 disable all */
        if (is_write) {
            uint32_t val; memcpy(&val, data, 4);
            _this->pulp_clock_en_reg.set(val);
            /* Reset READY counter so CV32 can re-poll after each init */
            _this->nb_recv_ready_reqs = 0;
            _this->pulp_ready_reg.set(0x00000000);
            bool en = (val != 0);
            _this->pulp_clock_en.sync(en);
            _this->trace.msg("[PULP Regs][0x40] Clock %s (broadcast)\n", en ? "enabled" : "disabled");
        } else {
            uint32_t val = _this->pulp_clock_en_reg.get();
            memcpy(data, &val, 4);
            _this->trace.msg("[PULP Regs][0x40] Read clock enable (0x%08x)\n", val);
        }
    }
    else if (offset == 0x44) { /* PULP_BINARY — entry point written by CV32 before enabling clock */
        if (is_write) {
            uint32_t val; memcpy(&val, data, 4);
            _this->pulp_binary_reg.set(val);
            _this->pulp_entry.sync((uint64_t)val);
            _this->trace.msg("[PULP Regs][0x44] Binary entry point set (0x%08x)\n", val);
        } else {
            uint32_t val = _this->pulp_binary_reg.get();
            memcpy(data, &val, 4);
            _this->trace.msg("[PULP Regs][0x44] Read binary entry point (0x%08x)\n", val);
        }
    }
    else if (offset == 0x48) { /* PULP_DONE — one write by the dispatcher core = one completed dispatch */
        if (is_write) {
            _this->pulp_done_reg.set(0x00000001);
            _this->pulp_done_irq.sync(true);
            _this->event_enqueue(_this->pulp_fsm_eu_event, 1);
            _this->trace.msg("[PULP Regs][0x48] Done signalled — DONE event fired\n");
        } else {
            uint32_t val = _this->pulp_done_reg.get();
            memcpy(data, &val, 4);
            _this->trace.msg("[PULP Regs][0x48] Read done (0x%08x)\n", val);
        }
    }
    else if (offset == 0x4C) { /* PULP_TASKBIN — task function address, read by core 0 */
        if (is_write) {
            uint32_t val; memcpy(&val, data, 4);
            _this->pulp_taskbin_reg.set(val);
            _this->trace.msg("[PULP Regs][0x4C] Taskbin set (0x%08x)\n", val);
        } else {
            uint32_t val = _this->pulp_taskbin_reg.get();
            memcpy(data, &val, 4);
            _this->trace.msg("[PULP Regs][0x4C] Read taskbin (0x%08x)\n", val);
        }
    }
    else if (offset == 0x50) { /* PULP_DATA — context pointer passed to the task */
        if (is_write) {
            uint32_t val; memcpy(&val, data, 4);
            _this->pulp_data_reg.set(val);
            _this->trace.msg("[PULP Regs][0x50] Data set (0x%08x)\n", val);
        } else {
            uint32_t val = _this->pulp_data_reg.get();
            memcpy(data, &val, 4);
            _this->trace.msg("[PULP Regs][0x50] Read data (0x%08x)\n", val);
        }
    }
    else if (offset == 0x54) { /* PULP_START — CV32: !=0 rings the doorbell and latches the request;
                                *               core 0: write 0 to ACK, which unblocks CV32's poll */
        if (is_write) {
            uint32_t val; memcpy(&val, data, 4);
            if (val != 0) {
                _this->pulp_start_reg.set(0x00000001);
                _this->pulp_start_irq.sync(true);
                _this->event_enqueue(_this->pulp_start_deassert_event, 1);
                _this->trace.msg("[PULP Regs][0x54] Dispatch doorbell → cluster core 0\n");
            } else {
                _this->pulp_start_reg.set(0x00000000);
                _this->trace.msg("[PULP Regs][0x54] ACK from cluster core 0 — PULP_START cleared\n");
            }
        } else {
            uint32_t val = _this->pulp_start_reg.get();
            memcpy(data, &val, 4);
            _this->trace.msg("[PULP Regs][0x54] Read start (0x%08x)\n", val);
        }
    }
    else if (offset == 0x58) { /* PULP_READY — each core writes 1 when booted; CV32 polls until all ready */
        if (is_write) {
            if (_this->nb_recv_ready_reqs < _this->nb_pulp_cores) {
                _this->nb_recv_ready_reqs++;
            }
            _this->trace.msg("[PULP Regs][0x58] Ready write %d/%d\n", _this->nb_recv_ready_reqs, _this->nb_pulp_cores);
            if (_this->nb_recv_ready_reqs == _this->nb_pulp_cores) {
                _this->pulp_ready_reg.set(0x00000001);
                _this->trace.msg("[PULP Regs][0x58] All cores ready\n");
            }
        } else {
            uint32_t val = _this->pulp_ready_reg.get();
            memcpy(data, &val, 4);
            _this->trace.msg("[PULP Regs][0x58] Read ready (0x%08x)\n", val);
        }
    }
    else if (offset == 0x5C) { /* PULP_RETURN — task exit code, bit 31 set by the trap handler */
        if (is_write) {
            uint32_t val; memcpy(&val, data, 4);
            _this->pulp_return_reg.set(val);
            _this->trace.msg("[PULP Regs][0x5C] Return value set (0x%08x)\n", val);
        } else {
            uint32_t val = _this->pulp_return_reg.get();
            memcpy(data, &val, 4);
            _this->trace.msg("[PULP Regs][0x5C] Read return value (0x%08x)\n", val);
        }
    }

    return vp::IO_REQ_OK;
}
