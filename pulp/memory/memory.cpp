/*
 * Copyright (C) 2020 GreenWaves Technologies, SAS, ETH Zurich and
 *                    University of Bologna
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
 * Authors: Germain Haugou, GreenWaves Technologies (germain.haugou@greenwaves-technologies.com)
 */

#ifdef CONFIG_FAULT_INJECTION
#include <vp/fault_injector.hpp>
#endif
#include <stdio.h>
#include <string.h>
#include <string>
#include <vp/vp.hpp>
#include <vp/signal.hpp>
#include <vp/stats/stats.hpp>
#include <vp/itf/io.hpp>
#include <vp/itf/wire.hpp>
#include <vp/debug_mem.hpp>
#include <memory/memory_config/memory_config.hpp>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>


class Memory : public vp::Component, public vp::DebugMemIf
{

public:
    Memory(vp::ComponentConf &config);

    static vp::IoReqStatus req(vp::Block *__this, vp::IoReq *req);

    // Backdoor debug access (vp/debug_mem.hpp): direct, zero-time access to
    // the backing storage. The default debug_mem_regions() advertises this
    // memory as one flat terminal region.
    vp::DebugMemIf *debug_mem_if() override { return this; }
    int debug_mem_access(uint64_t addr, uint8_t *data, uint64_t size,
        bool is_write) override;

    MemoryConfig cfg;

private:

    void stop() override;
    void reset(bool active) override;

    static void power_ctrl_sync(vp::Block *__this, bool value);
    static void meminfo_sync_back(vp::Block *__this, void **value);
    static void meminfo_sync(vp::Block *__this, void *value);
    vp::IoReqStatus handle_write(uint64_t addr, uint64_t size, uint8_t *data, uint8_t *memcheck_data);
    vp::IoReqStatus handle_read(uint64_t addr, uint64_t size, uint8_t *data, uint8_t *memcheck_data);
    vp::IoReqStatus handle_atomic(uint64_t addr, uint64_t size, uint8_t *in_data, uint8_t *out_data,
        vp::IoReqOpcode opcode, int initiator, uint8_t *in_memcheck_data, uint8_t *out_memcheck_data);
    void log_access(uint64_t addr, uint64_t size, bool is_write);

    vp::Trace trace;
    vp::IoSlave in;


    bool check = false;
    int width_bits = -1;

    // Mask applied to incoming request addresses. -1 (all bits set) means no
    // truncation. Otherwise it's (truncate_size - 1), where truncate_size is the
    // user-provided window size to wrap addresses into.
    uint64_t truncate_mask = (uint64_t)-1;

    // Name of the memfd associated to the memory (empty if not used)
    std::string memfd;

    uint8_t *mem_data;
    // Per-byte validity shadow of the memory content, used for uninitialized-value
    // tracking when memcheck is enabled
    uint8_t *memcheck_data = NULL;
    uint8_t *check_mem;

    int64_t next_packet_start;

    vp::WireSlave<bool> power_ctrl_itf;
    vp::WireSlave<void *> meminfo_itf;

#ifdef CONFIG_FAULT_INJECTION
    std::map<uint64_t, std::vector<vp::FIRequest *>> stuck_at_map;
    bool registered_with_fic = false;
#endif

    bool power_trigger;
    bool powered_up;

    vp::PowerSource read_8_power;
    vp::PowerSource read_16_power;
    vp::PowerSource read_32_power;
    vp::PowerSource write_8_power;
    vp::PowerSource write_16_power;
    vp::PowerSource write_32_power;
    vp::PowerSource background_power;

    vp::ClockEvent *power_event;
    int64_t last_access_timestamp;

    // Load-reserved reservation table
    std::map<uint64_t, uint64_t> res_table;

    bool free_mem = false;
    // Shared UMA mapping (from memfd), released with munmap() in stop()
    uint8_t *uma_map = NULL;
    // UMA:
    // - uma_window_start: address at which this memory is mapped in the
    //   system (e.g. the MAGIA L2 base, 0xC0000000);
    // - uma_host_base: address at which the host (e.g. QEMU/RTEMS) sees
    //   offset 0 of the memfd (start of the host RAM, e.g. 0xA0000000 on
    //   the QEMU cheshire machine);
    // - uma_window_size: size of the converted window
    //   [uma_window_start, +uma_window_size), 0 disables the conversion and
    //   maps the memfd from offset 0.
    // mem_data maps the memfd from offset (uma_window_start - uma_host_base),
    // so the host and this memory use the same physical addresses.
    uint64_t uma_window_start = 0;
    uint64_t uma_host_base = 0;
    uint64_t uma_window_size = 0;

    // UMA address conversion, used by req() and debug_mem_access(): an L2
    // address (the same one the host uses) becomes an index in mem_data.
    // Any other address (e.g. already an offset) is returned untouched.
    uint64_t uma_translate(uint64_t addr) const
    {
        if (this->uma_window_size > 0 && addr >= this->uma_window_start &&
            addr  < (this->uma_window_start + this->uma_window_size))
        {
            // index in mem_data, which maps the memfd from the L2 base (see mmap)
            return addr - this->uma_window_start;
        }
        return addr;
    }
    vp::Signal<uint64_t> log_addr;
    vp::Signal<uint64_t> log_size;
    vp::Signal<bool> log_is_write;
    // Last cycle where an access was logged. Used to delay a bit in the trace the requests
    // which arrives in the same cycle
    int64_t last_logged_access = -1;
    // Number of requests logged in the same cycle. Used to delay a bit in the trace the requests
    // which arrives in the same cycle
    int nb_logged_access_in_same_cycle = 0;

    // Statistics
    vp::StatScalar stat_reads;
    vp::StatScalar stat_writes;
    vp::StatScalar stat_bytes_read;
    vp::StatScalar stat_bytes_written;
    vp::StatBw stat_read_bw;
    vp::StatBw stat_write_bw;
};



Memory::Memory(vp::ComponentConf &config)
: vp::Component(config, this->cfg),
log_addr(*this, "req_addr", 64, vp::SignalCommon::ResetKind::HighZ),
log_size(*this, "req_size", 64, vp::SignalCommon::ResetKind::HighZ),
log_is_write(*this, "req_is_write", 1, vp::SignalCommon::ResetKind::HighZ)
{
    // If no compiled tree config, fall back to JSON
    if (!this->has_tree_config())
    {
        js::Config *js = get_js_config();
        this->cfg.size = js->get("size") ? js->get("size")->get_int() : 0;
        this->cfg.atomics = js->get("atomics") ? js->get("atomics")->get_bool() : false;
        this->cfg.latency = js->get("latency") ? js->get("latency")->get_int() : 0;
    }

    traces.new_trace("trace", &trace, vp::DEBUG);
    in.set_req_meth(&Memory::req);
    new_slave_port("input", &in);

    // Register statistics
    this->stats.register_stat(&this->stat_reads, "reads", "Number of read accesses");
    this->stats.register_stat(&this->stat_writes, "writes", "Number of write accesses");
    this->stats.register_stat(&this->stat_bytes_read, "bytes_read", "Total bytes read");
    this->stats.register_stat(&this->stat_bytes_written, "bytes_written", "Total bytes written");
    this->stats.register_stat(&this->stat_read_bw, "read_bandwidth", "Average read bandwidth");
    this->stat_read_bw.set_source(&this->stat_bytes_read);
    this->stats.register_stat(&this->stat_write_bw, "write_bandwidth", "Average write bandwidth");
    this->stat_write_bw.set_source(&this->stat_bytes_written);

    this->power_ctrl_itf.set_sync_meth(&Memory::power_ctrl_sync);
    new_slave_port("power_ctrl", &this->power_ctrl_itf);

    this->meminfo_itf.set_sync_back_meth(&Memory::meminfo_sync_back);
    this->meminfo_itf.set_sync_meth(&Memory::meminfo_sync);
    new_slave_port("meminfo", &this->meminfo_itf);

    js::Config *js_config = get_js_config()->get("power_trigger");
    this->power_trigger = js_config != NULL && js_config->get_bool();

    power.new_power_source("leakage", &background_power, this->get_js_config()->get("**/background"));
    power.new_power_source("read_8", &read_8_power, this->get_js_config()->get("**/read_8"));
    power.new_power_source("read_16", &read_16_power, this->get_js_config()->get("**/read_16"));
    power.new_power_source("read_32", &read_32_power, this->get_js_config()->get("**/read_32"));
    power.new_power_source("write_8", &write_8_power, this->get_js_config()->get("**/write_8"));
    power.new_power_source("write_16", &write_16_power, this->get_js_config()->get("**/write_16"));
    power.new_power_source("write_32", &write_32_power, this->get_js_config()->get("**/write_32"));

    // Fields not yet in MemoryConfig (still via JSON)
    this->check = get_js_config()->get_child_bool("check");
    this->width_bits = get_js_config()->get_child_int("width_bits");
    int align = get_js_config()->get_child_int("align");
    this->memfd = get_js_config()->get_child_str("memfd");
    this->uma_window_start = get_js_config()->get_uint("uma_window_start");
    this->uma_host_base = get_js_config()->get_uint("uma_host_base");
    this->uma_window_size = get_js_config()->get_uint("uma_window_size");
    if (this->uma_window_size > 0 && this->uma_window_start < this->uma_host_base)
    {
        this->trace.fatal("UMA base 0x%lx is below the host RAM base 0x%lx\n",
            (unsigned long)this->uma_window_start, (unsigned long)this->uma_host_base);
        return;
    }
    if (this->uma_window_size > 0)
    {
        this->trace.msg("UMA L2 window [0x%lx, 0x%lx), host RAM base 0x%lx\n",
            (unsigned long)this->uma_window_start,
            (unsigned long)(this->uma_window_start + this->uma_window_size),
            (unsigned long)this->uma_host_base);
    }

    // Optional address truncation. 0 means inactive; any non-zero value (which
    // should be a power of 2) is taken as the truncation window — incoming addresses
    // are AND-masked with (truncate_size - 1) before being used as offsets.
    int64_t truncate_size = get_js_config()->get_int("truncate_size");
    if (truncate_size > 0)
    {
        this->truncate_mask = (uint64_t)truncate_size - 1;
    }

    int umaFd=-1;
    uint8_t* uma_mem_data=NULL;
    if(!this->memfd.empty())
    {
        this->trace.msg("Building UMA Memory (size: 0x%x, check: %d) memfd %s\n", this->cfg.size, check, this->memfd.c_str());
        umaFd = open(this->memfd.c_str(), O_RDWR);
        if (umaFd < 0)
        {
            this->trace.fatal("Unable to open UMA memfd %s: %s\n", this->memfd.c_str(), strerror(errno));
            return;
        }
        this->trace.msg("Opened umaFd=%d\n", umaFd);

        // The memfd is the host RAM, offset 0 = uma_host_base (e.g. QEMU cheshire
        // 0xA0000000). Map the part the host sees at the L2 base, so that the
        // same physical address is used on both sides:
        //   memfd offset (uma_window_start - uma_host_base) + x
        //     = host uma_window_start + x = L2 uma_window_start + x
        uint64_t memfd_offset = 0;
        if (this->uma_window_size > 0)
        {
            memfd_offset = this->uma_window_start - this->uma_host_base;
        }
        struct stat st;
        if (fstat(umaFd, &st) == 0 &&
            (uint64_t)st.st_size < memfd_offset + (uint64_t)this->cfg.size)
        {
            close(umaFd);
            this->trace.fatal("UMA memfd %s too small (size: 0x%lx, needed: 0x%lx): "
                "the host RAM must cover 0x%lx-0x%lx\n",
                this->memfd.c_str(), (unsigned long)st.st_size,
                (unsigned long)(memfd_offset + this->cfg.size),
                (unsigned long)(this->uma_host_base + memfd_offset),
                (unsigned long)(this->uma_host_base + memfd_offset + this->cfg.size));
            return;
        }
        this->trace.msg("UMA memfd offset 0x%lx (host address 0x%lx) -> L2 0x%lx\n",
            (unsigned long)memfd_offset,
            (unsigned long)(this->uma_host_base + memfd_offset),
            (unsigned long)this->uma_window_start);

        uma_mem_data = (uint8_t*) mmap(NULL, this->cfg.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                                       umaFd, (off_t)memfd_offset);
        int mmap_errno = errno;
        // The mapping keeps its own reference to the memfd, the fd is no longer needed
        close(umaFd);
        if (uma_mem_data == MAP_FAILED)
        {
            this->trace.fatal("Unable to mmap UMA memfd %s (size: 0x%lx): %s\n",
                this->memfd.c_str(), (unsigned long)this->cfg.size, strerror(mmap_errno));
            return;
        }
        this->uma_map = uma_mem_data;
    }
    else
    {
        //Do nothing
    }

    if (align)
    {
        if(uma_mem_data==NULL)
        {
            mem_data = (uint8_t *)aligned_alloc(align, this->cfg.size);
        }
        else
        {
            mem_data = uma_mem_data;
        }
    }
    else
    {
        if(uma_mem_data==NULL)
        {
            mem_data = (uint8_t *)calloc(this->cfg.size, 1);
            if (mem_data == NULL) throw std::bad_alloc();
        }
        else
        {
            mem_data = uma_mem_data;
        }
        
    }
    // UMA memory comes from mmap() and must be released with munmap(), not free()
    this->free_mem = (uma_mem_data == NULL);

    // Special option to check for uninitialized accesses
    if (check)
    {
        check_mem = new uint8_t[(this->cfg.size + 7) / 8];
    }
    else
    {
        check_mem = NULL;
    }

    if (this->traces.get_trace_engine()->is_memcheck_enabled())
    {
        this->memcheck_data = (uint8_t *)calloc(this->cfg.size, 1);
        if (this->memcheck_data == NULL) throw std::bad_alloc();
    }

    // Initialize the Memory with a special value to detect uninitialized
    // variables.
    // Only do it for small memories to not slow down too much simulation init.
    if (this->get_js_config()->get_child_bool("init") && this->cfg.size < (2<<24))
    {
        memset(mem_data, 0x57, this->cfg.size);
    }

    // Preload the Memory
    js::Config *stim_file_conf = this->get_js_config()->get("stim_file");
    if (stim_file_conf != NULL)
    {
        string path = stim_file_conf->get_str();
        if (path != "")
        {
            trace.msg("Preloading Memory with stimuli file (path: %s)\n", path.c_str());

            FILE *file = fopen(path.c_str(), "rb");
            if (file == NULL)
            {
                this->trace.fatal("Unable to open stim file: %s, %s\n", path.c_str(), strerror(errno));
                return;
            }
            if (fread(this->mem_data, 1, this->cfg.size, file) == 0)
            {
                this->trace.fatal("Failed to read stim file: %s, %s\n", path.c_str(), strerror(errno));
                return;
            }
        }
    }

    this->background_power.leakage_power_start();
    this->background_power.dynamic_power_start();
    this->last_access_timestamp = -1;
}


void Memory::log_access(uint64_t addr, uint64_t size, bool is_write)
{
    int64_t cycles = this->clock.get_cycles();

    if (cycles > this->last_logged_access)
    {
        this->nb_logged_access_in_same_cycle = 0;
    }

    int64_t delay = 0;
    if (this->nb_logged_access_in_same_cycle > 0)
    {
        int64_t period = this->clock.get_period();
        delay = period - (period >> this->nb_logged_access_in_same_cycle);
    }
    this->log_addr.set_and_release(addr, 0, delay);
    this->log_size.set_and_release(size, 0, delay);
    this->log_is_write.set_and_release(is_write, 0, delay);
    this->nb_logged_access_in_same_cycle++;
    this->last_logged_access = cycles;
}


vp::IoReqStatus Memory::req(vp::Block *__this, vp::IoReq *req)
{
    Memory *_this = (Memory *)__this;

    // UMA address conversion (L2 address -> index in mem_data), so that the
    // shared buffers are reachable with the same address from both sides.
    uint64_t offset = _this->uma_translate(req->get_addr()) & _this->truncate_mask;
    uint8_t *data = req->get_data();
    uint64_t size = req->get_size();

    _this->trace.msg("Memory access (offset: 0x%x, size: 0x%x, is_write: %d, op: %d)\n", offset, size, req->get_is_write(), req->get_opcode());

    // Count stats
    if (req->get_is_write())
    {
        _this->stat_writes++;
        _this->stat_bytes_written += size;
    }
    else
    {
        _this->stat_reads++;
        _this->stat_bytes_read += size;
    }

    _this->log_access(offset, size, req->get_is_write());

    req->inc_latency(_this->cfg.latency);

    if (!req->is_debug())
    {
        // Impact the Memory bandwith on the packet
        if (_this->width_bits != -1)
        {
    #define MAX(a, b) (((a) > (b)) ? (a) : (b))
            int duration = ((size - 1) >> _this->width_bits) + 1;
            req->set_duration(duration);
            int64_t cycles = _this->clock.get_cycles();
            int64_t diff = _this->next_packet_start - cycles;
            if (diff > 0)
            {
                _this->trace.msg("Delayed packet (latency: %ld)\n", diff);
                // Set the latency, this will only set if it is higher than current latency, which
                // is what we want, as the already existing latency already meets our cyclestamp
                req->set_latency(diff);
            }
            _this->next_packet_start = MAX(_this->next_packet_start, cycles) + duration;
        }

        if (_this->power.is_enabled())
        {
            _this->last_access_timestamp = _this->time.get_time();

            if (req->get_is_write())
            {
                if (size == 1)
                    _this->write_8_power.account_energy_quantum();
                else if (size == 2)
                    _this->write_16_power.account_energy_quantum();
                else if (size == 4)
                    _this->write_32_power.account_energy_quantum();
            }
            else
            {
                if (size == 1)
                    _this->read_8_power.account_energy_quantum();
                else if (size == 2)
                    _this->read_16_power.account_energy_quantum();
                else if (size == 4)
                    _this->read_32_power.account_energy_quantum();
            }
        }
    }

#ifdef VP_TRACE_ACTIVE
    if (_this->power_trigger)
    {
        if (req->get_is_write() && size == 4 && offset == 0)
        {
            if (*(uint32_t *)data == 0xabbaabba)
            {
                _this->power.get_engine()->start_capture();
            }
            else if (*(uint32_t *)data == 0xdeadcaca)
            {
                static int measure_index = 0;
                _this->power.get_engine()->stop_capture();
                double dynamic_power, static_power;
                fprintf(stderr, "@power.measure_%d@%f@\n", measure_index++, _this->power.get_engine()->get_average_power(dynamic_power, static_power));
            }
        }
    }
#endif

    if (offset + size > _this->cfg.size)
    {
        _this->trace.force_warning_no_error("Received out-of-bound request (reqAddr: 0x%x, reqSize: 0x%x, memSize: 0x%x)\n", offset, size, _this->cfg.size);
        return vp::IO_REQ_INVALID;
    }

#ifdef CONFIG_FAULT_INJECTION
    if (req->fault_upset_request)
    {
        _this->mem_data[offset] = _this->mem_data[offset] ^ req->mask;
        return vp::IO_REQ_OK;
    }
#endif

    if (req->get_opcode() == vp::IoReqOpcode::READ)
    {
#ifdef VP_MEMCHECK_ACTIVE
        return _this->handle_read(offset, size, data, req->get_memcheck_data());
#else
        return _this->handle_read(offset, size, data, NULL);
#endif
    }
    else if (req->get_opcode() == vp::IoReqOpcode::WRITE)
    {
#ifdef VP_MEMCHECK_ACTIVE
        return _this->handle_write(offset, size, data, req->get_memcheck_data());
#else
        return _this->handle_write(offset, size, data, NULL);
#endif
    }
    else
    {
#ifdef CONFIG_ATOMICS
#ifdef VP_MEMCHECK_ACTIVE
        return _this->handle_atomic(offset, size, data, req->get_second_data(), req->get_opcode(),
            req->get_initiator(), req->get_memcheck_data(), req->get_second_memcheck_data());
#else
        return _this->handle_atomic(offset, size, data, req->get_second_data(), req->get_opcode(),
            req->get_initiator(), NULL, NULL);
#endif
#else
        _this->trace.force_warning("Received unsupported atomic operation\n");
        return vp::IO_REQ_INVALID;
#endif
    }
}



int Memory::debug_mem_access(uint64_t addr, uint8_t *data, uint64_t size, bool is_write)
{
    // Same UMA address conversion as req(), so that debug accesses (e.g. gdb)
    // can also use the L2 addresses (the same ones the host uses)
    uint64_t offset = this->uma_translate(addr) & this->truncate_mask;

    if (offset + size > (uint64_t)this->cfg.size)
    {
        return -1;
    }

    if (is_write)
    {
        memcpy((void *)&this->mem_data[offset], (void *)data, size);
    }
    else
    {
        memcpy((void *)data, (void *)&this->mem_data[offset], size);
    }

    return 0;
}



vp::IoReqStatus Memory::handle_write(uint64_t offset, uint64_t size, uint8_t *data, uint8_t *req_memcheck_data)
{
    // Writes on powered-down memory are silently ignored
    if (!this->powered_up)
    {
        return vp::IO_REQ_OK;
    }

#ifdef VP_MEMCHECK_ACTIVE
    if (this->memcheck_data != NULL)
    {
        if (req_memcheck_data != NULL)
        {
            memcpy((void *)&this->memcheck_data[offset], (void *)req_memcheck_data, size);
        }
        else
        {
            // If there is no memcheck data, it means the model which is writting does not have
            // support to it. Set all data to valid to avoid false negative
            memset((void *)&this->memcheck_data[offset], -1, size);
        }
    }
#endif

    if (data)
    {
        memcpy((void *)&this->mem_data[offset], (void *)data, size);

#ifdef CONFIG_FAULT_INJECTION
        auto it = this->stuck_at_map.lower_bound(offset);

        while (it != this->stuck_at_map.end() && it->first < offset + size)
        {
            for (const auto& tf : it->second)
            {
                uint8_t mask = (uint8_t) (1u << tf->bit);
                
                
                if (tf->is_high)
                {
                    this->mem_data[tf->addr] |= mask;
                }
                else
                {
                    this->mem_data[tf->addr] &= ~mask;
                }
            }

            ++it;
        }

#endif
    }

    return vp::IO_REQ_OK;
}


vp::IoReqStatus Memory::handle_read(uint64_t offset, uint64_t size, uint8_t *data, uint8_t *req_memcheck_data)
{
    // Reads on powered-down memory return 0
    if (!this->powered_up)
    {
        memset((void *)data, 0, size);
        return vp::IO_REQ_OK;
    }

#ifdef VP_MEMCHECK_ACTIVE
    if (this->memcheck_data != NULL)
    {
        if (req_memcheck_data != NULL)
        {
            memcpy((void *)req_memcheck_data, (void *)&this->memcheck_data[offset], size);
        }
    }
#endif

    if (data)
    {
        memcpy((void *)data, (void *)&this->mem_data[offset], size);
    }

    return vp::IO_REQ_OK;
}


static inline int64_t get_signed_value(int64_t val, int bits)
{
    return ((int64_t)val) << (64 - bits) >> (64 - bits);
}


vp::IoReqStatus Memory::handle_atomic(uint64_t addr, uint64_t size, uint8_t *in_data,
    uint8_t *out_data, vp::IoReqOpcode opcode, int initiator, uint8_t *in_memcheck_data,
    uint8_t *out_memcheck_data)
{
    int64_t operand = 0;
    int64_t prev_val = 0;
    int64_t result;
    bool is_write = true;

    memcpy((uint8_t *)&operand, in_data, size);

    this->handle_read(addr, size, (uint8_t *)&prev_val, out_memcheck_data);

    if (size < 8)
    {
        operand = get_signed_value(operand, size*8);
        prev_val = get_signed_value(prev_val, size*8);
    }

    switch (opcode)
    {
        case vp::IoReqOpcode::LR:
            this->res_table[initiator] = addr;
            is_write = false;
            break;
        case vp::IoReqOpcode::SC:
        {
            auto it = this->res_table.find(initiator);
            if (it != this->res_table.end() && it->second == addr)
            {
                // Valid reservation --> clear all others as we are going to write
                for (auto &it : this->res_table) {
                    if (it.second >= addr && it.second < addr+size)
                    {
                        it.second = -1;
                    }
                }
                result   = operand;
                prev_val = 0;
            }
            else
            {
                is_write = false;
                prev_val = 1;
            }
            break;
        }
        case vp::IoReqOpcode::SWAP:
            result = operand;
            break;
        case vp::IoReqOpcode::ADD:
            result = prev_val + operand;
            break;
        case vp::IoReqOpcode::XOR:
            result = prev_val ^ operand;
            break;
        case vp::IoReqOpcode::AND:
            result = prev_val & operand;
            break;
        case vp::IoReqOpcode::OR:
            result = prev_val | operand;
            break;
        case vp::IoReqOpcode::MIN:
            result = prev_val < operand ? prev_val : operand;
            break;
        case vp::IoReqOpcode::MAX:
            result = prev_val > operand ? prev_val : operand;
            break;
        case vp::IoReqOpcode::MINU:
            result = (uint64_t) prev_val < (uint64_t) operand ? prev_val : operand;
            break;
        case vp::IoReqOpcode::MAXU:
            result = (uint64_t) prev_val > (uint64_t) operand ? prev_val : operand;
            break;
        default:
            return vp::IO_REQ_INVALID;
    }

    memcpy(out_data, (uint8_t *)&prev_val, size);
    if (is_write)
    {
        this->handle_write(addr, size, (uint8_t *)&result, in_memcheck_data);
    }

    return vp::IO_REQ_OK;
}


void Memory::stop()
{
    if (this->free_mem)
    {
        free(this->mem_data);
        this->free_mem = false;
    }
    if (this->uma_map != NULL)
    {
        if (this->mem_data == this->uma_map)
        {
            this->mem_data = NULL;
        }
        munmap(this->uma_map, this->cfg.size);
        this->uma_map = NULL;
    }
    if (this->check)
    {
        delete[] this->check_mem;
    }
}

void Memory::reset(bool active)
{
    if (active)
    {
        this->next_packet_start = 0;
        this->powered_up = true;

#ifdef CONFIG_FAULT_INJECTION
        if (!this->registered_with_fic)
        {
            bool fic_enabled = this->get_js_config()->get_child_bool("fic_enabled");
            if (fic_enabled)
            {
                vp::FIC_registrator *fic = (vp::FIC_registrator *) this->get_service("FIC");
                fic->register_memory(this, this->mem_data, this->cfg.size, this->stuck_at_map);
                this->registered_with_fic = true;
            }
        }
#endif
    }
}



void Memory::power_ctrl_sync(vp::Block *__this, bool value)
{
    Memory *_this = (Memory *)__this;
    _this->powered_up = value;
}



void Memory::meminfo_sync_back(vp::Block *__this, void **value)
{
    Memory *_this = (Memory *)__this;
    *value = _this->mem_data;
}



void Memory::meminfo_sync(vp::Block *__this, void *value)
{
    Memory *_this = (Memory *)__this;
    _this->mem_data = (uint8_t *)value;
    _this->free_mem = false;
}



extern "C" vp::Component *gv_new(vp::ComponentConf &config)
{
    return new Memory(config);
}
