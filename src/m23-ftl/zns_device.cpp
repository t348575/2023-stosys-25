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

#include <cerrno>
#include <cstdint>
#include <functional>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../common/unused.h"
#include "zns_device.h"

extern "C" {

FtlTable log_table;
FtlTable data_table;

void update_lba(uint64_t &write_lba, const uint32_t lba_size, const int count) {
	UNUSED(lba_size);
	write_lba += count;
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

uint64_t get_mdts_size(int fd, ...) {
	nvme_id_ctrl identify_ctrl;
	int ret = nvme_identify_ctrl(fd, &identify_ctrl);
	if (ret != 0) {
		std::cerr << "ERROR: Could not get nvme idenitfy control: "
				  << std::to_string(ret) << std::endl;
	}

	void *registers = mmap(NULL, getpagesize(), PROT_READ, MAP_SHARED, fd, 0);
	;
	__u64 cap = le64_to_cpu((*(__le64 *)registers));
	munmap(registers, getpagesize());

	__u32 mpsmin = ((__u8 *)&cap)[6] & 0x0F;
	__u32 cap_mpsmin = 1 << (12 + mpsmin);
	return (1 << (identify_ctrl.mdts - 1)) * cap_mpsmin;
}

int ss_zns_device_zone_append(int fd, uint32_t nsid, uint64_t zslba,
							  int numbers, void *buffer, uint32_t buf_size,
							  uint64_t *written_slba) {
	return nvme_zns_append(fd, nsid, zslba, numbers - 1, 0, 0, 0, 0, buf_size,
						   buffer, 0, nullptr, (__u64 *)written_slba);
}

int deinit_ss_zns_device(struct user_zns_device *my_dev) {
	int ret = -ENOSYS;
	// this is to supress gcc warnings, remove it when you complete this
	// function
	UNUSED(my_dev);

	return ret;
}

int init_ss_zns_device(struct zdev_init_params *params,
					   struct user_zns_device **my_dev) {
	int fd = nvme_open(params->name);
	if (fd == -1) {
		return -1;
	}

	uint nsid;
	int ret = nvme_get_nsid(fd, &nsid);
	if (ret != 0) {
		return ret;
	}

	struct nvme_zone_report zones;
	ret = nvme_zns_report_zones(fd, nsid, 0, false, NVME_ZNS_ZRAS_REPORT_ALL,
								false, sizeof(zones), &zones);
	if (ret != 0) {
		return ret;
	}

	struct nvme_id_ns ns;
	ret = nvme_identify_ns(fd, nsid, &ns);
	if (ret != 0) {
		return ret;
	}
	uint32_t block_size = 1 << ns.lbaf[(ns.flbas & 0xf)].ds;

	struct nvme_zns_id_ns zns_ns;
	ret = nvme_zns_identify_ns(fd, nsid, &zns_ns);
	if (ret != 0) {
		return ret;
	}

	if (params->force_reset) {
		log_table.reset();
		data_table.reset();

		log_table.reserve(params->log_zones * zones.nr_zones);
	}

	*my_dev = (user_zns_device *)calloc(sizeof(user_zns_device), 1);
	(**my_dev).lba_size_bytes = block_size;
	(**my_dev).capacity_bytes = zones.nr_zones * zns_ns.lbafe->zsze;
	(**my_dev).tparams = {
			.zns_lba_size = block_size,
			.zns_zone_capacity = (uint32_t)zns_ns.lbafe->zsze,
			.zns_num_zones = (uint32_t)zones.nr_zones,
	};
	(**my_dev)._private = (device_data *)calloc(sizeof(device_data), 1);

	struct device_data *dd = ((device_data *)(**my_dev)._private);
	dd->fd = fd;
	dd->nsid = nsid;
	dd->log_info = new std::vector<ftl_info>();
	log_table.set_zone_info((uint32_t)zns_ns.lbafe->zsze);
	for (uint64_t i = 0; i < (uint64_t)params->log_zones; i++) {
		dd->log_info->push_back({.l_address = i,
								 .p_address = i * (uint32_t)zns_ns.lbafe->zsze,
								 .flag = status_flag::Valid});
	}

	return 0;
}

int zns_udevice_read(struct user_zns_device *my_dev, uint64_t address,
					 void *buffer, uint32_t size) {
    struct device_data *dd = ((device_data *)my_dev->_private);
    auto it = is_address_in_log(address, my_dev);
    if (it != dd->log_info->end()) {
        for (uint32_t i = 0; i < size; i++) {
                (char*)buffer[address + i] = log_table.
            }
        }
    }
	return 0;
}

uint64_t base_l_address(const uint64_t &address, const uint64_t &zone_size) {
	uint64_t iter = zone_size;
	while (!(address >= (iter - zone_size) && address < iter)) {
		iter += zone_size;
	}
	return iter - zone_size;
}

std::vector<ftl_info>::iterator
is_address_in_log(const uint64_t &address,
				  const struct user_zns_device *my_dev) {
	struct device_data *dd = ((device_data *)my_dev->_private);
	uint64_t l_address =
			base_l_address(address, my_dev->tparams.zns_zone_capacity);
	return std::find_if(dd->log_info->begin(), dd->log_info->end(),
						[l_address](struct ftl_info item) {
							return item.l_address == l_address;
						});
}

int zns_udevice_write(struct user_zns_device *my_dev, uint64_t address,
					  void *buffer, uint32_t size) {
	struct device_data *dd = ((device_data *)my_dev->_private);
	auto it = is_address_in_log(address, my_dev);
	if (it == dd->log_info->end()) {
		std::cerr << "ERROR: log zone not found" << std::endl;
		// TODO: handle creating a new log zone
	}

	if (log_table.has(address)) {

	} else {
		// TODO: if size if zone, directly make a data block
		uint32_t size_c = size;
		while (size_c > 0) {
			uint64_t slba;
			int ret = ss_zns_device_zone_append(
					dd->fd, dd->nsid, it->l_address, 1,
					(char *)buffer + (size - size_c), size_c, &slba);
			if (ret != 0) {
				return ret;
			}

			bool is_log_full =
					log_table.push_back({.l_address = address,
										 .p_address = slba,
										 .flag = status_flag::Valid});
			if (is_log_full) {
				log_table.reset_zone_info();
				ret = convert_log_to_data();
				if (ret != 0) {
					return ret;
				}
			}

			size_c -= std::min(my_dev->lba_size_bytes, size_c);
		}
	}

	return 0;
}

int convert_log_to_data() { return 0; }
}
