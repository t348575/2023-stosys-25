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

void update_lba(uint64_t &write_lba, const uint32_t lba_size, const int count) {
	UNUSED(lba_size);
	write_lba += count;
}

int ss_nvme_device_read(int fd, uint32_t nsid, uint64_t slba, uint16_t numbers, void *buffer, uint64_t buf_size) {
	return nvme_read(fd, nsid, slba, numbers - 1, 0, NVME_IO_DSM_FREQ_UNSPEC, 0, 0, 0, buf_size, buffer, 0, nullptr);
}

int ss_nvme_device_write(int fd, uint32_t nsid, uint64_t slba, uint16_t numbers, void *buffer, uint64_t buf_size) {
	return nvme_write(fd, nsid, slba, numbers - 1, 0, NVME_IO_DSM_FREQ_UNSPEC, 0, 0, 0, 0, buf_size, buffer, 0,
					  nullptr);
}

int ss_zns_device_zone_reset(int fd, uint32_t nsid, uint64_t slba) {
	return nvme_zns_mgmt_send(fd, nsid, slba, false, NVME_ZNS_ZSA_RESET, 0, nullptr);
}

int ss_nvme_device_io_with_mdts(int fd, uint32_t nsid, uint64_t slba, void *buffer, uint64_t buf_size,
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
		update_lba(slba, lba_size, numbers);
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

int ss_zns_device_zone_append(int fd, uint32_t nsid, uint64_t zslba, int numbers, void *buffer, uint32_t buf_size,
							  uint64_t *written_slba) {
	return nvme_zns_append(fd, nsid, zslba, numbers - 1, 0, 0, 0, 0, buf_size, buffer, 0, nullptr,
						   (__u64 *)written_slba);
}

int deinit_ss_zns_device(struct user_zns_device *my_dev) {
	free(my_dev->_private);
	free(my_dev);
	return 0;
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
	ret = nvme_zns_report_zones(fd, nsid, 0, false, NVME_ZNS_ZRAS_REPORT_ALL, false, sizeof(zones), &zones);
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
		log_table.push_back(std::vector<ftl_record>());
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

int zns_udevice_read(struct user_zns_device *my_dev, uint64_t address, void *buffer, uint32_t size) {
	struct device_data *dd = ((device_data *)my_dev->_private);

	uint32_t size_c = size;
	int ret = -1;
	while (size_c > 0) {
		int index = log_info.does_base_address_exist(address, my_dev);
		uint32_t op_size = std::min(my_dev->tparams.zns_zone_capacity, size_c);
		if (index != -1) {
			ftl_record record;
			uint result_index;
			ret = log_table.get_record(index, address, record, result_index);
			if (ret == -1) {
				return ret;
			}

			// std::cout << "reading from log_table" << std::endl;
			ret = ss_nvme_device_io_with_mdts(dd->fd, dd->nsid, record.p_address + result_index,
											  (char *)buffer + size - size_c, op_size, my_dev->lba_size_bytes, dd->mdts,
											  true);
			if (ret != 0) {
				return ret;
			}
			address += op_size;
			size_c -= op_size;
			continue;
		}

		index = data_table.does_base_address_exist(address, my_dev);
		if (index != -1) {
			auto record = data_table.get(index);
			auto offset = (address - base_l_address(address, my_dev->tparams.zns_zone_capacity)) / 4096;
			// std::cout << "reading from data_table " << offset << std::endl;
			ret = ss_nvme_device_io_with_mdts(dd->fd, dd->nsid, record.p_address + offset,
											  (char *)buffer + size - size_c, op_size, my_dev->lba_size_bytes, dd->mdts,
											  true);
			if (ret != 0) {
				return ret;
			}
			address += op_size;
			size_c -= op_size;
		}
	}
	return ret;
}

uint64_t base_l_address(const uint64_t &address, const uint64_t &zone_size) {
	uint64_t iter = zone_size;
	while (!(address >= (iter - zone_size) && address < iter)) {
		iter += zone_size;
	}
	return iter - zone_size;
}

int zns_udevice_write(struct user_zns_device *my_dev, uint64_t address, void *buffer, uint32_t size) {
	struct device_data *dd = ((device_data *)my_dev->_private);

	uint64_t base_address = base_l_address(address, my_dev->tparams.zns_zone_capacity);
	int data_table_idx = data_table.does_base_address_exist(base_address, my_dev);
	if (data_table_idx != -1) {
		ftl_record data_record = data_table.get(data_table_idx);
		data_table.set_invalid(data_table_idx);

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

	int block_idx = log_info.does_base_address_exist(address, my_dev);
	int p_address = 0;
	int write_ptr = 0;
	if (block_idx == -1) {
		block_idx = log_info.get_unused_block();
		if (block_idx == -1) {
			std::cerr << "ERROR: log zone not found" << std::endl;
			return -1;
		}

		auto zone_info = log_info.get(block_idx);
		zone_info.l_address = base_l_address(address, my_dev->tparams.zns_zone_capacity);
		log_info.set(block_idx, zone_info);
		p_address = zone_info.p_address;
		write_ptr = zone_info.write_ptr;
		// std::cout << "reseting " << zone_info.p_address << std::endl;
		int ret = ss_zns_device_zone_reset(dd->fd, dd->nsid, zone_info.p_address);
		if (ret == -1) {
			return ret;
		}
		// TODO: handle creating a new log zone
	}

	auto zone_info = log_info.get(block_idx);
	p_address = zone_info.p_address;
	write_ptr = zone_info.write_ptr;

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

	// TODO: if size if zone, directly make a data block
	uint32_t size_c = size;
	while (size_c > 0) {
		uint64_t op_size = std::min(my_dev->lba_size_bytes, size_c);
		// std::cout << "writing " << std::to_string(address) << " to " << p_address + write_ptr << std::endl;

		uint insertion_pos = log_table.insertion_pos(block_idx, address);
		if (insertion_pos != log_table.size_at(block_idx)) {
			// TODO: move current zone and rewrite
		}

		int ret = ss_nvme_device_io_with_mdts(dd->fd, dd->nsid, p_address + write_ptr,
											  (char *)buffer_to_use + (size - size_c), op_size, my_dev->lba_size_bytes,
											  dd->mdts, false);
		if (ret != 0) {
			if (device_buffer != nullptr && free_device_buffer) {
				free(device_buffer);
			}
			return ret;
		}

		write_ptr = log_info.update_write_ptr(block_idx);
		log_table.push_back(block_idx,
							{.l_address = address,
							 .p_address = p_address + size - size_c,
							 .write_ptr = 0,
							 .flag = status_flag::Valid});
		if (log_table.get_zone_size() == log_table.table_index_size(block_idx)) {
			ret = convert_log_to_data(log_info.get(block_idx), block_idx, p_address);
			if (ret != 0) {
				if (device_buffer != nullptr && free_device_buffer) {
					free(device_buffer);
				}
				return ret;
			}

			write_ptr = 0;
			if (size_c - op_size != 0) {
				if (free_device_buffer) {
					free(device_buffer);
				}
				return zns_udevice_write(my_dev, my_dev->tparams.zns_zone_capacity,
										 (char *)buffer + size - (size_c - op_size), size_c - op_size);
			}
			address += my_dev->tparams.zns_zone_capacity;
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

	// std::cout << "converting l_address: " << std::to_string(record.l_address) << " p_address "
	// 		  << std::to_string(record.p_address) << " to data_table" << std::endl;
	data_table.set(
			data_table_idx,
			{.l_address = record.l_address, .p_address = record.p_address, .write_ptr = 0, .flag = status_flag::Valid});
	log_info.set_invalid(log_idx);

	int data_info_idx = data_info.get_unused_block();
	ftl_record data_record = data_info.get(data_info_idx);
	data_info.remove_index(data_info_idx);

	data_record.flag = status_flag::Invalid;
	data_record.write_ptr = 0;
	log_info.set(log_idx, data_record);

	p_address = data_record.p_address;

	log_table.clear_index(log_idx);

	return 0;
}
}
