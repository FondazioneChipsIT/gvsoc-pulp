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
 * Authors: Riccardo Repetto, Chips-IT (riccardo.repetto@chips.it)
 */

/*
 * Fetch enable module, reading the host control words from a memfd.
 *
 * The host (QEMU) cannot generate GVSoC requests: it only writes shared
 * memory. The module maps by itself, from the memfd, the page(s) holding the
 * fetch enable word (fetch_en_addr_base) and the entry word (entry_addr), and
 * once out of reset reads the fetch enable word every poll_period cycles. When
 * it contains fetch_en_pattern, the module reads the entry word, sends it on
 * the "entry" port (core boot address) and then raises fetch enable on all the
 * bound cores, since the cores take the boot address as PC on the fetch enable
 * rising edge. Then it stops polling.
 *
 * memfd_host_base is the host address of memfd offset 0: a word at host
 * address A is at memfd offset A - memfd_host_base. The same module serves:
 *   - a CSR memfd holding the page 0xCCFF_0000 (memfd_host_base = 0xCCFF_0000),
 *     mapped by QEMU over (or instead of) that page of its RAM;
 *   - the RAM memfd itself (memfd_host_base = UMA host RAM base), when the
 *     control words are left in the host RAM / L2.
 *
 * The words are read directly from the mapping, not through the L2 model:
 * the polling does not appear in the L2 traces and has no L2 latency.
 */

#include <vp/vp.hpp>
#include <vp/itf/wire.hpp>
#include <stdio.h>
#include <string.h>
#include <string>
#include <algorithm>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>

class FetchEnableModuleV2 : public vp::Component
{

public:

  FetchEnableModuleV2(vp::ComponentConf &config);

  void reset(bool active) override;
  void stop() override;

private:

  static void poll_handler(vp::Block *__this, vp::ClockEvent *event);
  // Maps the page(s) holding the control words, false on error
  bool map_words();
  bool check_word_addr(const char *name, uint64_t addr);
  void check_value(uint32_t value);
  void set_fetch_enable(bool active);

  vp::Trace     trace;
  vp::WireMaster<bool> fetch_enable_itf;
  vp::WireMaster<uint64_t> entry_itf;

  vp::ClockEvent *poll_event;

  uint64_t fetch_en_base_address;
  uint64_t fetch_en_addr_size;
  uint32_t fetch_en_pattern;
  int64_t poll_period;
  std::string memfd;
  uint64_t memfd_host_base;
  // Address of the entry word, 0 if the entry does not come from the host
  uint64_t entry_addr;

  uint8_t *map_base = NULL;
  size_t map_size = 0;
  volatile uint32_t *fetch_en_word = NULL;
  volatile uint32_t *entry_word = NULL;

  bool enabled;
  int64_t nb_polls;
};

FetchEnableModuleV2::FetchEnableModuleV2(vp::ComponentConf &config)
: vp::Component(config)
{
  this->traces.new_trace("trace", &trace, vp::INFO);
  this->new_master_port("fetch_en", &this->fetch_enable_itf);
  this->new_master_port("entry", &this->entry_itf);

  this->poll_event = this->event_new(&FetchEnableModuleV2::poll_handler);

  this->fetch_en_base_address = get_js_config()->get_uint("fetch_en_addr_base");
  this->fetch_en_addr_size = get_js_config()->get_uint("fetch_en_addr_size");
  this->fetch_en_pattern = (uint32_t)get_js_config()->get_uint("fetch_en_pattern");
  this->poll_period = get_js_config()->get("poll_period")->get_int();
  if (this->poll_period < 1)
  {
    this->poll_period = 1;
  }
  this->memfd = get_js_config()->get_child_str("memfd");
  this->memfd_host_base = get_js_config()->get_uint("memfd_host_base");
  this->entry_addr = get_js_config()->get_uint("entry_addr");

  this->enabled = false;
  this->nb_polls = 0;

  if (this->memfd.empty())
  {
    this->trace.fatal("No memfd given for the fetch enable and entry words\n");
    return;
  }
  this->map_words();
}

bool FetchEnableModuleV2::check_word_addr(const char *name, uint64_t addr)
{
  if (addr & (sizeof(uint32_t) - 1))
  {
    this->trace.fatal("%s word not 32-bit aligned (addr: 0x%lx)\n", name, (unsigned long)addr);
    return false;
  }
  if (addr < this->memfd_host_base)
  {
    this->trace.fatal("%s word (0x%lx) below the memfd host base (0x%lx)\n",
      name, (unsigned long)addr, (unsigned long)this->memfd_host_base);
    return false;
  }
  return true;
}

bool FetchEnableModuleV2::map_words()
{
  uint64_t page_size = (uint64_t)sysconf(_SC_PAGESIZE);

  if (this->fetch_en_addr_size < sizeof(uint32_t))
  {
    this->trace.fatal("Fetch enable area too small (size: 0x%lx, needed: 0x%lx)\n",
      (unsigned long)this->fetch_en_addr_size, (unsigned long)sizeof(uint32_t));
    return false;
  }
  // memfd offset 0 is page-aligned by definition: so must be its host address
  if (this->memfd_host_base & (page_size - 1))
  {
    this->trace.fatal("memfd host base 0x%lx not page-aligned\n",
      (unsigned long)this->memfd_host_base);
    return false;
  }
  if (!this->check_word_addr("Fetch enable", this->fetch_en_base_address))
  {
    return false;
  }
  if (this->entry_addr != 0 && !this->check_word_addr("Entry", this->entry_addr))
  {
    return false;
  }

  // Pages covering the word(s)
  uint64_t fetch_en_offset = this->fetch_en_base_address - this->memfd_host_base;
  uint64_t first_offset = fetch_en_offset;
  uint64_t map_end = fetch_en_offset + sizeof(uint32_t);
  uint64_t entry_offset = 0;
  if (this->entry_addr != 0)
  {
    entry_offset = this->entry_addr - this->memfd_host_base;
    first_offset = std::min(first_offset, entry_offset);
    map_end = std::max(map_end, entry_offset + sizeof(uint32_t));
  }
  uint64_t map_offset = first_offset & ~(page_size - 1);
  this->map_size = (size_t)(((map_end - map_offset) + page_size - 1) & ~(page_size - 1));

  int fd = open(this->memfd.c_str(), O_RDWR);
  if (fd < 0)
  {
    this->trace.fatal("Unable to open memfd %s: %s\n", this->memfd.c_str(), strerror(errno));
    return false;
  }

  struct stat st;
  if (fstat(fd, &st) == 0 && (uint64_t)st.st_size < map_end)
  {
    close(fd);
    this->trace.fatal("memfd %s too small (size: 0x%lx, needed: 0x%lx): "
      "it must cover host address 0x%lx\n",
      this->memfd.c_str(), (unsigned long)st.st_size, (unsigned long)map_end,
      (unsigned long)(this->memfd_host_base + map_end - 1));
    return false;
  }

  void *ptr = mmap(NULL, this->map_size, PROT_READ, MAP_SHARED, fd, (off_t)map_offset);
  int mmap_errno = errno;
  // The mapping keeps its own reference to the memfd
  close(fd);
  if (ptr == MAP_FAILED)
  {
    this->trace.fatal("Unable to mmap memfd %s (offset: 0x%lx, size: 0x%lx): %s\n",
      this->memfd.c_str(), (unsigned long)map_offset, (unsigned long)this->map_size,
      strerror(mmap_errno));
    return false;
  }

  this->map_base = (uint8_t *)ptr;
  this->fetch_en_word = (volatile uint32_t *)(this->map_base + (fetch_en_offset - map_offset));
  this->trace.msg(vp::Trace::LEVEL_INFO,
    "Mapped fetch enable word 0x%lx (memfd %s offset 0x%lx, mapping 0x%lx size 0x%lx)\n",
    (unsigned long)this->fetch_en_base_address, this->memfd.c_str(),
    (unsigned long)fetch_en_offset, (unsigned long)map_offset, (unsigned long)this->map_size);

  if (this->entry_addr != 0)
  {
    this->entry_word = (volatile uint32_t *)(this->map_base + (entry_offset - map_offset));
    this->trace.msg(vp::Trace::LEVEL_INFO, "Mapped entry word 0x%lx (memfd offset 0x%lx)\n",
      (unsigned long)this->entry_addr, (unsigned long)entry_offset);
  }
  return true;
}

void FetchEnableModuleV2::stop()
{
  if (this->map_base != NULL)
  {
    munmap(this->map_base, this->map_size);
    this->map_base = NULL;
    this->fetch_en_word = NULL;
    this->entry_word = NULL;
  }
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
  else if (this->fetch_en_word != NULL)
  {
    this->trace.msg(vp::Trace::LEVEL_INFO,
      "Polling fetch enable word (addr: 0x%lx, pattern: 0x%08x, period: %ld cycles)\n",
      this->fetch_en_base_address, this->fetch_en_pattern, this->poll_period);
    this->event_enqueue(this->poll_event, 1);
  }
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
    if (this->entry_word != NULL)
    {
      // Written by the host before the pattern: the acquire load of the pattern
      // orders this read after it. Sent before the fetch enable rising edge,
      // which loads the boot address into the PC.
      uint32_t entry = __atomic_load_n((uint32_t *)this->entry_word, __ATOMIC_ACQUIRE);
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

  if (_this->enabled || _this->fetch_en_word == NULL)
  {
    return;
  }

  // The word is written by another process (QEMU): acquire load from the shared mapping
  uint32_t value = __atomic_load_n((uint32_t *)_this->fetch_en_word, __ATOMIC_ACQUIRE);
  _this->check_value(value);
}

extern "C" vp::Component *gv_new(vp::ComponentConf &config)
{
  return new FetchEnableModuleV2(config);
}
