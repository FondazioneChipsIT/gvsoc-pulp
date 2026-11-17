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
 * UMA backing of an io_v2 memory (memory.memory_v3).
 *
 * Maps the window [window_start - host_base, + size) of the shared memory
 * file (memfd, the host RAM whose offset 0 is at host_base) and, at start,
 * gives it to the memory as its backing array through the meminfo port. The
 * memory then reads and writes the host RAM directly, at the same physical
 * addresses as the host.
 *
 * The mapping is done in the constructor, so that an error is reported
 * before the simulation starts, and released in stop().
 */

#include <vp/vp.hpp>
#include <vp/itf/wire.hpp>
#include <string.h>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>

class UmaMemfd : public vp::Component
{
public:
    UmaMemfd(vp::ComponentConf &config);

    void start() override;
    void stop() override;

private:
    vp::Trace trace;
    vp::WireMaster<void *> meminfo_itf;

    std::string memfd;
    uint64_t host_base;
    uint64_t window_start;
    uint64_t size;

    uint8_t *map = NULL;
};

UmaMemfd::UmaMemfd(vp::ComponentConf &config)
: vp::Component(config)
{
    this->traces.new_trace("trace", &this->trace, vp::INFO);
    this->new_master_port("meminfo", &this->meminfo_itf);

    this->memfd = this->get_js_config()->get_child_str("memfd");
    this->host_base = this->get_js_config()->get_uint("host_base");
    this->window_start = this->get_js_config()->get_uint("window_start");
    this->size = this->get_js_config()->get_uint("size");

    uint64_t offset = this->window_start - this->host_base;
    uint64_t page_size = (uint64_t)sysconf(_SC_PAGESIZE);
    if (offset & (page_size - 1))
    {
        this->trace.fatal("UMA window offset 0x%lx not page-aligned\n", (unsigned long)offset);
        return;
    }

    int fd = open(this->memfd.c_str(), O_RDWR);
    if (fd < 0)
    {
        this->trace.fatal("Unable to open UMA memfd %s: %s\n", this->memfd.c_str(),
            strerror(errno));
        return;
    }

    struct stat st;
    if (fstat(fd, &st) == 0 && (uint64_t)st.st_size < offset + this->size)
    {
        close(fd);
        this->trace.fatal("UMA memfd %s too small (size: 0x%lx, needed: 0x%lx): "
            "the host RAM must cover [0x%lx, 0x%lx)\n",
            this->memfd.c_str(), (unsigned long)st.st_size,
            (unsigned long)(offset + this->size), (unsigned long)this->window_start,
            (unsigned long)(this->window_start + this->size));
        return;
    }

    void *ptr = mmap(NULL, this->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)offset);
    int mmap_errno = errno;
    // The mapping keeps its own reference to the memfd
    close(fd);
    if (ptr == MAP_FAILED)
    {
        this->trace.fatal("Unable to mmap UMA memfd %s (offset: 0x%lx, size: 0x%lx): %s\n",
            this->memfd.c_str(), (unsigned long)offset, (unsigned long)this->size,
            strerror(mmap_errno));
        return;
    }

    this->map = (uint8_t *)ptr;
    this->trace.msg(vp::Trace::LEVEL_INFO,
        "Mapped UMA memfd %s offset 0x%lx size 0x%lx (host 0x%lx -> memory offset 0)\n",
        this->memfd.c_str(), (unsigned long)offset, (unsigned long)this->size,
        (unsigned long)this->window_start);
}

void UmaMemfd::start()
{
    // Before any reset or access: the memory uses the mapping as its array
    if (this->map != NULL)
    {
        this->meminfo_itf.sync(this->map);
    }
}

void UmaMemfd::stop()
{
    if (this->map != NULL)
    {
        munmap(this->map, this->size);
        this->map = NULL;
    }
}

extern "C" vp::Component *gv_new(vp::ComponentConf &config)
{
    return new UmaMemfd(config);
}
