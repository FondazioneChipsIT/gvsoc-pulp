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
 * memory (UMA memfd). So the module maps by itself, from the memfd, only the
 * page holding the fetch enable word (fetch_en_addr_base) and, once out of
 * reset, reads it every poll_period cycles. When the word contains
 * fetch_en_pattern, it raises fetch enable on all the bound cores and stops
 * polling. The L2 memory therefore only needs to map the L2 itself.
 *
 * The memfd offset of the word is fetch_en_addr_base - uma_host_base (host
 * address of memfd offset 0). Without memfd (e.g. PCIe VFIO mode) the module
 * does not poll.
 *
 * A write to the "input" port (from a GVSoC master) still sets the fetch
 * enable directly.
 */

#include <vp/vp.hpp>
#include <vp/itf/io.hpp>
#include <vp/itf/wire.hpp>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>

class FetchEnableModule : public vp::Component
{

public:

  FetchEnableModule(vp::ComponentConf &config);

  void reset(bool active) override;
  void stop() override;

  static vp::IoReqStatus req(vp::Block *__this, vp::IoReq *req);

private:

  static void poll_handler(vp::Block *__this, vp::ClockEvent *event);
  void map_fetch_enable_word();
  void check_value(uint32_t value);
  void set_fetch_enable(bool active);

  vp::Trace     trace;
  vp::IoSlave in;
  vp::WireMaster<bool> fetch_enable_itf;

  vp::ClockEvent *poll_event;

  uint64_t fetch_en_base_address;
  uint64_t fetch_en_addr_size;
  uint32_t fetch_en_pattern;
  int64_t poll_period;
  int64_t poll_sleep_us;
  bool enabled;
  int64_t nb_polls;

  std::string memfd;
  uint64_t uma_host_base;
  // Mapping of the page(s) holding the fetch enable word, released in stop()
  uint8_t *map_base = NULL;
  size_t map_size = 0;
  volatile uint32_t *fetch_en_word = NULL;
};

FetchEnableModule::FetchEnableModule(vp::ComponentConf &config)
: vp::Component(config)
{
  this->traces.new_trace("trace", &trace, vp::INFO);
  this->in.set_req_meth(&FetchEnableModule::req);
  this->new_slave_port("input", &this->in, this);
  this->new_master_port("fetch_en", &this->fetch_enable_itf);

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
  this->memfd = get_js_config()->get_child_str("memfd");
  this->uma_host_base = get_js_config()->get_uint("uma_host_base");

  this->enabled = false;
  this->nb_polls = 0;

  if (!this->memfd.empty())
  {
    this->map_fetch_enable_word();
  }
}

void FetchEnableModule::map_fetch_enable_word()
{
  if (this->fetch_en_addr_size < sizeof(uint32_t))
  {
    this->trace.fatal("Fetch enable area too small (size: 0x%lx, needed: 0x%lx)\n",
      (unsigned long)this->fetch_en_addr_size, (unsigned long)sizeof(uint32_t));
    return;
  }
  if (this->fetch_en_base_address & (sizeof(uint32_t) - 1))
  {
    this->trace.fatal("Fetch enable word not 32-bit aligned (addr: 0x%lx)\n",
      (unsigned long)this->fetch_en_base_address);
    return;
  }
  if (this->fetch_en_base_address < this->uma_host_base)
  {
    this->trace.fatal("Fetch enable word (0x%lx) below the host RAM base (0x%lx)\n",
      (unsigned long)this->fetch_en_base_address, (unsigned long)this->uma_host_base);
    return;
  }

  int fd = open(this->memfd.c_str(), O_RDWR);
  if (fd < 0)
  {
    this->trace.fatal("Unable to open UMA memfd %s: %s\n", this->memfd.c_str(), strerror(errno));
    return;
  }

  // mmap() needs a page-aligned offset: map the pages covering the word
  uint64_t word_offset = this->fetch_en_base_address - this->uma_host_base;
  uint64_t page_size = (uint64_t)sysconf(_SC_PAGESIZE);
  uint64_t map_offset = word_offset & ~(page_size - 1);
  uint64_t map_end = word_offset + sizeof(uint32_t);
  this->map_size = (size_t)(((map_end - map_offset) + page_size - 1) & ~(page_size - 1));

  struct stat st;
  if (fstat(fd, &st) == 0 && (uint64_t)st.st_size < map_end)
  {
    close(fd);
    this->trace.fatal("UMA memfd %s too small (size: 0x%lx, needed: 0x%lx): "
      "the host RAM must cover 0x%lx\n",
      this->memfd.c_str(), (unsigned long)st.st_size, (unsigned long)map_end,
      (unsigned long)this->fetch_en_base_address);
    return;
  }

  void *ptr = mmap(NULL, this->map_size, PROT_READ, MAP_SHARED, fd, (off_t)map_offset);
  int mmap_errno = errno;
  // The mapping keeps its own reference to the memfd
  close(fd);
  if (ptr == MAP_FAILED)
  {
    this->trace.fatal("Unable to mmap UMA memfd %s (offset: 0x%lx, size: 0x%lx): %s\n",
      this->memfd.c_str(), (unsigned long)map_offset, (unsigned long)this->map_size,
      strerror(mmap_errno));
    return;
  }

  this->map_base = (uint8_t *)ptr;
  this->fetch_en_word = (volatile uint32_t *)(this->map_base + (word_offset - map_offset));
  this->trace.msg(vp::Trace::LEVEL_INFO,
    "Mapped fetch enable word 0x%lx (memfd %s offset 0x%lx, page 0x%lx size 0x%lx)\n",
    (unsigned long)this->fetch_en_base_address, this->memfd.c_str(),
    (unsigned long)word_offset, (unsigned long)map_offset, (unsigned long)this->map_size);
}

void FetchEnableModule::stop()
{
  if (this->map_base != NULL)
  {
    munmap(this->map_base, this->map_size);
    this->map_base = NULL;
    this->fetch_en_word = NULL;
  }
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
  else if (this->fetch_en_word != NULL)
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

  if (_this->enabled || _this->fetch_en_word == NULL)
  {
    return;
  }

  // The word is written by another process (QEMU): acquire load from the shared mapping
  uint32_t value = __atomic_load_n((uint32_t *)_this->fetch_en_word, __ATOMIC_ACQUIRE);
  _this->check_value(value);
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
