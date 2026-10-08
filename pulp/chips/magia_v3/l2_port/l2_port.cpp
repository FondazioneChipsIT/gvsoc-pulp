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

#include <deque>
#include <vp/vp.hpp>
#include <vp/itf/io_v2.hpp>

/*
 * AXI port of the L2 of the testbench (axi_sim_mem), from the beat plane of
 * the NoC to the IoV2SingleReq memory behind it, which must answer inline.
 *
 * A read burst taken at cycle t returns its first beat at t + latency, the
 * latency being the one the memory annotates (at least one cycle), then one
 * beat per cycle; the bursts share the R channel and come back in order. A
 * write beat is written to the memory when it arrives, and the burst is
 * acknowledged latency cycles after its last beat.
 *
 * This replaces the framework beat-to-single-req adapter, which takes one
 * more cycle to send a read to the memory and one more to return its
 * response, i.e. two cycles more than the RTL on every L2 read
 * (core/models/utils/io_v2_beat_to_single_req_adapter.cpp).
 */

class L2Port : public vp::Component
{
public:
    L2Port(vp::ComponentConf &config);
    void reset(bool active) override;

private:
    struct Response {
        vp::IoReq *req;
        int64_t ready;
        bool is_write;
    };

    static vp::IoReqStatus in_req(vp::Block *__this, vp::IoReq *req);
    static void in_resp_retry(vp::Block *__this, vp::IoRetryChannel channel);
    static vp::IoRespAck out_resp(vp::Block *__this, vp::IoReq *req);
    static void out_retry(vp::Block *__this, vp::IoRetryChannel channel);
    static void fsm_handler(vp::Block *__this, vp::ClockEvent *event);

    // Access the memory, which must answer inline. Returns its latency.
    int64_t access(uint64_t addr, uint8_t *data, uint64_t size, bool is_write,
        uint8_t *strb, vp::IoRespStatus &status);
    void send_up(std::deque<Response> &queue, bool &denied);
    void schedule();

    vp::Trace trace;
    vp::IoSlave in{&L2Port::in_req, &L2Port::in_resp_retry};
    vp::IoMaster out{&L2Port::out_retry, &L2Port::out_resp};
    vp::ClockEvent *fsm_event;
    vp::IoReq mem_req;

    int width;
    vp::IoReqAllocator *beat_allocator;

    // Read beats and write acknowledgements to give back, in order
    std::deque<Response> read_queue;
    std::deque<Response> write_queue;
    // Cycle of the last read beat scheduled on the R channel
    int64_t last_read_ready;
    // Status of the write burst being received
    vp::IoRespStatus write_status;
    // Set when the master denied the head response
    bool read_denied;
    bool write_denied;
};

L2Port::L2Port(vp::ComponentConf &config)
    : vp::Component(config)
{
    this->traces.new_trace("trace", &this->trace, vp::DEBUG);

    this->new_slave_port("input", &this->in);
    this->new_master_port("output", &this->out);

    this->fsm_event = this->event_new(&L2Port::fsm_handler);

    this->width = this->get_js_config()->get_child_int("width");
    this->beat_allocator = vp::IoReqAllocator::get(this->width);
}

void L2Port::reset(bool active)
{
    if (!active)
    {
        return;
    }
    this->read_queue.clear();
    this->write_queue.clear();
    this->last_read_ready = -1;
    this->write_status = vp::IO_RESP_OK;
    this->read_denied = false;
    this->write_denied = false;
}

int64_t L2Port::access(uint64_t addr, uint8_t *data, uint64_t size, bool is_write,
    uint8_t *strb, vp::IoRespStatus &status)
{
    vp::IoReq *req = &this->mem_req;
    req->prepare();
    req->set_addr(addr);
    req->set_size(size);
    req->set_data(data);
    req->set_is_write(is_write);
    req->set_strb(strb);
    req->is_first = true;
    req->is_last = true;
    if (this->out.req(req) != vp::IO_REQ_DONE)
    {
        this->trace.fatal("The memory must answer inline (addr: 0x%lx)\n", addr);
    }
    status = req->get_resp_status();
    return req->get_full_latency();
}

vp::IoReqStatus L2Port::in_req(vp::Block *__this, vp::IoReq *req)
{
    L2Port *_this = (L2Port *)__this;
    int64_t now = _this->clock.get_cycles();
    vp::IoReqOpcode opcode = req->get_opcode();

    if (opcode == vp::READ)
    {
        // The whole burst is read now, its beats are given back one per cycle
        uint64_t size = req->get_size();
        uint64_t addr = req->get_addr();
        uint64_t offset = 0;
        _this->trace.msg(vp::Trace::LEVEL_TRACE, "Read burst (addr: 0x%lx, size: 0x%lx)\n",
            addr, size);
        while (offset < size || offset == 0)
        {
            uint64_t beat_size = std::min<uint64_t>(size - offset, _this->width);
            vp::IoReq *beat = _this->beat_allocator->alloc();
            beat->prepare();
            vp::IoRespStatus status;
            int64_t latency = _this->access(addr + offset, beat->get_data(), beat_size, false,
                nullptr, status);
            beat->set_addr(addr + offset);
            beat->set_size(beat_size);
            beat->set_opcode(vp::READ);
            beat->set_resp_status(status);
            beat->is_first = offset == 0;
            beat->is_last = offset + beat_size >= size;
            beat->burst_id = req->burst_id;
            beat->initiator = req->initiator;

            int64_t ready = now + std::max((int64_t)1, latency);
            if (ready <= _this->last_read_ready) ready = _this->last_read_ready + 1;
            _this->last_read_ready = ready;
            _this->read_queue.push_back(Response{beat, ready, false});

            offset += beat_size;
            if (beat_size == 0) break;
        }
        _this->schedule();
        // The request belongs to the master, which frees it
        return vp::IO_REQ_GRANTED;
    }

    if (opcode != vp::WRITE)
    {
        _this->trace.force_warning("Unsupported opcode %d (addr: 0x%lx)\n", opcode,
            req->get_addr());
        req->set_resp_status(vp::IO_RESP_INVALID);
        return vp::IO_REQ_DONE;
    }

    vp::IoRespStatus status;
    int64_t latency = _this->access(req->get_addr(), req->get_data(), req->get_size(), true,
        req->get_strb(), status);
    if (req->is_first)
    {
        _this->write_status = vp::IO_RESP_OK;
    }
    if (status == vp::IO_RESP_INVALID)
    {
        _this->write_status = vp::IO_RESP_INVALID;
    }

    if (!req->is_last)
    {
        // Granted beat: consumed, ours to free
        req->free();
        return vp::IO_REQ_GRANTED;
    }

    uint64_t addr = req->get_addr();
    uint64_t size = req->get_size();
    vp::IoReq *ack = vp::io_v2_write_ack(req);
    ack->set_addr(addr);
    ack->set_size(size);
    ack->set_resp_status(_this->write_status);
    _this->write_queue.push_back(Response{ack, now + std::max((int64_t)1, latency), true});
    _this->schedule();
    return vp::IO_REQ_GRANTED;
}

void L2Port::send_up(std::deque<Response> &queue, bool &denied)
{
    if (queue.empty() || denied || queue.front().ready > this->clock.get_cycles())
    {
        return;
    }
    vp::IoReq *resp = queue.front().req;
    if (this->in.resp(resp) == vp::IO_RESP_DENIED)
    {
        denied = true;
        return;
    }
    queue.pop_front();
}

vp::IoRespAck L2Port::out_resp(vp::Block *__this, vp::IoReq *req)
{
    L2Port *_this = (L2Port *)__this;
    _this->trace.fatal("The memory must answer inline\n");
    return vp::IO_RESP_ACCEPTED;
}

void L2Port::out_retry(vp::Block *__this, vp::IoRetryChannel channel)
{
}

void L2Port::in_resp_retry(vp::Block *__this, vp::IoRetryChannel channel)
{
    L2Port *_this = (L2Port *)__this;
    if (channel != vp::IO_RETRY_WRITE && _this->read_denied)
    {
        _this->read_denied = false;
        _this->send_up(_this->read_queue, _this->read_denied);
    }
    if (channel != vp::IO_RETRY_READ && _this->write_denied)
    {
        _this->write_denied = false;
        _this->send_up(_this->write_queue, _this->write_denied);
    }
    _this->schedule();
}

void L2Port::fsm_handler(vp::Block *__this, vp::ClockEvent *event)
{
    L2Port *_this = (L2Port *)__this;
    _this->send_up(_this->read_queue, _this->read_denied);
    _this->send_up(_this->write_queue, _this->write_denied);
    _this->schedule();
}

void L2Port::schedule()
{
    int64_t now = this->clock.get_cycles();
    int64_t next = -1;
    if (!this->read_queue.empty() && !this->read_denied)
    {
        next = this->read_queue.front().ready;
    }
    if (!this->write_queue.empty() && !this->write_denied)
    {
        int64_t ready = this->write_queue.front().ready;
        if (next == -1 || ready < next) next = ready;
    }
    if (next == -1)
    {
        return;
    }
    int64_t delay = next > now ? next - now : 1;
    if (this->fsm_event->is_enqueued())
    {
        if (this->fsm_event->get_cycle() <= now + delay)
        {
            return;
        }
        this->event_cancel(this->fsm_event);
    }
    this->event_enqueue(this->fsm_event, delay);
}

extern "C" vp::Component *gv_new(vp::ComponentConf &config)
{
    return new L2Port(config);
}
