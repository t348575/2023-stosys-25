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
#include <cstring>
#include <functional>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../common/unused.h"
#include "zns_device.h"

extern "C" {

FtlTable<std::vector<ftl_record>, ftl_record> log_table;
FtlTable<ftl_record, int> data_table;
FtlTable<ftl_record, int> log_info;
FtlTable<ftl_record, int> data_info;

inline int ss_nvme_device_read(int fd, uint32_t nsid, uint64_t slba, uint16_t numbers, void *buffer,
							   uint64_t buf_size) {
	return nvme_read(fd, nsid, slba, numbers - 1, 0, NVME_IO_DSM_FREQ_UNSPEC, 0, 0, 0, buf_size, buffer, 0, nullptr);
}

inline int ss_nvme_device_write(int fd, uint32_t nsid, uint64_t slba, uint16_t numbers, void *buffer,
								uint64_t buf_size) {
	return nvme_write(fd, nsid, slba, numbers - 1, 0, NVME_IO_DSM_FREQ_UNSPEC, 0, 0, 0, 0, buf_size, buffer, 0,
					  nullptr);
}

inline int ss_zns_device_zone_reset(int fd, uint32_t nsid, uint64_t slba) {
	return nvme_zns_mgmt_send(fd, nsid, slba, false, NVME_ZNS_ZSA_RESET, 0, nullptr);
}

inline int ss_nvme_device_io_with_mdts(int fd, uint32_t nsid, uint64_t slba, void *buffer, uint64_t buf_size,
									   uint64_t lba_size, uint64_t mdts_size, bool read) {
	uint16_t numbers;
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
		ret = op(fd, nsid, slba, numbers, (char *)buffer + (buf_size - size), op_size);
		if (ret != 0) {
			return ret;
		}
		slba += numbers;
		size -= op_size;
	}
	return ret;
}

uint64_t get_mdts_size(int fd) {
	nvme_id_ctrl identify_ctrl;
	int ret = nvme_identify_ctrl(fd, &identify_ctrl);
	if (ret != 0) {
		std::cerr << "ERROR: Could not get nvme idenitfy control: " << std::to_string(ret) << std::endl;
	}

	void *registers = mmap(NULL, getpagesize(), PROT_READ, MAP_SHARED, fd, 0);
	__u64 cap = le64_to_cpu((*(__le64 *)registers));
	munmap(registers, getpagesize());

	__u32 mpsmin = ((__u8 *)&cap)[6] & 0x0F;
	__u32 cap_mpsmin = 1 << (12 + mpsmin);
	return (1 << (identify_ctrl.mdts - 1)) * cap_mpsmin;
}

int deinit_ss_zns_device(struct user_zns_device *my_dev) {
	free(my_dev->_private);
	free(my_dev);
	return 0;
}

inline int report_zones(int fd, __u32 nsid, __u64 slba, enum nvme_zns_report_options opts, bool partial, __u32 data_len,
						void *data) {
	return nvme_zns_mgmt_recv(fd, nsid, slba, NVME_ZNS_ZRA_REPORT_ZONES, opts, partial, data_len, data);
}

int init_ss_zns_device(struct zdev_init_params *params, struct user_zns_device **my_dev) {
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
	ret = report_zones(fd, nsid, 0, NVME_ZNS_ZRAS_REPORT_ALL, false, sizeof(zones), &zones);
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
	}

	*my_dev = (user_zns_device *)calloc(sizeof(user_zns_device), 1);
	(**my_dev).lba_size_bytes = block_size;
	(**my_dev).capacity_bytes = (zones.nr_zones - params->log_zones) * zns_ns.lbafe->zsze * 4096;
	(**my_dev).tparams = {
			.zns_lba_size = block_size,
			.zns_zone_capacity = (uint32_t)zns_ns.lbafe->zsze * 4096,
			.zns_num_zones = (uint32_t)zones.nr_zones,
	};
	(**my_dev)._private = (device_data *)calloc(sizeof(device_data), 1);

	struct device_data *dd = ((device_data *)(**my_dev)._private);
	dd->fd = fd;
	dd->nsid = nsid;
	dd->mdts = 262144;
	log_table.set_zone_info((uint32_t)zns_ns.lbafe->zsze);
	for (uint64_t i = 0; i < (uint64_t)params->log_zones; i++) {
		log_info.push_back({.l_address = i,
							.p_address = i * (uint32_t)zns_ns.lbafe->zsze,
							.write_ptr = 0,
							.flag = status_flag::Invalid});
		auto vec = std::vector<ftl_record>();
		vec.reserve(zns_ns.lbafe->zsze);
		log_table.push_back(vec);
	}

	uint64_t n_data_zones = zones.nr_zones - params->log_zones;
	for (uint64_t i = 0; i < n_data_zones; i++) {
		data_info.push_back({.l_address = i,
							 .p_address = (i + params->log_zones) * (uint32_t)zns_ns.lbafe->zsze,
							 .write_ptr = 0,
							 .flag = status_flag::Invalid});
		data_table.push_back({.l_address = 0, .p_address = 0, .write_ptr = 0, .flag = status_flag::Invalid});
	}

	return 0;
}

inline uint64_t closest_upper_multiple(uint64_t n, uint64_t x) {
	if (x > n) {
		return x;
	}

	n = n + x / 2;
	n = n - (n % x);
	return n;
}

inline uint64_t closest_lower_multiple(uint64_t n, uint64_t x) { return x - (x % n); }

int zns_udevice_read(struct user_zns_device *my_dev, uint64_t address, void *buffer, uint32_t size) {
	struct device_data *dd = ((device_data *)my_dev->_private);

	uint32_t size_c = size;
	int ret = -1;
	while (size_c > 0) {
		ftl_record record;
		auto base_l_address = closest_lower_multiple(my_dev->tparams.zns_zone_capacity, address);
		int index = log_info.does_address_exist(base_l_address, &record);
		uint32_t op_size = std::min(my_dev->tparams.zns_zone_capacity, size_c);
		uint32_t offset = 0;
		if (index != -1) {
			ret = log_table.get_record(index, address, record, offset);
			if (ret == -1) {
				return ret;
			}
		} else if (data_table.does_address_exist(base_l_address, &record) != -1) {
			offset = (address - base_l_address) / 4096;
		}

		ret = ss_nvme_device_io_with_mdts(dd->fd, dd->nsid, record.p_address + offset, (char *)buffer + size - size_c,
										  op_size, my_dev->lba_size_bytes, dd->mdts, true);
		if (ret != 0) {
			return ret;
		}
		address += op_size;
		size_c -= op_size;
	}
	return ret;
}

int zns_udevice_write(struct user_zns_device *my_dev, uint64_t address, void *buffer, uint32_t size) {
	struct device_data *dd = ((device_data *)my_dev->_private);
	uint64_t base_address = closest_lower_multiple(my_dev->tparams.zns_zone_capacity, address);

	// if exists in data block
	int data_table_idx = data_table.does_address_exist(base_address, nullptr);
	if (data_table_idx != -1) {
		ftl_record data_record = data_table.get(data_table_idx);
		data_table.set_flag(data_table_idx, status_flag::Invalid);

		uint32_t zone_size = log_table.get_zone_size();
		log_table.record_transaction([=](std::vector<std::vector<ftl_record>> &table) {
			table[data_table_idx].resize(zone_size);
			for (uint64_t i = 0; i < zone_size; i++) {
				table[data_table_idx][i] = {
						.l_address = base_address + (i * my_dev->lba_size_bytes),
						.p_address = data_record.p_address + i,
						.write_ptr = 0,
						.flag = status_flag::Valid,
				};
			}
		});
		log_info.set(data_table_idx, data_record);
		return zns_udevice_write(my_dev, address, buffer, size);
	}

	// if log block does not exist for this address
	int block_idx = log_info.does_address_exist(base_address, nullptr);
	int p_address = 0;
	int write_ptr = 0;
	if (block_idx == -1) {
		block_idx = log_info.get_unused_block();
		if (block_idx == -1) {
			std::cerr << "ERROR: log zone not found" << std::endl;
			return -1;
		}

		auto zone_info = log_info.get(block_idx);
		zone_info.l_address = closest_lower_multiple(my_dev->tparams.zns_zone_capacity, address);
		log_info.set(block_idx, zone_info);
		p_address = zone_info.p_address;
		write_ptr = zone_info.write_ptr;
		int ret = ss_zns_device_zone_reset(dd->fd, dd->nsid, zone_info.p_address);
		if (ret == -1) {
			return ret;
		}
		// TODO: handle creating a new log zone
	}

	auto zone_info = log_info.get(block_idx);
	p_address = zone_info.p_address;
	write_ptr = zone_info.write_ptr;

	// if already exists in log table
	char *buffer_to_use = (char *)buffer;
	char *device_buffer = nullptr;
	bool free_device_buffer = false;
	if (log_table.has(address)) {
		free_device_buffer = true;
		uint64_t log_table_size = log_table.size_at(block_idx) * 4096;
		uint64_t device_buf_size = std::max(log_table_size, (uint64_t)my_dev->tparams.zns_zone_capacity);
		device_buffer = (char *)malloc(device_buf_size);
		int ret = ss_nvme_device_io_with_mdts(dd->fd, dd->nsid, p_address, device_buffer, log_table_size,
											  my_dev->lba_size_bytes, dd->mdts, true);
		if (ret != 0) {
			free(device_buffer);
			return ret;
		}

		ret = ss_zns_device_zone_reset(dd->fd, dd->nsid, p_address);
		if (ret != 0) {
			free(device_buffer);
			return ret;
		}
		log_info.reset_write_ptr(block_idx);
		log_table.clear_index(block_idx);
		write_ptr = 0;

		memcpy((char *)device_buffer + (address - base_address), buffer, device_buf_size);
		buffer_to_use = device_buffer;
	}

	// write to log table
	uint32_t size_c = size;
	while (size_c > 0) {
		uint64_t op_size = std::min(closest_upper_multiple(my_dev->lba_size_bytes, size),
									(uint64_t)my_dev->tparams.zns_zone_capacity);

		// uint insertion_pos = log_table.insertion_pos(block_idx, address);
		// if (insertion_pos != log_table.size_at(block_idx)) {
		// TODO: move current zone and rewrite
		// }

		int ret = ss_nvme_device_io_with_mdts(dd->fd, dd->nsid, p_address + write_ptr,
											  (char *)buffer_to_use + (size - size_c), op_size, my_dev->lba_size_bytes,
											  dd->mdts, false);
		if (ret != 0) {
			if (device_buffer != nullptr && free_device_buffer) {
				free(device_buffer);
			}
			return ret;
		}

		uint64_t num_operations = op_size / my_dev->lba_size_bytes;
		// if log block is full
		if (log_table.get_zone_size() == log_table.table_index_size(block_idx) + num_operations) {
			ret = convert_log_to_data(log_info.get(block_idx), block_idx, p_address);
			if (ret != 0) {
				if (device_buffer != nullptr && free_device_buffer) {
					free(device_buffer);
				}
				return ret;
			}

			if (size_c - op_size != 0) {
				if (free_device_buffer) {
					free(device_buffer);
				}
				return zns_udevice_write(my_dev, my_dev->tparams.zns_zone_capacity,
										 (char *)buffer + size - (size_c - op_size), size_c - op_size);
			}
			address += my_dev->tparams.zns_zone_capacity;
			write_ptr = 0;
		} else {
			write_ptr = log_info.increment_write_ptr(block_idx, num_operations);
			for (uint64_t i = 0; i < num_operations; i++) {
				log_table.push_back(block_idx,
									{.l_address = address + (i * my_dev->lba_size_bytes),
									 .p_address = p_address + (size - size_c) + i,
									 .write_ptr = 0,
									 .flag = status_flag::Valid});
			}
		}

		size_c -= op_size;
	}

	if (device_buffer != nullptr && free_device_buffer) {
		free(device_buffer);
	}

	return 0;
}

int convert_log_to_data(const ftl_record record, const int log_idx, int &p_address) {
	int data_table_idx = data_table.get_unused_block();
	if (data_table_idx == -1) {
		return -1;
	}

	data_table.set(
			data_table_idx,
			{.l_address = record.l_address, .p_address = record.p_address, .write_ptr = 0, .flag = status_flag::Valid});
	log_info.set_flag(log_idx, status_flag::Invalid);

	int data_info_idx = data_info.get_unused_block();
	ftl_record data_record = data_info.get(data_info_idx);
	data_info.set_flag(data_info_idx, status_flag::Valid);

	data_record.flag = status_flag::Invalid;
	data_record.write_ptr = 0;
	log_info.set(log_idx, data_record);

	p_address = data_record.p_address;

	log_table.clear_index(log_idx);

	return 0;
}
}
