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
 * Fetch enable module.
 *
 * The host (QEMU) cannot generate GVSoC requests: it only writes the shared
 * memory (UMA memfd). So, once out of reset, this module polls a word of the
 * L2 (fetch_en_addr_base) through its "mem" port every poll_period cycles.
 * When the word contains fetch_en_pattern, it raises fetch enable on all the
 * bound cores and stops polling.
 *
 * A write to the "input" port (from a GVSoC master) still sets the fetch
 * enable directly.
 */

#include <vp/vp.hpp>
#include <vp/itf/io.hpp>
#include <vp/itf/wire.hpp>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

class FetchEnableModule : public vp::Component
{

public:

  FetchEnableModule(vp::ComponentConf &config);

  void reset(bool active) override;

  static vp::IoReqStatus req(vp::Block *__this, vp::IoReq *req);

private:

  static void poll_handler(vp::Block *__this, vp::ClockEvent *event);
  static void mem_response(vp::Block *__this, vp::IoReq *req);
  void check_value(uint32_t value);
  void set_fetch_enable(bool active);

  vp::Trace     trace;
  vp::IoSlave in;
  vp::IoMaster mem_itf;
  vp::WireMaster<bool> fetch_enable_itf;

  vp::ClockEvent *poll_event;
  vp::IoReq poll_req;
  uint32_t poll_data;

  uint64_t fetch_en_base_address;
  uint64_t fetch_en_addr_size;
  uint32_t fetch_en_pattern;
  int64_t poll_period;
  int64_t poll_sleep_us;
  bool enabled;
  int64_t nb_polls;
};

FetchEnableModule::FetchEnableModule(vp::ComponentConf &config)
: vp::Component(config)
{
  this->traces.new_trace("trace", &trace, vp::INFO);
  this->in.set_req_meth(&FetchEnableModule::req);
  this->new_slave_port("input", &this->in, this);
  this->new_master_port("fetch_en", &this->fetch_enable_itf);

  this->mem_itf.set_resp_meth(&FetchEnableModule::mem_response);
  this->new_master_port("mem", &this->mem_itf);

  this->poll_event = this->event_new(&FetchEnableModule::poll_handler);

  this->fetch_en_base_address = get_js_config()->get("fetch_en_addr_base")->get_int();
  this->fetch_en_addr_size = get_js_config()->get("fetch_en_addr_size")->get_int();
  this->fetch_en_pattern = (uint32_t)get_js_config()->get("fetch_en_pattern")->get_int();
  this->poll_period = get_js_config()->get("poll_period")->get_int();
  if (this->poll_period < 1)
  {
    this->poll_period = 1;
  }
  this->poll_sleep_us = get_js_config()->get("poll_sleep_us")->get_int();

  this->enabled = false;
  this->nb_polls = 0;
}

void FetchEnableModule::reset(bool active)
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
      "Polling fetch enable word (addr: 0x%lx, pattern: 0x%08x, period: %ld cycles, sleep: %ld us)\n",
      this->fetch_en_base_address, this->fetch_en_pattern, this->poll_period, this->poll_sleep_us);
    this->event_enqueue(this->poll_event, 1);
  }
}

void FetchEnableModule::set_fetch_enable(bool active)
{
  if (active && this->poll_event->is_enqueued())
  {
    this->event_cancel(this->poll_event);
  }
  this->enabled = active;
  this->fetch_enable_itf.sync(active);
}

void FetchEnableModule::check_value(uint32_t value)
{
  this->nb_polls++;
  this->trace.msg(vp::Trace::LEVEL_TRACE, "Polled fetch enable word (value: 0x%08x)\n", value);

  if (value == this->fetch_en_pattern)
  {
    this->trace.msg(vp::Trace::LEVEL_INFO,
      "Fetch enable pattern 0x%08x found after %ld polls, enabling fetch\n",
      value, this->nb_polls);
    this->set_fetch_enable(true);
  }
  else
  {
    // The cores are stalled and the poll is the only event: without a wall-clock
    // pause the engine would poll (and log) millions of times while the host boots.
    if (this->poll_sleep_us > 0)
    {
      usleep(this->poll_sleep_us);
    }
    this->event_enqueue(this->poll_event, this->poll_period);
  }
}

void FetchEnableModule::poll_handler(vp::Block *__this, vp::ClockEvent *event)
{
  FetchEnableModule *_this = (FetchEnableModule *)__this;

  if (_this->enabled)
  {
    return;
  }

  _this->poll_data = 0;
  _this->poll_req.init();
  _this->poll_req.set_addr(_this->fetch_en_base_address);
  _this->poll_req.set_size(sizeof(uint32_t));
  _this->poll_req.set_is_write(false);
  _this->poll_req.set_data((uint8_t *)&_this->poll_data);

  vp::IoReqStatus err = _this->mem_itf.req(&_this->poll_req);
  if (err == vp::IO_REQ_OK)
  {
    _this->check_value(_this->poll_data);
  }
  else if (err == vp::IO_REQ_INVALID)
  {
    _this->trace.force_warning("Invalid access while polling fetch enable word (addr: 0x%lx)\n",
      _this->fetch_en_base_address);
  }
  // IO_REQ_PENDING: the value is checked in mem_response
}

void FetchEnableModule::mem_response(vp::Block *__this, vp::IoReq *req)
{
  FetchEnableModule *_this = (FetchEnableModule *)__this;
  _this->check_value(_this->poll_data);
}

vp::IoReqStatus FetchEnableModule::req(vp::Block *__this, vp::IoReq *req)
{
    FetchEnableModule *_this = (FetchEnableModule *)__this;

    uint64_t offset = req->get_addr();
    uint64_t size = req->get_size();
    bool is_write = req->get_is_write();
    uint8_t *data = req->get_data();
    uint32_t fetch_enable = 0;

    if ((!is_write) || (size!=sizeof(uint32_t)))
      return vp::IO_REQ_INVALID;
    else {

      if ((offset>=_this->fetch_en_base_address) && (offset<(_this->fetch_en_base_address)+_this->fetch_en_addr_size)) {
        memcpy((uint8_t*)&fetch_enable, data, size);

        _this->trace.msg(vp::Trace::LEVEL_INFO, "FETCH_ENABLE written 0x%08x\n", fetch_enable);
        _this->set_fetch_enable(fetch_enable != 0);
      }

      return vp::IO_REQ_OK;
    }
}

extern "C" vp::Component *gv_new(vp::ComponentConf &config)
{
  return new FetchEnableModule(config);
}
