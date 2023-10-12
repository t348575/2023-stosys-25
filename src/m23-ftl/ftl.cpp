#include <algorithm>
#include <cstdint>
#include <iostream>

#include "../common/VisualProfiler.hpp"
#include "../common/utils.h"
#include "ftl.h"

#define PROFILE_FTL_FUNCTION() PROFILE_FUNCTION_CATEGORY("ftl")

DataMap *DataMap::records = nullptr;

inline uint64_t closest_lower_multiple(uint64_t n, uint64_t x) { return x - (x % n); }

inline int ss_zns_device_zone_reset(int fd, uint32_t nsid, uint64_t slba) {
	PROFILE_FTL_FUNCTION();
	return nvme_zns_mgmt_send(fd, nsid, slba, false, NVME_ZNS_ZSA_RESET, 0, nullptr);
}

void DataMap::init(user_zns_device *_device, const device_data *_dd) {
	PROFILE_FTL_FUNCTION();
	device = _device;
	dd = _dd;

	const uint64_t size_zone_report = sizeof(nvme_zone_report) + device->tparams.zns_num_zones * sizeof(nvme_zns_desc);
	std::vector<uint8_t> zone_report;
	zone_report.resize(size_zone_report);
	int ret = nvme_zns_report_zones(dd->fd, dd->nsid, 0, false, NVME_ZNS_ZRAS_REPORT_ALL, true, size_zone_report,
									&zone_report[0]);
	if (ret != 0) {
		std::cerr << "ERROR: could not get report-zones: " << nvme_status_to_string(ret, false) << std::endl;
	}

	struct nvme_zone_report *r = (nvme_zone_report *)&zone_report[0];
	struct nvme_zns_desc *desc = (nvme_zns_desc *)(&zone_report[0] + sizeof(*r));

	log_zones.reserve(dd->log_zones - 1);

	// fresh start
	if (desc->wp == 0) {
		uint32_t i = 1;
		for (; i < dd->log_zones; i++) {
			log_zones.push_back(LogRecord(i * dd->zone_size, dd->zone_size));
		}
		for (uint32_t j = 0; i < device->tparams.zns_num_zones; i++, j++) {
			data_zones.push_back(DataRecord({.l_address = 0,
											 .p_address = i * dd->zone_size,
											 .base_address = j * device->tparams.zns_zone_capacity,
											 .flag = status_flag::Invalid}));
		}
		init_cache();
		return;
	}

	std::vector<uint8_t> metadata_zone;
	metadata_zone.resize(device->tparams.zns_zone_capacity);
	ret = ss_nvme_device_io_with_mdts(0, &metadata_zone[0], device->tparams.zns_zone_capacity, true);
	if (ret != 0) {
		std::cerr << "ERROR: could not read metadata zone: " << nvme_status_to_string(ret, false) << std::endl;
	}

	const uint64_t num_blocks = u64_from_buffer(&metadata_zone[(desc->wp * device->lba_size_bytes) - 8]);
	uint64_t buffer_ptr = (desc->wp * device->lba_size_bytes) - (num_blocks * device->lba_size_bytes);

	for (uint64_t i = 0; i < dd->n_zones; i++) {
		ftl_record record;
		record.flag = (status_flag)metadata_zone[buffer_ptr];
		record.base_address = u64_from_buffer(&metadata_zone[buffer_ptr + 1]);
		record.p_address = u64_from_buffer(&metadata_zone[buffer_ptr + 9]);
		data_zones.push_back(DataRecord(record));
		buffer_ptr += 17;
	}

	for (uint64_t i = 0; i < dd->log_zones - 1; i++) {
		const bool is_seq = (bool)metadata_zone[buffer_ptr];
		if (is_seq) {
			const uint64_t base_address = u64_from_buffer(&metadata_zone[buffer_ptr + 1]);
			const uint64_t p_address = u64_from_buffer(&metadata_zone[buffer_ptr + 9]);
			buffer_ptr += 17;

			log_zones.push_back(LogRecord(p_address, dd->zone_size));
			const uint64_t zone_num = p_address / dd->zone_size;

			log_zones[i].write_ptr =
					((struct nvme_zns_desc *)(&zone_report[0] + sizeof(*r) + zone_num * sizeof(*desc)))->wp;
			if (log_zones[i].write_ptr == 0xFFFFFFFFFFFFFFFF) {
				log_zones[i].write_ptr = (zone_num + 1) * dd->zone_size;
			}
			log_zones[i].zone_size = log_zones[i].write_ptr - p_address;
			log_zones[i].base_address = base_address;

			log_zones[i].is_seq = true;
			for (uint64_t j = 0; j < log_zones[i].zone_size; j++) {
				log_zones[i].logs.push_back({.l_address = base_address + (j * device->lba_size_bytes),
											 .p_address = p_address + j,
											 .base_address = base_address,
											 .flag = status_flag::Valid});
			}
		} else {
			const uint64_t p_address = u64_from_buffer(&metadata_zone[buffer_ptr + 1]);
			const uint64_t log_size = u64_from_buffer(&metadata_zone[buffer_ptr + 9]);
			buffer_ptr += 17;

			log_zones.push_back(LogRecord(p_address, dd->zone_size));
			const uint64_t zone_num = p_address / dd->zone_size;

			log_zones[i].write_ptr =
					((struct nvme_zns_desc *)(&zone_report[0] + sizeof(*r) + zone_num * sizeof(*desc)))->wp;
			if (log_zones[i].write_ptr == 0xFFFFFFFFFFFFFFFF) {
				log_zones[i].write_ptr = (zone_num + 1) * dd->zone_size;
			}
			log_zones[i].zone_size = log_zones[i].write_ptr - p_address;

			for (uint64_t j = 0; j < log_size; j++) {
				const uint64_t l_address = u64_from_buffer(&metadata_zone[buffer_ptr]);
				const uint64_t inner_p_address = u64_from_buffer(&metadata_zone[buffer_ptr + 8]);
				buffer_ptr += 16;

				log_zones[i].logs.push_back(
						{.l_address = l_address,
						 .p_address = inner_p_address,
						 .base_address = closest_lower_multiple(device->tparams.zns_zone_capacity, l_address),
						 .flag = status_flag::Valid});
			}
		}
	}
	init_cache();
}

void DataMap::add_or_update(const uint64_t idx, const ftl_record record) {
	PROFILE_FTL_FUNCTION();
	log_zones[idx].logs.push_back(record);
	log_zones[idx].zone_size++;
}

uint64_t DataMap::get_free_log_zone(const uint64_t address, int &index) {
	PROFILE_FTL_FUNCTION();
	auto const size = log_zones.size();
#ifdef PREFER_SEQ_LOG
	auto const base_address = closest_lower_multiple(device->tparams.zns_zone_capacity, address);
	for (uint64_t i = 0; i < size; i++) {
		if (log_zones[i].is_seq && log_zones[i].base_address == base_address &&
			log_zones[i].zone_size != dd->zone_size) {
			index = i;
			return log_zones[i].zone_size;
		}
	}

	for (uint64_t i = 0; i < size; i++) {
		if (log_zones[i].zone_size == 0) {
			log_zones[i].is_seq = true;
			log_zones[i].base_address = base_address;
			index = i;
			return log_zones[i].zone_size;
		}
	}

	for (uint64_t i = 0; i < size; i++) {
		if (!log_zones[i].is_seq && log_zones[i].zone_size != dd->zone_size) {
			index = i;
			return log_zones[i].zone_size;
		}
	}
#endif

	for (uint64_t i = 0; i < size; i++) {
		if (log_zones[i].zone_size != dd->zone_size) {
			log_zones[i].is_seq = false;
			index = i;
			return log_zones[i].zone_size;
		}
	}

	index = -1;
	return 0;
}

uint64_t DataMap::count_free_log_pages() {
	PROFILE_FTL_FUNCTION();
	uint64_t count = 0;
	for (const auto &t : log_zones) {
		count += dd->zone_size - t.zone_size;
	}
	return count;
}

std::vector<struct count_base_address_sums> DataMap::count_base_addresses() {
	PROFILE_FTL_FUNCTION();
	std::vector<struct count_base_address_sums> sums;
	sums.reserve(dd->log_zones * dd->zone_size);
	for (const auto &outer : log_zones) {
		for (const auto &t : outer.logs) {
			bool found = false;
			for (auto &s : sums) {
				if (s.base_address == t.base_address) {
					s.count++;
					found = true;
					break;
				}
			}

			if (!found) {
				sums.push_back({.base_address = t.base_address, .p_address = t.p_address, .count = 1});
			}
		}
	}
	std::sort(sums.rbegin(), sums.rend(),
			  [](const struct count_base_address_sums &a, const struct count_base_address_sums &b) {
				  return a.count < b.count;
			  });
	return sums;
}

std::vector<struct count_base_address_sums> LogRecord::count_base_addresses() {
	PROFILE_FTL_FUNCTION();
	std::vector<struct count_base_address_sums> sums;
	sums.reserve(logs.size());
	for (const auto &t : logs) {
		bool found = false;
		for (auto &s : sums) {
			if (s.base_address == t.base_address) {
				s.count++;
				found = true;
				break;
			}
		}

		if (!found) {
			sums.push_back({.base_address = t.base_address, .p_address = t.p_address, .count = 1});
		}
	}
	std::sort(sums.rbegin(), sums.rend(),
			  [](const struct count_base_address_sums &a, const struct count_base_address_sums &b) {
				  return a.count < b.count;
			  });
	return sums;
}

std::vector<ftl_record> LogRecord::remove_base_address(const uint64_t base_address, const bool simulate) {
	PROFILE_FTL_FUNCTION();
	std::vector<ftl_record> removing;
	removing.reserve(logs.size());
	for (uint64_t i = 0; i < logs.size(); i++) {
		if (logs[i].base_address == base_address) {
			removing.push_back(logs[i]);
			if (!simulate) {
				logs.erase(logs.begin() + i);
				i--;
			}
		}
	}
	return removing;
}

bool DataMap::get_data_zone(const uint64_t l_address, ftl_record &record, uint64_t &idx) {
	PROFILE_FTL_FUNCTION();
	for (uint64_t i = 0; i < data_zones.size(); i++) {
		if (data_zones[i].record.base_address == l_address) {
			if (data_zones[i].record.flag == status_flag::Invalid) {
				data_zones[i].record.flag = status_flag::Valid;
			}

			record = data_zones[i].record;
			idx = i;
			return true;
		}
	}

	return false;
}

bool DataMap::remove_unused_data_zone(const uint64_t base_address, ftl_record &record) {
	PROFILE_FTL_FUNCTION();
	for (uint64_t i = 0; i < data_zones.size(); i++) {
		if (data_zones[i].record.base_address == base_address) {
			record = data_zones[i].record;
			data_zones.erase(data_zones.begin() + i);
			return true;
		}
	}
	return false;
}

bool DataMap::find_in_log(const uint64_t address, ftl_record &record) {
	PROFILE_FTL_FUNCTION();
	for (auto &t : log_zones) {
		for (const auto &inner : t.logs) {
			if (inner.l_address == address) {
				record = inner;
				return true;
			}
		}
	}

	return false;
}

bool DataMap::find_in_data(const uint64_t address, ftl_record &record) {
	PROFILE_FTL_FUNCTION();
	const auto base_address = closest_lower_multiple(device->tparams.zns_zone_capacity, address);
	for (auto &t : data_zones) {
		if (t.record.base_address == base_address && t.record.flag == status_flag::Valid) {
			record = t.record;
			record.p_address += (address - base_address) / device->lba_size_bytes;
			return true;
		}
	}

	return false;
}

void DataMap::set_data_zone(const uint64_t idx, const ftl_record record) { data_zones[idx].record = record; }

void DataMap::delete_from_log_if_exists(const uint64_t address) {
	PROFILE_FTL_FUNCTION();
	for (auto &t : log_zones) {
		for (uint64_t i = 0; i < t.logs.size(); i++) {
			if (t.logs[i].l_address == address) {
				t.logs.erase(t.logs.begin() + i);
				i--;
				return;
			}
		}
	}
}

void DataMap::write_metadata(std::string file_name) {
	records->log_mtx.lock();
	records->data_mtx.lock();
	nvme_zone_report zone_report;
	int ret = nvme_zns_report_zones(dd->fd, dd->nsid, 0, false, NVME_ZNS_ZRAS_REPORT_ALL, true, sizeof(zone_report),
									&zone_report);
	const nvme_zns_desc *zone_desc = (struct nvme_zns_desc *)(void *)&zone_report;

	uint64_t bytes_needed = 0;
	for (const auto &lz : log_zones) {
		bytes_needed++;
		if (lz.is_seq) {
			bytes_needed += 16;
		} else {
			bytes_needed += (lz.logs.size() * 2 * 16) + 16;
		}
	}

	bytes_needed += dd->n_zones * 17;
	bytes_needed += 8;

	const uint64_t blocks_needed =
			closest_upper_multiple(device->lba_size_bytes, bytes_needed) / device->lba_size_bytes;
	if (zone_desc->wp + blocks_needed >= dd->zone_size) {
		ret = ss_zns_device_zone_reset(dd->fd, dd->nsid, 1);
		if (ret != 0) {
			std::cerr << "ERROR: Could not reset metadata zone: " << nvme_status_to_string(ret, false) << std::endl;
			return;
		}
	}

	std::vector<uint8_t> buffer;
	buffer.resize(blocks_needed * device->lba_size_bytes);
	uint64_t buffer_ptr = 0;
	for (auto const &dz : data_zones) {
		buffer[buffer_ptr] = dz.record.flag;
		u64_to_buffer(dz.record.base_address, &buffer[buffer_ptr + 1]);
		u64_to_buffer(dz.record.p_address, &buffer[buffer_ptr + 9]);
		buffer_ptr += 17;
	}

	for (auto const &lz : log_zones) {
		buffer[buffer_ptr] = lz.is_seq;
		if (lz.is_seq) {
			u64_to_buffer(lz.base_address, &buffer[buffer_ptr + 1]);
			u64_to_buffer(lz.p_address, &buffer[buffer_ptr + 9]);
			buffer_ptr += 17;
		} else {
			u64_to_buffer(lz.p_address, &buffer[buffer_ptr + 1]);
			u64_to_buffer(lz.logs.size(), &buffer[buffer_ptr + 9]);
			buffer_ptr += 17;
			for (auto const &entry : lz.logs) {
				u64_to_buffer(entry.l_address, &buffer[buffer_ptr]);
				u64_to_buffer(entry.p_address, &buffer[buffer_ptr + 8]);
				buffer_ptr += 16;
			}
		}
	}
	u64_to_buffer(blocks_needed, &buffer[buffer.size() - 8]);
	ret = ss_nvme_device_io_with_mdts(zone_desc->wp, &buffer[0], buffer.size(), false);
	if (ret != 0) {
		std::cerr << "ERROR: could not write metadata to disk: " << nvme_status_to_string(ret, false) << std::endl;
	}

	records->log_mtx.unlock();
	records->data_mtx.unlock();
}