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

/*
 * Fetch enable module, io_v2 version reading through the L2.
 *
 * The host (QEMU) cannot generate GVSoC requests: it only writes its RAM,
 * which is also the L2 (UMA memfd mapped by the L2 memory). So, once out of
 * reset, this module reads the fetch enable word (fetch_en_addr_base) through
 * its "mem" port, bound to the L2, every poll_period cycles. When the word
 * contains fetch_en_pattern, it raises fetch enable on all the bound cores and
 * stops polling. The module does not map the memfd itself.
 *
 * Entry point from the host: when entry_addr is not 0, once the pattern is
 * found the module also reads the 32-bit word at entry_addr (written by the
 * host before the pattern) and sends it on the "entry" port (core boot
 * address) before raising fetch enable, since the cores take the boot address
 * as PC on the fetch enable rising edge.
 *
 * The "mem" port is synchronous (IoV2Sync): the L2 answers inline. The
 * latency it annotates is not modelled, the poll period dominates.
 */

#include <vp/vp.hpp>
#include <vp/itf/io_v2.hpp>
#include <vp/itf/wire.hpp>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

class FetchEnableModuleV2 : public vp::Component
{

public:

  FetchEnableModuleV2(vp::ComponentConf &config);

  void reset(bool active) override;

private:

  static void poll_handler(vp::Block *__this, vp::ClockEvent *event);
  // Never called by a synchronous slave
  static vp::IoRespAck mem_resp(vp::Block *__this, vp::IoReq *req);
  static void mem_retry(vp::Block *__this, vp::IoRetryChannel channel);
  // Reads a 32-bit word through the mem port, false on error
  bool read_word(uint64_t addr, uint32_t *value);
  void check_value(uint32_t value);
  void set_fetch_enable(bool active);

  vp::Trace     trace;
  vp::IoMaster mem_itf{&FetchEnableModuleV2::mem_retry, &FetchEnableModuleV2::mem_resp};
  vp::WireMaster<bool> fetch_enable_itf;
  vp::WireMaster<uint64_t> entry_itf;

  vp::ClockEvent *poll_event;
  vp::IoReq mem_req;

  uint64_t fetch_en_base_address;
  uint64_t fetch_en_addr_size;
  uint32_t fetch_en_pattern;
  int64_t poll_period;
  // Address of the entry word, 0 if the entry does not come from the host
  uint64_t entry_addr;
  bool enabled;
  int64_t nb_polls;
};

FetchEnableModuleV2::FetchEnableModuleV2(vp::ComponentConf &config)
: vp::Component(config)
{
  this->traces.new_trace("trace", &trace, vp::INFO);
  this->new_master_port("mem", &this->mem_itf);
  this->new_master_port("fetch_en", &this->fetch_enable_itf);
  this->new_master_port("entry", &this->entry_itf);

  this->poll_event = this->event_new(&FetchEnableModuleV2::poll_handler);

  this->fetch_en_base_address = get_js_config()->get("fetch_en_addr_base")->get_int();
  this->fetch_en_addr_size = get_js_config()->get("fetch_en_addr_size")->get_int();
  this->fetch_en_pattern = (uint32_t)get_js_config()->get("fetch_en_pattern")->get_int();
  this->poll_period = get_js_config()->get("poll_period")->get_int();
  if (this->poll_period < 1)
  {
    this->poll_period = 1;
  }
  this->entry_addr = get_js_config()->get_uint("entry_addr");

  if (this->fetch_en_addr_size < sizeof(uint32_t))
  {
    this->trace.fatal("Fetch enable area too small (size: 0x%lx, needed: 0x%lx)\n",
      (unsigned long)this->fetch_en_addr_size, (unsigned long)sizeof(uint32_t));
  }
  if (this->fetch_en_base_address & (sizeof(uint32_t) - 1))
  {
    this->trace.fatal("Fetch enable word not 32-bit aligned (addr: 0x%lx)\n",
      (unsigned long)this->fetch_en_base_address);
  }
  if (this->entry_addr & (sizeof(uint32_t) - 1))
  {
    this->trace.fatal("Entry word not 32-bit aligned (addr: 0x%lx)\n",
      (unsigned long)this->entry_addr);
  }

  this->enabled = false;
  this->nb_polls = 0;
}

void FetchEnableModuleV2::reset(bool active)
{
  if (active)
  {
    this->enabled = false;
    this->nb_polls = 0;
    if (this->poll_event->is_enqueued())
    {
      this->event_cancel(this->poll_event);
    }
  }
  else if (this->mem_itf.is_bound())
  {
    this->trace.msg(vp::Trace::LEVEL_INFO,
      "Polling fetch enable word through the L2 (addr: 0x%lx, pattern: 0x%08x, period: %ld cycles)\n",
      this->fetch_en_base_address, this->fetch_en_pattern, this->poll_period);
    this->event_enqueue(this->poll_event, 1);
  }
}

bool FetchEnableModuleV2::read_word(uint64_t addr, uint32_t *value)
{
  *value = 0;
  this->mem_req = vp::IoReq(addr, (uint8_t *)value, sizeof(uint32_t), false);

  vp::IoReqStatus status = this->mem_itf.req(&this->mem_req);
  if (status != vp::IO_REQ_DONE)
  {
    // The L2 is synchronous (IoV2Sync): it always answers inline
    this->trace.fatal("Unexpected asynchronous answer from the L2 (addr: 0x%lx, status: %d)\n",
      (unsigned long)addr, (int)status);
    return false;
  }
  if (this->mem_req.get_resp_status() != vp::IO_RESP_OK)
  {
    this->trace.force_warning("Invalid L2 access (addr: 0x%lx)\n", (unsigned long)addr);
    return false;
  }
  return true;
}

void FetchEnableModuleV2::set_fetch_enable(bool active)
{
  if (active && this->poll_event->is_enqueued())
  {
    this->event_cancel(this->poll_event);
  }
  this->enabled = active;
  this->fetch_enable_itf.sync(active);
}

void FetchEnableModuleV2::check_value(uint32_t value)
{
  this->nb_polls++;
  this->trace.msg(vp::Trace::LEVEL_TRACE, "Polled fetch enable word (value: 0x%08x)\n", value);

  if (value == this->fetch_en_pattern)
  {
    this->trace.msg(vp::Trace::LEVEL_INFO,
      "Fetch enable pattern 0x%08x found after %ld polls, enabling fetch\n",
      value, this->nb_polls);
    if (this->entry_addr != 0)
    {
      // Written by the host before the pattern. Sent before the fetch enable
      // rising edge, which loads the boot address into the PC.
      uint32_t entry;
      if (!this->read_word(this->entry_addr, &entry))
      {
        return;
      }
      this->trace.msg(vp::Trace::LEVEL_INFO, "Entry from host: 0x%08x\n", entry);
      if (this->entry_itf.is_bound())
      {
        this->entry_itf.sync(entry);
      }
      else
      {
        this->trace.force_warning("Entry 0x%08x read from host but entry port not bound\n", entry);
      }
    }
    this->set_fetch_enable(true);
  }
  else
  {
    this->event_enqueue(this->poll_event, this->poll_period);
  }
}

void FetchEnableModuleV2::poll_handler(vp::Block *__this, vp::ClockEvent *event)
{
  FetchEnableModuleV2 *_this = (FetchEnableModuleV2 *)__this;

  if (_this->enabled)
  {
    return;
  }

  uint32_t value;
  if (_this->read_word(_this->fetch_en_base_address, &value))
  {
    _this->check_value(value);
  }
}

vp::IoRespAck FetchEnableModuleV2::mem_resp(vp::Block *__this, vp::IoReq *req)
{
  return vp::IO_RESP_ACCEPTED;
}

void FetchEnableModuleV2::mem_retry(vp::Block *__this, vp::IoRetryChannel channel)
{
}

extern "C" vp::Component *gv_new(vp::ComponentConf &config)
{
  return new FetchEnableModuleV2(config);
}
