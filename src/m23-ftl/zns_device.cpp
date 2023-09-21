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

std::vector<ftl_record> logs;
DataMap log_records;
uint32_t write_ptr;

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

typedef int (*nvme_op)(int, uint32_t, uint64_t, uint16_t, void *, uint64_t);
inline int ss_nvme_device_io_with_mdts(int fd, uint32_t nsid, uint64_t slba, void *buffer, uint64_t buf_size,
									   uint64_t lba_size, uint64_t mdts_size, bool read) {
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

	// logs.reserve(dd->zone_size * params->log_zones);
	log_records = DataMap(*my_dev, dd);

	std::thread t(gc_thread, *my_dev);
	dd->t_handle = t.native_handle();
	t.detach();
	return 0;
}

void gc_thread(const struct user_zns_device *my_dev) {}

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
	struct device_data *dd = ((device_data *)my_dev->_private);

	uint32_t size_c = size;
	while (size_c > 0) {
		for (uint i = 0; i < logs.size(); i++) {
			if (logs[i].l_address == address + ((size - size_c) / my_dev->lba_size_bytes) &&
				logs[i].flag == status_flag::Valid) {
				uint upto = std::min((uint)logs.size(), size_c);
				uint j = i;
				for (; j < upto; j++) {
					if (logs[j].l_address != address + size - size_c + j - i || logs[j].flag != status_flag::Valid) {
						break;
					}
				}

				uint bytes_read = (j - i) * my_dev->lba_size_bytes;
				int ret = ss_nvme_device_io_with_mdts(dd->fd, dd->nsid, logs[i].p_address,
													  (char *)buffer + (size - size_c), bytes_read,
													  my_dev->lba_size_bytes, dd->mdts, true);
				if (ret != 0) {
					return ret;
				}

				size_c -= bytes_read;

				break;
			}
		}
	}
	return 0;
}

int ss_zns_device_zone_append(int fd, uint32_t nsid, uint64_t zslba, int numbers, void *buffer, uint32_t buf_size,
							  uint64_t *written_slba) {
	return nvme_zns_append(fd, nsid, zslba, numbers - 1, 0, 0, 0, 0, buf_size, buffer, 0, nullptr,
						   (__u64 *)written_slba);
}

void mark_invalid_if_exists(const uint64_t address, const uint32_t size) {
	uint32_t size_c = size;
	for (uint i = 0; i < logs.size(); i++) {
		if (logs[i].flag == status_flag::Invalid) {
			continue;
		}
		if (logs[i].l_address == address + size - size_c && logs[i].flag == status_flag::Valid) {
			uint upto = std::min((uint)logs.size(), size_c);
			uint j = i;
			for (; j < upto; j++) {
				if (logs[j].l_address != address + size - size_c + j - i || logs[j].flag != status_flag::Valid) {
					break;
				}
			}

			for (uint k = i; k < j; k++) {
				logs[k].flag = status_flag::Invalid;
			}

			size_c -= j - i;
			break;
		}
	}
}

int zns_udevice_write(struct user_zns_device *my_dev, uint64_t address, void *buffer, uint32_t size) {
	struct device_data *dd = ((device_data *)my_dev->_private);
	uint32_t size_c = size;
	while (size_c > 0) {
		if (write_ptr % dd->zone_size == 0) {
			int ret = ss_zns_device_zone_reset(dd->fd, dd->nsid, write_ptr);
			if (ret != 0) {
				return ret;
			}
		}

		mark_invalid_if_exists(address, size / my_dev->lba_size_bytes);

		const uint64_t op_size = std::min(closest_upper_multiple(my_dev->lba_size_bytes, size_c), dd->mdts);
		const uint64_t num_operations = op_size / my_dev->lba_size_bytes;
		auto slba = closest_lower_multiple(dd->zone_size, write_ptr);
		const int ret = ss_nvme_device_io_with_mdts(dd->fd, dd->nsid, slba, (char *)buffer + (size - size_c), op_size,
													my_dev->lba_size_bytes, dd->mdts, false);
		if (ret != 0) {
			return ret;
		}

		for (uint i = 0; i < num_operations; i++) {
			log_records.add_or_update({.l_address = address + i + ((size - size_c) / my_dev->lba_size_bytes),
									   .p_address = write_ptr + i,
									   .flag = status_flag::Valid});
		}
		write_ptr += num_operations;
		size_c -= op_size;
	}

	return 0;
}