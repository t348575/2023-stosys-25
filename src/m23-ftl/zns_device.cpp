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
#include <fcntl.h>
#include <iostream>
#include <set>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "../common/VisualProfiler.hpp"
#include "../common/unused.h"
#include "../common/utils.h"
#include "ftl.h"
#include "zns_device.h"

#define PROFILE_NVME_FUNCTION() PROFILE_FUNCTION_CATEGORY("nvme")

inline int ss_nvme_device_read(int fd, uint32_t nsid, uint64_t slba, uint16_t numbers, void *buffer,
							   uint64_t buf_size) {
	return nvme_read(fd, nsid, slba, numbers - 1, 0, NVME_IO_DSM_FREQ_UNSPEC, 0, 0, 0, buf_size, buffer, 0, nullptr);
}

inline int ss_nvme_device_write(int fd, uint32_t nsid, uint64_t slba, uint16_t numbers, void *buffer,
								uint64_t buf_size) {
	return nvme_write(fd, nsid, slba, numbers - 1, 0, NVME_IO_DSM_FREQ_UNSPEC, 0, 0, 0, 0, buf_size, buffer, 0,
					  nullptr);
}

inline int ss_zns_device_zone_reset(uint64_t slba) {
	PROFILE_SCOPE("nvme", "ss_zns_device_zone_reset");
	auto const records = DataMap::get_instance();
	return nvme_zns_mgmt_send(records->dd->fd, records->dd->nsid, slba, false, NVME_ZNS_ZSA_RESET, 0, nullptr);
}

typedef int (*nvme_op)(int, uint32_t, uint64_t, uint16_t, void *, uint64_t);
int ss_nvme_device_io_with_mdts(uint64_t slba, void *buffer, uint64_t buf_size, bool read) {
	std::string name = "mdts_" + std::string(read ? "read" : "write") + "_" + std::to_string(buf_size);
	PROFILE_SCOPE("nvme", name);
	uint16_t numbers;
	int ret = 0;
	uint64_t size = buf_size;
	nvme_op op;
	if (read) {
		op = ss_nvme_device_read;
	} else {
		op = ss_nvme_device_write;
	}

	auto const records = DataMap::get_instance();
	while (size > 0) {
		uint64_t op_size = std::min(DataMap::get_instance()->dd->mdts, size);
		numbers = op_size / records->device->lba_size_bytes;
		ret = op(records->dd->fd, records->dd->nsid, slba, numbers, (char *)buffer + (buf_size - size), op_size);
		if (ret != 0) {
			return ret;
		}
		slba += numbers;
		size -= op_size;
	}
	return ret;
}

char *nvme_char_from_block(const char *dev) {
	char *path = NULL;
	int ret, id;

	ret = sscanf(dev, "nvme%dn", &id);
	ret = asprintf(&path, "nvme%d", id);
	if (ret < 0)
		return NULL;
	return path;
}

void *mmap_registers(const char *dev) {
	int fd;
	char *base, path[512];
	void *membase;

	base = nvme_char_from_block((char *)dev);
	if (!base)
		return NULL;

	sprintf(path, "/sys/class/nvme/%s/device/resource0", base);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		sprintf(path, "/sys/class/misc/%s/device/resource0", base);
		fd = open(path, O_RDONLY);
	}
	if (fd < 0) {
		fprintf(stderr, "%s did not find a pci resource, open failed %s\n", base, strerror(errno));
		free(base);
		return NULL;
	}

	membase = mmap(NULL, getpagesize(), PROT_READ, MAP_SHARED, fd, 0);
	if (membase == MAP_FAILED) {
		fprintf(stderr, "%s failed to map. ", base);
		fprintf(stderr, "Did your kernel enable CONFIG_IO_STRICT_DEVMEM?\n");
		membase = NULL;
	}

	free(base);
	close(fd);
	return membase;
}

uint64_t get_mdts_size(const int fd, const char *name) {
	nvme_id_ctrl identify_ctrl;
	int ret = nvme_identify_ctrl(fd, &identify_ctrl);
	if (ret != 0) {
		std::cerr << "ERROR: Could not get nvme idenitfy control: " << std::to_string(ret) << std::endl;
	}

	void *registers = mmap_registers(name);
	__u64 cap = le64_to_cpu((*(__le64 *)registers));
	munmap(registers, getpagesize());

	__u32 mpsmin = ((__u8 *)&cap)[6] & 0x0F;
	__u32 cap_mpsmin = 1 << (12 + mpsmin);
	return (1 << (identify_ctrl.mdts - 1)) * cap_mpsmin;
}

int deinit_ss_zns_device(struct user_zns_device *my_dev) {
	UNUSED(my_dev);
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

	*my_dev = (user_zns_device *)calloc(sizeof(user_zns_device), 1);
	(**my_dev).lba_size_bytes = block_size;
	(**my_dev).capacity_bytes = (zones.nr_zones - params->log_zones - 1) * zns_ns.lbafe->zsze * block_size;
	(**my_dev).tparams = {
			.zns_lba_size = block_size,
			.zns_zone_capacity = (uint32_t)zns_ns.lbafe->zsze * block_size,
			.zns_num_zones = (uint32_t)zones.nr_zones,
	};
	(**my_dev)._private = (device_data *)calloc(sizeof(device_data), 1);

	struct device_data *dd = ((device_data *)(**my_dev)._private);
	dd->fd = fd;
	dd->nsid = nsid;
	dd->mdts = get_mdts_size(dd->fd, params->name);
	dd->zone_size = zns_ns.lbafe->zsze;
	dd->start_merge_threshold = (uint64_t)(((double)dd->zone_size) * (double)0.9);
	dd->gc_wmark = params->gc_wmark;
	dd->log_zones = params->log_zones;
	dd->n_zones = zones.nr_zones - params->log_zones;

	if (!DataMap::get_instance()->gc_running) {
		DataMap::get_instance()->init(*my_dev, dd);
		std::thread t1(gc_loop);
		t1.detach();
	}

	return 0;
}

void try_logical_conversion() {
	PROFILE_NVME_FUNCTION();
	auto const records = DataMap::get_instance();
	for (uint64_t j = 0; j < records->log_zones.size(); j++) {
		if (records->log_zones[j].logs.size() != records->dd->zone_size) {
			continue;
		}

		auto const start_record = records->log_zones[j].logs[0];
		auto prev_record = records->log_zones[j].logs[0];
		bool can_logically_make_data = true;
		for (uint64_t i = 1; i < records->log_zones[j].logs.size(); i++) {
			if (records->log_zones[j].logs[i].base_address != start_record.base_address ||
				prev_record.l_address + records->device->lba_size_bytes != records->log_zones[j].logs[i].l_address) {
				can_logically_make_data = false;
				break;
			}
			prev_record = records->log_zones[j].logs[i];
		}

		if (can_logically_make_data) {
			ftl_record data_zone;
			if (!records->remove_unused_data_zone(start_record.base_address, data_zone)) {
				std::cerr << "ERROR: could not find a data zone to convert" << std::endl;
				return;
			}

			records->log_zones.erase(records->log_zones.begin() + j);
			records->data_zones.push_back(DataRecord(start_record));
			records->log_zones.push_back(LogRecord(data_zone.p_address, records->dd->zone_size));
			int ret = ss_zns_device_zone_reset(data_zone.p_address);
			if (ret != 0) {
				std::cerr << "ERROR: could not reset log zone in logical conversion" << std::endl;
			}
		}
	}
}

void reclaim_empty_log_zones() {
	PROFILE_NVME_FUNCTION();
	auto const records = DataMap::get_instance();
	for (auto &t : records->log_zones) {
		if (t.zone_size == records->dd->zone_size && t.logs.size() == 0) {
			int ret = ss_zns_device_zone_reset(t.p_address);
			if (ret != 0) {
				std::cerr << "ERROR: could not reset log zone" << std::endl;
			}

			t.write_ptr = t.p_address;
			t.zone_size = 0;
		}
	}
}

uint64_t full_merge() {
	PROFILE_NVME_FUNCTION();
	auto const records = DataMap::get_instance();
	const auto count = records->count_base_addresses();
	if (count.size() == 0) {
		return 0;
	}

	std::vector<float> p_address_ratio_count;
	p_address_ratio_count.reserve(count.size());
	for (const auto &c : count) {
		std::set<uint64_t> distinct_p_addresses_test;
		for (auto &t : records->log_zones) {
			const auto removed = t.remove_base_address(c.base_address, true);
			for (const auto &t : removed) {
				distinct_p_addresses_test.insert(closest_lower_multiple(records->dd->zone_size, t.p_address));
			}
		}
		p_address_ratio_count.push_back((float)c.count / (float)distinct_p_addresses_test.size());
	}

	count_base_address_sums merge_record = count[0];
	for (uint64_t i = 1; i < count.size() - 1; i++) {
		if (p_address_ratio_count[i] > p_address_ratio_count[i - 1] && count[i - 1].count * 2 >= count[i].count) {
			merge_record = count[i];
		}
	}

	std::vector<ftl_record> base_address_items;
	std::set<uint64_t> distinct_p_addresses;
	base_address_items.reserve(records->dd->log_zones * records->dd->zone_size);
	for (auto &t : records->log_zones) {
		const auto removed = t.remove_base_address(merge_record.base_address, false);
		base_address_items.insert(base_address_items.end(), removed.begin(), removed.end());
	}

	for (const auto &t : base_address_items) {
		distinct_p_addresses.insert(closest_lower_multiple(records->dd->zone_size, t.p_address));
	}

	std::vector<char> log_zone, temp_buffer;
	log_zone.resize(records->device->tparams.zns_zone_capacity);
	temp_buffer.resize(records->device->tparams.zns_zone_capacity);
	for (const auto &p_address : distinct_p_addresses) {
		const auto base_p_address = closest_lower_multiple(records->dd->zone_size, p_address);
		int ret = ss_nvme_device_io_with_mdts(base_p_address, (void *)&temp_buffer[0],
											  records->device->tparams.zns_zone_capacity, true);
		if (ret != 0) {
			std::cerr << "ERROR: could not read log zone for merge" << std::endl;
		}

		for (const auto &item : base_address_items) {
			if (closest_lower_multiple(records->dd->zone_size, item.p_address) == p_address) {
				memcpy((void *)&log_zone[item.l_address - merge_record.base_address],
					   (void *)&temp_buffer[(item.p_address - base_p_address) * records->device->lba_size_bytes],
					   records->device->lba_size_bytes);
			}
		}
	}

	ftl_record data_zone_record;
	uint64_t data_zone_idx;
	if (!records->get_data_zone(merge_record.base_address, data_zone_record, data_zone_idx)) {
		std::cerr << "ERROR: could not find data zone for " << std::to_string(merge_record.base_address) << std::endl;
	}

	std::vector<char> data_zone;
	data_zone.resize(records->device->tparams.zns_zone_capacity);
	int ret = ss_nvme_device_io_with_mdts(data_zone_record.p_address, (void *)&data_zone[0],
										  records->device->tparams.zns_zone_capacity, true);
	if (ret != 0) {
		std::cerr << "ERROR: could not read data zone for merge" << std::endl;
	}

	records->set_data_zone(data_zone_idx,
						   {.l_address = 0,
							.p_address = data_zone_record.p_address,
							.base_address = merge_record.base_address,
							.flag = status_flag::Valid});

	for (const auto &item : base_address_items) {
		memcpy((void *)&data_zone[item.l_address - merge_record.base_address],
			   (void *)&log_zone[item.l_address - merge_record.base_address], records->device->lba_size_bytes);
	}

	ret = ss_zns_device_zone_reset(data_zone_record.p_address);
	if (ret != 0) {
		std::cerr << "ERROR: could not reset data zone for merge" << std::endl;
	}

	ret = ss_nvme_device_io_with_mdts(data_zone_record.p_address, (void *)&data_zone[0],
									  records->device->tparams.zns_zone_capacity, false);
	if (ret != 0) {
		std::cerr << "ERROR: could not write data zone for merge" << std::endl;
	}

	return merge_record.count;
}

uint64_t merge_log_zones() {
	PROFILE_NVME_FUNCTION();
	auto const records = DataMap::get_instance();
	uint64_t idx[2];
	uint64_t sum_size = 0;
	bool found = false;
	for (uint64_t i = 0; i < records->log_zones.size() - 1; i++) {
		for (uint64_t j = i + 1; j < records->log_zones.size(); j++) {
			if (records->log_zones[i].logs.size() > 0 && records->log_zones[j].logs.size() > 0 &&
				records->log_zones[i].logs.size() + records->log_zones[j].logs.size() <= records->dd->zone_size) {
				if (records->log_zones[i].logs.size() + records->log_zones[j].logs.size() > sum_size) {
					found = true;
					idx[0] = i;
					idx[1] = j;
					sum_size = records->log_zones[i].logs.size() + records->log_zones[j].logs.size();
				}
			}
		}
	}

	if (!found) {
		return 0;
	}

	if (records->log_zones[idx[1]].logs.size() < records->dd->zone_size / 2) {
		return 0;
	}

	std::vector<char> old_log_zone, new_log_zone;
	old_log_zone.resize(records->device->tparams.zns_zone_capacity);
	new_log_zone.reserve(records->device->tparams.zns_zone_capacity);
	auto base_p_address = records->log_zones[idx[0]].p_address;
	for (uint i = 0; i < 2; i++) {
		int ret = ss_nvme_device_io_with_mdts(records->log_zones[idx[i]].p_address, (void *)&old_log_zone[0],
											  records->device->tparams.zns_zone_capacity, true);
		if (ret != 0) {
			std::cerr << "ERROR: could not read log zone for merge" << std::endl;
		}

		for (const auto &t : records->log_zones[idx[i]].logs) {
			const auto from = ((t.p_address - records->log_zones[idx[i]].p_address) * records->device->lba_size_bytes);
			new_log_zone.insert(new_log_zone.end(), old_log_zone.begin() + from,
								old_log_zone.begin() + from + records->device->lba_size_bytes);
		}

		ret = ss_zns_device_zone_reset(records->log_zones[idx[i]].p_address);
		if (ret != 0) {
			std::cerr << "ERROR: could not reset log zone in logical conversion" << std::endl;
		}

		if (i == 1) {
			records->log_zones[idx[i]].write_ptr = records->log_zones[idx[i]].p_address;
			records->log_zones[idx[i]].zone_size = 0;
		}

		for (uint64_t j = 0; j < records->log_zones[idx[i]].logs.size(); j++) {
			records->log_zones[idx[i]].logs[j].p_address = base_p_address + j;
		}
		base_p_address += records->log_zones[idx[i]].logs.size();
	}

	int ret = ss_nvme_device_io_with_mdts(records->log_zones[idx[0]].p_address, (void *)&new_log_zone[0],
										  new_log_zone.size(), false);
	if (ret != 0) {
		std::cerr << "ERROR: could not write while merging two log zones" << std::endl;
	}

	records->log_zones[idx[0]].zone_size = new_log_zone.size() / records->device->lba_size_bytes;
	records->log_zones[idx[0]].write_ptr = records->log_zones[idx[0]].p_address + records->log_zones[idx[0]].zone_size;

	records->log_zones[idx[0]].logs.insert(records->log_zones[idx[0]].logs.end(),
										   records->log_zones[idx[1]].logs.begin(),
										   records->log_zones[idx[1]].logs.end());
	uint64_t saved_size = records->log_zones[idx[1]].logs.size();
	records->log_zones[idx[1]].logs.clear();
	return saved_size;
}

void reorder_write_seq_log_zone() {
#ifdef PREFER_SEQ_LOG
	PROFILE_NVME_FUNCTION();
	auto const records = DataMap::get_instance();
	std::vector<uint8_t> data_buffer, log_buffer;
	for (uint64_t i = 0; i < records->log_zones.size(); i++) {
		if (!records->log_zones[i].is_seq) {
			continue;
		}

		if (records->log_zones[i].logs.size() < records->dd->zone_size * 0.75) {
			continue;
		}

		data_buffer.resize(records->device->tparams.zns_zone_capacity);
		log_buffer.resize(records->device->tparams.zns_zone_capacity);

		int ret = ss_nvme_device_io_with_mdts(records->log_zones[i].p_address, &log_buffer[0],
											  records->device->tparams.zns_zone_capacity, true);
		if (ret != 0) {
			return;
		}

		ftl_record data_zone_record;
		uint64_t data_zone_idx;
		if (!records->get_data_zone(records->log_zones[i].base_address, data_zone_record, data_zone_idx)) {
			std::cerr << "ERROR: could not find data zone for " << std::to_string(records->log_zones[i].base_address)
					  << std::endl;
		}

		ret = ss_nvme_device_io_with_mdts(data_zone_record.p_address, &data_buffer[0],
										  records->device->tparams.zns_zone_capacity, true);
		if (ret != 0) {
			return;
		}

		const auto base_p_address = closest_lower_multiple(records->dd->zone_size, records->log_zones[i].p_address);
		for (uint64_t j = 0; j < records->log_zones[i].logs.size(); j++) {
			memcpy(&data_buffer[records->log_zones[i].logs[j].l_address - records->log_zones[i].base_address],
				   &log_buffer[(records->log_zones[i].logs[j].p_address - base_p_address) *
							   records->device->lba_size_bytes],
				   records->device->lba_size_bytes);
		}

		ret = ss_zns_device_zone_reset(data_zone_record.p_address);
		if (ret != 0) {
			return;
		}

		ret = ss_nvme_device_io_with_mdts(data_zone_record.p_address, &data_buffer[0],
										  records->device->tparams.zns_zone_capacity, false);
		if (ret != 0) {
			return;
		}

		records->log_zones[i].is_seq = false;
		records->log_zones[i].logs.clear();

		uint64_t num_free_log_pages = records->count_free_log_pages();
		if (num_free_log_pages / records->dd->zone_size > records->dd->gc_wmark) {
			return;
		}
	}
#endif
}

void gc() {
	PROFILE_NVME_FUNCTION();
	auto const records = DataMap::get_instance();
	records->log_mtx.lock();
	records->data_mtx.lock();

	reorder_write_seq_log_zone();
	try_logical_conversion();
	reclaim_empty_log_zones();

	uint64_t count = 0;
	while (full_merge() > records->dd->zone_size / 4 && count < (records->dd->log_zones - 1) * records->dd->zone_size) {
		while (merge_log_zones() != 0) {}
		reclaim_empty_log_zones();
		count++;
	}

	records->data_mtx.unlock();
	records->log_mtx.unlock();
}

void gc_loop() {
	auto const records = DataMap::get_instance();
	records->gc_running = true;
	while (records->should_run_gc) {
		std::this_thread::sleep_for(std::chrono::milliseconds(30));

		records->log_mtx.lock_shared();
		uint64_t num_free_log_pages = records->count_free_log_pages();
		records->log_mtx.unlock_shared();
		while (num_free_log_pages / records->dd->zone_size < records->dd->gc_wmark) {
			gc();
			records->log_mtx.lock_shared();
			num_free_log_pages = records->count_free_log_pages();
			records->log_mtx.unlock_shared();
		}
	}
	PROFILER_FLUSH();
	records->gc_exited = true;
}

int zns_udevice_read(struct user_zns_device *my_dev, uint64_t address, void *buffer, uint32_t size) {
	PROFILE_NVME_FUNCTION();
	UNUSED(my_dev);
	auto const records = DataMap::get_instance();
	records->log_mtx.lock_shared();
	records->data_mtx.lock_shared();

	uint64_t size_c = size;
	uint64_t end_address = address + size;
	while (size_c > 0) {
		uint64_t op_size;
		ftl_record record, next_record;

		if (records->get_from_cache(address, (char *)buffer + (size - size_c))) {
			op_size = records->device->lba_size_bytes;
			size_c -= op_size;
			address += size - size_c;
			continue;
		}

		bool in_log = true;
		if (records->find_in_log(address, record)) {
			in_log = true;
		} else if (records->find_in_data(address, record)) {
			in_log = false;
		} else {
			records->data_mtx.unlock_shared();
			records->log_mtx.unlock_shared();
			return -1;
		}

		bool is_next_address_in_same = true;
		uint64_t base_p_address = closest_lower_multiple(records->dd->zone_size, record.p_address);

		uint64_t next_address = address;
		while (is_next_address_in_same && next_address < end_address) {
			next_address += records->device->lba_size_bytes;
			if (in_log) {
				if (!records->find_in_log(next_address, next_record)) {
					break;
				}
			} else {
				if (!records->find_in_data(next_address, next_record)) {
					break;
				}
			}
			is_next_address_in_same =
					closest_lower_multiple(records->dd->zone_size, next_record.p_address) == base_p_address;
		}

		op_size = std::min(size_c, next_address - address == 0 ? records->device->lba_size_bytes : next_address - address);

		int ret = ss_nvme_device_io_with_mdts(record.p_address, (char *)buffer + (size - size_c), op_size, true);
		if (ret != 0) {
			std::cerr << "ERROR: reading with mdts in zns_udevice_read: " << nvme_status_to_string(ret, false)
					  << std::endl;
			return ret;
		}
		records->cache(record.l_address, (char *)buffer + (size - size_c));
		size_c -= op_size;
		address += size - size_c;
	}

	records->data_mtx.unlock_shared();
	records->log_mtx.unlock_shared();

	return 0;
}

int zns_udevice_write(struct user_zns_device *my_dev, uint64_t address, void *buffer, uint32_t size) {
	PROFILE_NVME_FUNCTION();
	auto const records = DataMap::get_instance();
	UNUSED(my_dev);
	records->log_mtx.lock();
	uint64_t size_c = size;
	while (size_c > 0) {
		int zone_idx;
		uint64_t zone_size = records->get_free_log_zone(address, zone_idx);
		while (zone_idx == -1) {
			records->log_mtx.unlock();
			gc();
			records->log_mtx.lock();
			zone_size = records->get_free_log_zone(address, zone_idx);
		}

		const uint64_t op_size =
				std::min((records->dd->zone_size - zone_size) * records->device->lba_size_bytes, size_c);
		const uint64_t num_operations = op_size / records->device->lba_size_bytes;
		records->write_counter += num_operations;
		int ret = ss_nvme_device_io_with_mdts(records->log_zones[zone_idx].write_ptr, (char *)buffer + (size - size_c),
											  op_size, false);
		if (ret != 0) {
			std::cerr << "ERROR: zns_udevice_write zone write failed" << std::endl;
			return ret;
		}

		for (uint64_t i = 0; i < num_operations; i++) {
			auto const l_address = address + (i * records->device->lba_size_bytes);
			auto const base_address = closest_lower_multiple(records->device->tparams.zns_zone_capacity, l_address);

			records->delete_from_log_if_exists(l_address);
			records->add_or_update(zone_idx,
								   {.l_address = l_address,
									.p_address = records->log_zones[zone_idx].write_ptr + i,
									.base_address = base_address,
									.flag = status_flag::Valid});
			records->cache(l_address, (char *)buffer + (size - size_c) + (i * records->device->lba_size_bytes));
		}
		records->log_zones[zone_idx].write_ptr += num_operations;
		size_c -= op_size;
		address += size - size_c;
	}
	records->log_mtx.unlock();
	return 0;
}

bool is_fresh_start() {
	PROFILE_NVME_FUNCTION();
	ftl_record record;
	auto records = DataMap::get_instance();
	if (records->find_in_log(0, record)) {
		const uint64_t size_zone_report =
				sizeof(nvme_zone_report) + records->device->tparams.zns_num_zones * sizeof(nvme_zns_desc);
		std::vector<uint8_t> zone_report;
		zone_report.resize(size_zone_report);
		int ret = nvme_zns_report_zones(records->dd->fd, records->dd->nsid, 0, false, NVME_ZNS_ZRAS_REPORT_ALL, true,
										size_zone_report, &zone_report[0]);
		if (ret != 0) {
			std::cerr << "ERROR: could not get report-zones: " << nvme_status_to_string(ret, false) << std::endl;
		}

		struct nvme_zone_report *r = (nvme_zone_report *)&zone_report[0];
		auto const zone_num =
				(closest_lower_multiple(records->dd->zone_size, record.p_address) / records->dd->zone_size) + 1;
		struct nvme_zns_desc *zone_desc =
				(struct nvme_zns_desc *)(&zone_report[0] + sizeof(*r) + zone_num * sizeof(*zone_desc));

		return zone_desc->wp == 0;
	} else if (records->find_in_data(0, record)) {
		return false;
	}
	return true;
}
