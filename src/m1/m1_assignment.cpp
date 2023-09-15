/*
 * MIT License
Copyright (c) 2021 - current
Authors:  Animesh Trivedi
This code is part of the Storage System Course at VU Amsterdam
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../common/unused.h"
#include "device.h"
#include "m1_assignment.h"

extern "C" {

int ss_nvme_device_io_with_mdts(int fd, uint32_t nsid, uint64_t slba,
                                uint16_t numbers, void *buffer,
                                uint64_t buf_size, uint64_t lba_size,
                                uint64_t mdts_size, bool read) {
    UNUSED(numbers);
    int ret = 0;
    uint64_t size = buf_size;
    std::function<int(int, uint32_t, uint64_t, uint16_t, void *, uint64_t)> op;
    if (read) {
        op = ss_nvme_device_read;
    } else {
        op = ss_nvme_device_write;
    }

    while (size > 0) {
        uint64_t op_size = std::min(mdts_size, size);
        numbers = op_size / lba_size;
        ret = op(fd, nsid, slba, numbers, (char *)buffer + (buf_size - size),
                 op_size);
        if (ret != 0) {
            return ret;
        }
        update_lba(slba, lba_size, numbers);
        size -= op_size;
    }
    return ret;
}

int ss_nvme_device_read(int fd, uint32_t nsid, uint64_t slba, uint16_t numbers,
                        void *buffer, uint64_t buf_size) {
    return nvme_read(fd, nsid, slba, numbers - 1, 0, NVME_IO_DSM_FREQ_UNSPEC, 0,
                     0, 0, buf_size, buffer, 0, nullptr);
}

int ss_nvme_device_write(int fd, uint32_t nsid, uint64_t slba, uint16_t numbers,
                         void *buffer, uint64_t buf_size) {
    return nvme_write(fd, nsid, slba, numbers - 1, 0, NVME_IO_DSM_FREQ_UNSPEC,
                      0, 0, 0, 0, buf_size, buffer, 0, nullptr);
}

int ss_zns_device_zone_reset(int fd, uint32_t nsid, uint64_t slba) {
    return nvme_zns_mgmt_send(fd, nsid, slba, false, NVME_ZNS_ZSA_RESET, 0,
                              nullptr);
}

int ss_zns_device_zone_append(int fd, uint32_t nsid, uint64_t zslba,
                              int numbers, void *buffer, uint32_t buf_size,
                              uint64_t *written_slba) {
    return nvme_zns_append(fd, nsid, zslba, numbers - 1, 0, 0, 0, 0, buf_size,
                           buffer, 0, nullptr, (__u64 *)written_slba);
}

void update_lba(uint64_t &write_lba, const uint32_t lba_size, const int count) {
    UNUSED(lba_size);
    write_lba += count;
}

uint64_t get_mdts_size(int fd, ...) {
    nvme_id_ctrl identify_ctrl;
    int ret = nvme_identify_ctrl(fd, &identify_ctrl);
    if (ret != 0) {
        std::cerr << "ERROR: Could not get nvme idenitfy control: "
                  << std::to_string(ret) << std::endl;
    }

    void *registers = mmap(NULL, getpagesize(), PROT_READ, MAP_SHARED, fd, 0);;
    __u64 cap = le64_to_cpu((*(__le64 *)registers));
    munmap(registers, getpagesize());

    __u32 mpsmin = ((__u8 *)&cap)[6] & 0x0F;
    __u32 cap_mpsmin = 1 << (12 + mpsmin);
    return (1 << (identify_ctrl.mdts - 1)) * cap_mpsmin;
}
}