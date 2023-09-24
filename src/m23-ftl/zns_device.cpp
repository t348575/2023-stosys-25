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
#include <functional>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "../common/unused.h"
#include "ftl.h"
#include "zns_device.h"
#include "visual_profiler.h"

std::vector<ftl_record> logs;
DataMap records;

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
	PROFILE_FUNCTION();
	// std::cout << "reset " << std::to_string(slba) << std::endl;
	return nvme_zns_mgmt_send(fd, nsid, slba, false, NVME_ZNS_ZSA_RESET, 0, nullptr);
}

typedef int (*nvme_op)(int, uint32_t, uint64_t, uint16_t, void *, uint64_t);
inline int ss_nvme_device_io_with_mdts(int fd, uint32_t nsid, uint64_t slba, void *buffer, uint64_t buf_size,
									   uint64_t lba_size, uint64_t mdts_size, bool read) {
	std::string name = "mdts_" + std::string(read ? "read" : "write");
	PROFILE_SCOPE(name);
	uint16_t numbers;
	int ret = 0;
	uint64_t size = buf_size;
	nvme_op op;
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

int ss_zns_device_zone_append(int fd, uint32_t nsid, uint64_t zslba, int numbers, void *buffer, uint32_t buf_size,
							  uint64_t *written_slba) {
	return nvme_zns_append(fd, nsid, zslba, numbers - 1, 0, 0, 0, 0, buf_size, buffer, 0, nullptr,
						   (__u64 *)written_slba);
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
	records.should_run_gc = false;
	records.ready_to_force_gc = true;
	records.force_gc.notify_one();
	while(!records.gc_exited) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

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

	*my_dev = (user_zns_device *)calloc(sizeof(user_zns_device), 1);
	(**my_dev).lba_size_bytes = block_size;
	(**my_dev).capacity_bytes = (zones.nr_zones - params->log_zones) * zns_ns.lbafe->zsze * block_size;
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
	dd->gc_wmark = params->gc_wmark;
	dd->log_zones = params->log_zones;
	dd->n_zones = zones.nr_zones - params->log_zones;

	nvme_zns_id_ctrl id_ctrl;
	ret = nvme_zns_identify_ctrl(fd, &id_ctrl);
	if (ret != 0) {
		return ret;
	}
	dd->zasl = (1 << id_ctrl.zasl) * (**my_dev).lba_size_bytes;

	records.init(*my_dev, dd);

	std::thread t1(gc_loop);
	dd->t_handle_loop = t1.native_handle();
	t1.detach();

	std::thread t2(gc_immediate);
	dd->t_handle_immediate = t2.native_handle();
	t2.detach();

	return 0;
}

void reclaim_log_zones() {
	for (auto &t: records.log_zones) {
		if (t.logs.size() == 0 && t.zone_size == records.dd->zone_size) {
			t.zone_size = 0;
			int ret = ss_zns_device_zone_reset(records.dd->fd, records.dd->nsid, t.p_address);
			if (ret != 0) {
				std::cerr << "ERROR: could not reset zone while reclaiming log zone" << std::endl;
			}
		}
	}
}

void merge_log_to_data(LogRecord &t, const std::vector<count_base_address_sums> &sums) {
	const auto zone_size = records.device->tparams.zns_zone_capacity;
	char *log_zone = new char[zone_size];
	int ret = ss_nvme_device_io_with_mdts(
			records.dd->fd, records.dd->nsid, closest_lower_multiple(records.dd->zone_size, sums[0].p_address),
			log_zone, zone_size, records.device->lba_size_bytes, records.dd->mdts, true);
	if (ret != 0) {
		std::cerr << "ERROR: could not read log zone in gc_loop" << std::endl;
		return;
	}

	auto removed = t.remove_base_address(sums[0].base_address);
	if (removed.size() == 0) {
		delete[] log_zone;
		return;
	}

	char *data_zone = new char[zone_size];
	ftl_record data_zone_record;
	uint64_t data_zone_idx;
	if (!records.get_data_zone(sums[0].base_address, data_zone_record, data_zone_idx)) {
		std::cerr << "ERROR: no free data zones found" << std::endl;
		return;
	}
	records.set_data_zone(data_zone_idx,
							{.l_address = 0,
							.p_address = data_zone_record.p_address,
							.base_address = sums[0].base_address,
							.flag = status_flag::Valid});
	// auto guard_data = WriteGuard(records.data_zones[data_zone_idx]);

	ret = ss_nvme_device_io_with_mdts(records.dd->fd, records.dd->nsid, data_zone_record.p_address, data_zone, zone_size, records.device->lba_size_bytes, records.dd->mdts, true);
	if (ret != 0) {
		std::cerr << "ERROR: could not read data zone in gc_loop" << std::endl;
		return;
	}

	ret = ss_zns_device_zone_reset(records.dd->fd, records.dd->nsid, data_zone_record.p_address);
	if (ret != 0) {
		std::cerr << "ERROR: could not reset data zone" << std::endl;
		return;
	}

	const auto sub_by = closest_lower_multiple(records.dd->zone_size, removed[0].p_address);
	for (const auto &r : removed) {
		memcpy(data_zone + (r.l_address - r.base_address),
				log_zone + ((r.p_address - sub_by) * records.device->lba_size_bytes),
				records.device->lba_size_bytes);
	}

	// TODO: decide to stick with write or append
	// ret = ss_nvme_device_io_with_mdts(records.dd->fd, records.dd->nsid, data_zone_record.p_address, data_zone,
	// 								  zone_size, records.device->lba_size_bytes, records.dd->mdts, false);
	auto num_loop = zone_size / records.dd->zasl;
	auto base_loop = records.dd->zasl / records.device->lba_size_bytes;
	uint64_t res;
	for (uint64_t s = 0; s < num_loop; s++) {
		ret = ss_zns_device_zone_append(records.dd->fd, records.dd->nsid, data_zone_record.p_address, base_loop, data_zone + (s * records.dd->zasl), records.dd->zasl, &res);
		if (ret != 0) {
			std::cerr << "ERROR: could not write to data zone in gc_loop" << std::endl;
			return;
		}
	}
	// if (ret != 0) {
	// 	std::cerr << "ERROR: could not write to data zone in gc_loop" << std::endl;
	// 	continue;
	// }
	delete[] log_zone;
	delete[] data_zone;
	reclaim_log_zones();
}

void check_merge() {
	for (auto &t : records.log_zones) {
		auto sums = t.count_base_addresses();
		if (sums.size() == 0) {
			continue;
		}

		PROFILE_SCOPE("gc_cleanup");
		merge_log_to_data(t, sums);
	}
}

void cleanup_logs() {
	std::vector<ftl_record> cleanup_logs;
	uint64_t zone_to_use = 0;
	cleanup_logs.reserve(records.dd->zone_size * 2);
	for (auto &t: records.log_zones) {
		if (t.logs.size() == records.dd->zone_size) {
			continue;
		}

		if (t.logs.size() + cleanup_logs.size() > records.dd->zone_size) {
			continue;
		}

		cleanup_logs.insert(cleanup_logs.end(), t.logs.begin(), t.logs.end());
		t.logs.clear();
		t.zone_size = 0;
		zone_to_use = t.p_address;
	}

	if (cleanup_logs.size() == 0) {
		return;
	}

	for (auto &t: records.log_zones) {
		if (t.p_address == zone_to_use) {
			t.logs.insert(t.logs.end(), cleanup_logs.begin(), cleanup_logs.end());
			break;
		}
	}

	auto new_log_size = cleanup_logs.size() * records.device->lba_size_bytes;
	char *buffer = new char[new_log_size];
	uint64_t buffer_ptr = 0;
	for (uint64_t i = 0; i < cleanup_logs.size(); i++) {
		auto prev = cleanup_logs[i].p_address;
		uint64_t j;
		for (j = i; j < cleanup_logs.size(); j++) {
			if (cleanup_logs[j].p_address != prev + 1) {
				break;
			}
			prev = cleanup_logs[j].p_address;
		}

		auto read_size = j - i > 0 ? (prev - cleanup_logs[i].p_address) * records.device->lba_size_bytes: records.device->lba_size_bytes;
		int ret = ss_nvme_device_io_with_mdts(records.dd->fd, records.dd->nsid, cleanup_logs[i].p_address, buffer + buffer_ptr, read_size, records.device->lba_size_bytes, records.dd->mdts, true);
		if (ret != 0) {
			std::cerr << "ERROR: could not read log data" << std::endl;
			return;
		}
	}

	int ret = ss_zns_device_zone_reset(records.dd->fd, records.dd->nsid, zone_to_use);
	if (ret != 0) {
		std::cerr << "ERROR: could not reset data zone" << std::endl;
		return;
	}

	auto num_loop = (new_log_size + (records.dd->zasl - 1)) / records.dd->zasl;
	uint64_t res;
	for (uint64_t s = 0; s < num_loop; s++) {
		auto const buf_size = std::min(records.dd->zasl, new_log_size - (s * records.dd->zasl));
		ret = ss_zns_device_zone_append(records.dd->fd, records.dd->nsid, zone_to_use, buf_size / records.device->lba_size_bytes, buffer + (s * records.dd->zasl), buf_size, &res);
		if (ret != 0) {
			std::cerr << "ERROR: could not write to data zone in gc_loop for log re-write" << std::endl;
			return;
		}
	}

	delete[] buffer;
}

void gc() {
	if (records.get_num_free_log_zones() <= records.dd->gc_wmark) {
		reclaim_log_zones();
		if (records.get_num_free_log_zones() <= records.dd->gc_wmark) {
			cleanup_logs();
		}
	}

	if (records.in_action) {
		return;
	}

	PROFILE_SCOPE("gc");
	check_merge();
	reclaim_log_zones();
}

void gc_loop() {
	while (records.should_run_gc) {
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
		std::lock_guard<std::mutex> lk(records.mtx);
		gc();
	}
	records.gc_exited = true;
}

void gc_immediate() {
	while (records.should_run_gc) {
		std::unique_lock lk(records.force_gc_mtx);
		records.force_gc.wait(lk, []{ return records.ready_to_force_gc; });

		if (!records.should_run_gc) {
			return;
		}

		gc();
		records.ready_to_force_gc = false;
	}
}

uint64_t closest_upper_multiple(uint64_t n, uint64_t x) {
	if (x > n) {
		return x;
	}

	n = n + x / 2;
	n = n - (n % x);
	return n;
}
uint64_t closest_lower_multiple(uint64_t n, uint64_t x) { return x - (x % n); }

int zns_udevice_read(struct user_zns_device *my_dev, uint64_t address, void *buffer, uint32_t size) {
	records.in_action = true;
	PROFILE_FUNCTION();
	struct device_data *dd = ((device_data *)my_dev->_private);

	std::lock_guard<std::mutex> lk(records.mtx);

	uint32_t size_c = size;
	while (size_c > 0) {
		auto const find_address = address + (size - size_c);
		uint64_t possible_op_size = std::min(records.dd->mdts, (uint64_t)size_c);
		ftl_record record, end_record;
		records.find(find_address, record);
		bool found = false;

		do {
			found = records.find(find_address + possible_op_size, end_record);
			possible_op_size -= records.device->lba_size_bytes;
		} while(!found && closest_lower_multiple(records.dd->zone_size, record.p_address) != closest_lower_multiple(records.dd->zone_size, end_record.p_address));
		const auto op_size = std::min(possible_op_size + records.device->lba_size_bytes, (uint64_t)size_c);

		int ret = ss_nvme_device_io_with_mdts(dd->fd, dd->nsid, record.p_address, (char *)buffer + (size - size_c),
											  op_size, my_dev->lba_size_bytes, dd->mdts, true);
		if (ret != 0) {
			records.in_action = false;
			return ret;
		}
		size_c -= op_size;
	}

	records.in_action = false;
	return 0;
}

int zns_udevice_write(struct user_zns_device *my_dev, uint64_t address, void *buffer, uint32_t size) {
	records.in_action = true;
	PROFILE_FUNCTION();
	std::lock_guard<std::mutex> lk(records.mtx);

	struct device_data *dd = ((device_data *)my_dev->_private);
	uint32_t size_c = size;
	while (size_c > 0) {
		int free_log_zone;
		uint64_t zone_size = records.get_free_log_zone(free_log_zone);
		if (free_log_zone == -1) {
			while (free_log_zone == -1) {
				// std::lock_guard lk(records.force_gc_mtx);
				// records.ready_to_force_gc = true;
				// records.force_gc.notify_one();
				gc();
				zone_size = records.get_free_log_zone(free_log_zone);
			}
		}

		const uint64_t slba = records.get_log_p_address(free_log_zone);
		if (zone_size == 0) {
			int ret = ss_zns_device_zone_reset(dd->fd, dd->nsid, slba);
			// std::cout << "reset here " << std::to_string(slba) << std::endl;
			if (ret != 0) {
				records.in_action = false;
				return ret;
			}
		}

		const uint64_t op_size = std::min(closest_upper_multiple(my_dev->lba_size_bytes, size_c), dd->zasl);
		const uint64_t num_operations = op_size / my_dev->lba_size_bytes;
		uint64_t written_slba;
		const int ret = ss_zns_device_zone_append(dd->fd, dd->nsid, slba, num_operations,
												  (char *)buffer + (size - size_c), op_size, &written_slba);
		if (ret != 0) {
			records.in_action = false;
			std::cout << "failed at " << std::to_string(slba) << std::endl;
			return ret;
		}

		for (uint i = 0; i < num_operations; i++) {
			uint64_t l_address = address + (size - size_c) + i * my_dev->lba_size_bytes;
			records.delete_if_exists(l_address);
			records.add_or_update(free_log_zone,
								  {
										  .l_address = l_address,
										  .p_address = written_slba + i,
										  .base_address = closest_lower_multiple(
												  records.device->tparams.zns_zone_capacity, l_address),
										  .flag = status_flag::Valid,
								  });
		}
		size_c -= op_size;
	}

	records.in_action = false;
	return 0;
}