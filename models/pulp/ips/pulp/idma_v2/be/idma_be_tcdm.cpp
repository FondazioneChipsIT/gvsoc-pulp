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
 * Authors: Germain Haugou, ETH Zurich (germain.haugou@iis.ee.ethz.ch)
 *          Lorenzo Zuolo, Chips-IT (lorenzo.zuolo@chips.it)
 *          - io_v2 port of the TCDM back-end (protocol, deny/retry handling)
 */

#include <algorithm>
#include <vp/vp.hpp>
#include "idma_be_tcdm.hpp"

// io_v2 port of pulp/idma/be/idma_be_tcdm.cpp. The FSM, the line splitting and
// the timing model are untouched; the differences are all in how a line request
// is issued and how its outcome is read:
//
//   - v1's IO_REQ_OK becomes IO_REQ_DONE, and v1's IO_REQ_INVALID becomes
//     IO_REQ_DONE plus IO_RESP_INVALID on the request's response-status
//     sideband. Both are handled in send_line() below.
//   - the cost of a line is read with get_full_latency() (head latency +
//     bandwidth occupancy) instead of get_latency(), so an interconnect that
//     models throughput is not silently dropped. On magia's TCDM path
//     (interleaver + memory_v3) duration is 0, so this is the v1 value.
//   - the master port takes its resp / retry callbacks at construction. Unlike
//     the v1 model, IO_REQ_DENIED is a normal outcome here: the TCDM
//     interconnect models bank contention (SpatzTcdmInterco) and refuses the
//     losers of an arbitration round, so a denied line is held and re-sent from
//     ico_retry() — inside the callback, same cycle, as the io_v2 retry
//     contract requires. Only an *asynchronous* answer (GRANTED) remains
//     unsupported.



IDmaBeTcdm::IDmaBeTcdm(vp::Component *idma, std::string itf_name, IdmaBeProducer *be)
:   Block(idma, itf_name),
    fsm_event(this, &IDmaBeTcdm::fsm_handler)
{
    // Backend will be used later for interaction
    this->be = be;

    // Declare master port to TCDM interface
    this->traces.new_trace("trace", &this->trace, vp::DEBUG);

    // Declare our own trace so that we can individually activate traces.
    //
    // The block is passed as the port context: the resp / retry callbacks are
    // static methods that cast it back to IDmaBeTcdm. Without it the context
    // would default to the enclosing component and the cast would produce
    // garbage — harmless in the v1 model, whose master port had no callbacks
    // (the transparent interleaver never denied nor answered asynchronously),
    // fatal here where the interconnect really does call retry().
    idma->new_master_port(itf_name, &this->ico_itf, this);

    // Get the width of the TCDM interconnect. This is used to constrain the size of the
    // requests which are sent to the TCDM
    this->width = idma->get_js_config()->get_int("tcdm_width");

    // Max number of pending bursts
    this->burst_queue_maxsize = idma->get_js_config()->get_int("burst_queue_size");

    // Local memory base
    this->loc_base = idma->get_js_config()->get_int("loc_base");
}



// Issue one line request and return the cost the interconnect reported. The
// TCDM side is required to answer inline (IoV2SingleReq + an inline slave); an
// asynchronous or back-pressured answer would need a response path this
// back-end deliberately does not have (the DMA always wins TCDM arbitration).
bool IDmaBeTcdm::send_line(vp::IoReq *req, uint64_t base, uint64_t size, int64_t *latency)
{
    vp::IoReqStatus status = this->ico_itf.req(req);

    if (status == vp::IO_REQ_DENIED)
    {
        // Bank conflict: a contention-modelling interconnect refuses the line
        // and elects a winner, then calls retry() on the losers. We keep the
        // line and re-send it from there.
        this->trace.msg(vp::Trace::LEVEL_TRACE,
            "TCDM line denied, holding it (base: 0x%lx, size: 0x%lx)\n", base, size);
        return false;
    }

    if (status != vp::IO_REQ_DONE)
    {
        // The TCDM side is required to answer inline: an asynchronous reply
        // would need a response path this back-end deliberately does not have.
        this->trace.fatal("Asynchronous response is not supported on TCDM backend\n");
        *latency = 0;
        return true;
    }

    if (req->get_resp_status() != vp::IO_RESP_OK)
    {
        this->trace.force_warning("Invalid access during TCDM line access "
            "(base: 0x%lx, size: 0x%lx)\n", base, size);
    }

    *latency = req->get_full_latency();
    return true;
}



vp::IoRespAck IDmaBeTcdm::ico_resp(vp::Block *__this, vp::IoReq *req)
{
    IDmaBeTcdm *_this = (IDmaBeTcdm *)__this;
    _this->trace.fatal("Asynchronous response is not supported on TCDM backend\n");
    return vp::IO_RESP_ACCEPTED;
}



// The interconnect signals it can accept again. The held line MUST be re-sent
// from inside this callback, same cycle: a bank-arbitrating interconnect
// (SpatzTcdmInterco, log_ico_v2) only keeps its election window open for the
// duration of this call, so deferring the re-send live-locks. See
// vp/itf/io_v2.hpp (IoSlave::retry).
void IDmaBeTcdm::ico_retry(vp::Block *__this, vp::IoRetryChannel channel)
{
    IDmaBeTcdm *_this = (IDmaBeTcdm *)__this;

    if (_this->denied_kind == DENIED_NONE)
    {
        // Nothing held — the interconnect does not track who it denied, so a
        // retry meant for another master reaches us too.
        return;
    }

    int64_t latency = 0;
    if (!_this->send_line(&_this->req, _this->denied_base, _this->denied_size, &latency))
    {
        // Refused again: keep holding and wait for the next retry.
        return;
    }

    DeniedKind kind = _this->denied_kind;
    uint64_t size = _this->denied_size;
    _this->denied_kind = DENIED_NONE;

    if (kind == DENIED_WRITE)
    {
        _this->write_line_completed(size, latency);
    }
    else
    {
        _this->read_line_completed(size, latency);
    }
}



void IDmaBeTcdm::activate_burst()
{
    // If queue is not empty and we don't have any active burst, activate it
    if (this->current_burst_size == 0 && this->burst_queue_size.size() > 0)
    {
        this->current_burst_base = this->burst_queue_base.front();
        this->current_burst_size = this->burst_queue_size.front();
    }
}



void IDmaBeTcdm::enqueue_burst(uint64_t base, uint64_t size, bool is_write, IdmaTransfer *transfer)
{
    // Just enqueue the burst and trigger the FSM, the FSM will take care of sending the requests
    this->burst_queue_base.push(base);
    this->burst_queue_size.push(size);
    this->burst_queue_is_write.push(is_write);
    this->burst_queue_transfer.push(transfer);

    // We may need to activate the first burst
    this->activate_burst();

    // Trigger the FSM since we may need to start processing a burst
    this->fsm_event.enqueue();
}



// Called by the backend to enqueue a read burst
void IDmaBeTcdm::read_burst(IdmaTransfer *transfer, uint64_t base, uint64_t size)
{
    this->enqueue_burst(base, size, false, transfer);
}



// Called by the backend to enqueue a write burst
void IDmaBeTcdm::write_burst(IdmaTransfer *transfer, uint64_t base, uint64_t size)
{
    this->enqueue_burst(base, size, true, transfer);
}



uint64_t IDmaBeTcdm::get_burst_size(uint64_t base, uint64_t size)
{
    // There is no constraint on burst size, this backend will anyway cut the burst into lines
    return size;
}


bool IDmaBeTcdm::can_accept_burst()
{
    // Accept a burst if we have room in the queue of pending bursts
    return this->burst_queue_base.size() < this->burst_queue_maxsize;
}

bool IDmaBeTcdm::can_accept_data()
{
    // Accept data if we don't have already a chunk of data being written
    return this->write_current_chunk_size == 0 && this->write_ack_timestamp == -1;
}



// Called by backend to push data for the current burst
void IDmaBeTcdm::write_data(IdmaTransfer *transfer, uint8_t *data, uint64_t size)
{
    this->trace.msg(vp::Trace::LEVEL_TRACE, "Writing data (size: 0x%lx)\n", size);

    // Since the data may be bigger than a line, first enqueue the whole data and process it
    // line by line
    this->write_current_chunk_ack_size = size;
    this->write_current_chunk_size = size;
    this->write_current_chunk_base = this->current_burst_base;
    this->write_current_chunk_data = data;
    this->write_current_chunk_data_start = data;
    this->write_current_transfer = transfer;

    // Send a line now, the rest will be handled by the FSM
    this->write_line();
}



void IDmaBeTcdm::reset(bool active)
{
    if (active)
    {
        this->current_burst_size = 0;
        this->read_pending_line_size = 0;

        this->write_current_chunk_size = 0;
        this->write_ack_timestamp = -1;

        this->last_line_timestamp = -1;

        this->denied_kind = DENIED_NONE;
    }
}



void IDmaBeTcdm::write_line()
{
    // Only send the line if no line was already sent in this cycle. Since we try to send a line as
    // soon as we receive a request, this may happen
    if (this->last_line_timestamp == -1 || this->last_line_timestamp < this->clock.get_cycles())
    {
        this->last_line_timestamp = this->clock.get_cycles();

        // Extract one line from current data chunk
        uint64_t base = this->write_current_chunk_base;
        uint64_t size = this->get_line_size(this->write_current_chunk_base, this->write_current_chunk_size);

        this->trace.msg(vp::Trace::LEVEL_TRACE, "Writing line to TCDM (base: 0x%lx, size: 0x%lx)\n",
            base, size);

        // Prepare the line request
        vp::IoReq *req = &this->req;

        req->prepare();
        req->set_is_write(true);
        req->set_addr(base - this->loc_base);
        req->set_size(size);
        req->set_data(this->write_current_chunk_data);

        // Send request to TCDM. The chunk cursor is advanced only once the line
        // is accepted: a denied line must be re-sent unchanged from ico_retry().
        int64_t latency = 0;
        if (!this->send_line(req, base, size, &latency))
        {
            this->denied_kind = DENIED_WRITE;
            this->denied_base = base;
            this->denied_size = size;
            return;
        }

        this->write_line_completed(size, latency);
    }
    else
    {
        this->update();
    }
}



void IDmaBeTcdm::write_line_completed(uint64_t size, int64_t latency)
{
    // The line landed: move the chunk cursor past it.
    this->write_current_chunk_base += size;
    this->write_current_chunk_size -= size;
    this->write_current_chunk_data += size;

    if (latency == 0)
    {
        // If the response has no latency, handle it now so that we can immediately continue
        // with the next line
        this->remove_chunk_from_current_burst(size);
        this->write_handle_req_ack();
    }
    else
    {
        // Otherwise enqueue it with timestamp so that we acknowledge it at correct time
        this->write_ack_timestamp = this->clock.get_cycles() + latency;
        this->write_ack_size = size;
        this->fsm_event.enqueue(latency);
    }
}



void IDmaBeTcdm::write_handle_req_ack()
{
    if (this->write_current_chunk_size == 0)
    {
        this->trace.msg(vp::Trace::LEVEL_TRACE, "Finished TCDM line, notifying middle-end\n");

        this->be->update();
        // If the chunk is done, acknowledge it
        this->be->ack_data(this->write_current_transfer, this->write_current_chunk_data_start, this->write_current_chunk_ack_size);
    }
    else
    {
        // Otherwise, the FSM will take care of the next line
        this->fsm_event.enqueue();
    }
}



uint64_t IDmaBeTcdm::get_line_size(uint64_t base, uint64_t size)
{
    // Make sure we don't go over interface width
    size = std::min(size, (uint64_t)this->width);

    // And that we don't cross the line
    uint64_t next_page = (base + this->width - 1) & ~(this->width - 1);
    if (next_page > base)
    {
        size = std::min(next_page - base, size);
    }

    return size;
}



void IDmaBeTcdm::remove_chunk_from_current_burst(uint64_t size)
{
    // Update current burst
    this->current_burst_base += size;
    this->current_burst_size -= size;

    if (this->current_burst_size == 0)
    {
        // In case it is the last chunk, remove the current burst
        this->burst_queue_base.pop();
        this->burst_queue_size.pop();
        this->burst_queue_is_write.pop();
        this->burst_queue_transfer.pop();

        // And take the next one
        this->activate_burst();

        this->be->update();
        this->update();
    }
}


void IDmaBeTcdm::read_line()
{
    vp::IoReq *req = &this->req;

    // Extract line from current read burst
    uint64_t base = this->current_burst_base;
    uint64_t size = this->get_line_size(base, this->current_burst_size);

    this->trace.msg(vp::Trace::LEVEL_TRACE, "Reading line from TCDM (base: 0x%lx, size: 0x%lx)\n",
            base, size);

    // Prepare the IO request to TCDM
    req->prepare();
    req->set_is_write(false);
    req->set_addr(base - this->loc_base);
    req->set_size(size);
    // Since the destination backend may keep the data until the write is done, we need
    // to dynamically allocate the data since we may read several times before data is acknowledged
    // We will free it when we receive the ack
    req->set_data(new uint8_t[size]);

    // Send to TCDM. A denied line is held and re-sent from ico_retry(); the
    // request keeps its freshly allocated buffer, so nothing is leaked or
    // allocated twice, and the burst cursor is left untouched.
    int64_t latency = 0;
    if (!this->send_line(req, base, size, &latency))
    {
        this->denied_kind = DENIED_READ;
        this->denied_base = base;
        this->denied_size = size;
        return;
    }

    this->read_line_completed(size, latency);
}



void IDmaBeTcdm::read_line_completed(uint64_t size, int64_t latency)
{
    vp::IoReq *req = &this->req;

    if (latency == 0 && this->be->is_ready_to_accept_data(this->burst_queue_transfer.front()))
    {
        // If there is no latency and backend is ready, we can immediately push the data
        IdmaTransfer *transfer = this->burst_queue_transfer.front();
        this->remove_chunk_from_current_burst(size);
        this->be->write_data(transfer, req->get_data(), size);
        // Keep pulling the next line ourselves instead of waiting to be woken
        // up by write_data_ack(). This is the one behavioural difference with
        // the v1 back-end, and it is required by the v2 destination: v1's AXI
        // back-end acknowledged each chunk as soon as its request was served
        // ("to let the other backend protocol send the rest of the burst
        // immediately"), which paced this read path for free. The v2 one only
        // acknowledges when the burst's B response arrives — the write is not
        // guaranteed before that — so a read path waiting for the ack would
        // never deliver the rest of the burst the ack depends on: deadlock
        // after the first lines. Re-arming here keeps the v1 rate of one line
        // per cycle, and over-running the destination is impossible because
        // every line is gated on is_ready_to_accept_data() above.
        this->update();
    }
    else
    {
        // Otherwise we have to put it on hold since we can only have one request pending
        this->read_pending_timestamp = this->clock.get_cycles() + latency;
        this->read_pending_line_data = req->get_data();
        this->read_pending_line_size = size;
        this->fsm_event.enqueue(latency);
    }
}



// Called by destination backend to ack the data we sent for writing
void IDmaBeTcdm::write_data_ack(uint8_t *data)
{
    // Release the data since we are now sure it won't be used anymore
    delete[] data;
    // And check if there is any action to take since backend may became ready
    this->update();
}



// This is called everytime we should check if any action should be taken
void IDmaBeTcdm::fsm_handler(vp::Block *__this, vp::ClockEvent *event)
{
    IDmaBeTcdm *_this = (IDmaBeTcdm *)__this;

    if (_this->denied_kind != DENIED_NONE)
    {
        // A line is held waiting for the interconnect's retry. It owns
        // this->req — the single request object used for every line — so no new
        // line may be prepared until it has been re-sent from ico_retry().
        return;
    }

    // Check if we should acknowledge the previous line, this can happen when the write request
    // got a latency
    if (_this->write_ack_timestamp != -1)
    {
        if (_this->write_ack_timestamp <= _this->clock.get_cycles())
        {
            _this->write_ack_timestamp = -1;
            _this->remove_chunk_from_current_burst(_this->write_ack_size);
            _this->write_handle_req_ack();
        }
        else
        {
            _this->fsm_event.enqueue(_this->write_ack_timestamp - _this->clock.get_cycles());
        }
    }

    // If a write chunk is pending, only send a line if we are not waiting for previous line
    // acknowledgement
    if (_this->write_current_chunk_size > 0 && _this->write_ack_timestamp == -1)
    {
        // Pending write chunk
        _this->write_line();
    }

    if (_this->burst_queue_is_write.size() > 0 && !_this->burst_queue_is_write.front())
    {
        // If a read burst is pending, only read new line fi previous one has been sent
        if (_this->current_burst_size > 0 && _this->read_pending_line_size == 0)
        {
            _this->read_line();
        }

        // If a read line is stuck because backend was not ready to receive it,
        // check if it is now ready
        if (_this->read_pending_line_size > 0 && _this->be->is_ready_to_accept_data(_this->burst_queue_transfer.front()))
        {
            // Maybe it was actually stuck due to latency
            if (_this->read_pending_timestamp <= _this->clock.get_cycles())
            {
                uint64_t size = _this->read_pending_line_size;
                _this->read_pending_line_size = 0;
                IdmaTransfer *transfer = _this->burst_queue_transfer.front();
                _this->remove_chunk_from_current_burst(size);
                _this->be->write_data(transfer, _this->read_pending_line_data, size);
                // Same self-pacing as in read_line(): the v2 destination only
                // acknowledges at the end of the burst, so nothing else would
                // wake this path up to read the remaining lines.
                _this->update();
            }
            else
            {
                _this->fsm_event.enqueue(_this->read_pending_timestamp - _this->clock.get_cycles());
            }
        }
    }
}



void IDmaBeTcdm::update()
{
    // All the checks are centralized in a new cycle in the FSM
    this->fsm_event.enqueue();
}



bool IDmaBeTcdm::is_empty()
{
    return this->burst_queue_base.empty();
}
