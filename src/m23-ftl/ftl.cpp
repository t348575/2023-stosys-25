#include <algorithm>
#include <cstdint>

#include "ftl.h"
#include "visual_profiler.h"

inline int ss_zns_device_zone_reset(int fd, uint32_t nsid, uint64_t slba) {
	PROFILE_FUNCTION();
	return nvme_zns_mgmt_send(fd, nsid, slba, false, NVME_ZNS_ZSA_RESET, 0, nullptr);
}

void DataMap::init(const user_zns_device *_device, const device_data *_dd) {
	device = _device;
	dd = _dd;
	PROFILE_FUNCTION();
	log_zones.reserve(device->tparams.zns_num_zones);
	uint32_t j = 0;
	for (uint32_t i = 0; i < device->tparams.zns_num_zones; i++) {
		if (i < dd->log_zones) {
			log_zones.push_back(LogRecord(i * dd->zone_size, dd->zone_size));
			ss_zns_device_zone_reset(dd->fd, dd->nsid, i * dd->zone_size);
		} else {
			data_zones.push_back(DataRecord(
					{.l_address = 0, .p_address = i * dd->zone_size, .base_address = j * device->tparams.zns_zone_capacity, .flag = status_flag::Valid}));
			ss_zns_device_zone_reset(dd->fd, dd->nsid, i * dd->zone_size);
			j++;
		}
	}
}

void DataMap::add_or_update(const uint64_t idx, const ftl_record record) {
	PROFILE_FUNCTION();
	// auto guard = WriteGuard(log_zones[idx]);
	log_zones[idx].logs.push_back(record);
	log_zones[idx].zone_size += 1;
}

uint64_t DataMap::get_free_log_zone(int &index) {
	PROFILE_FUNCTION();
	for (uint i = 0; i < log_zones.size(); i++) {
		// auto guard = ReadGuard(log_zones[i]);
		if (log_zones[i].zone_size != dd->zone_size) {
			index = i;
			return log_zones[i].zone_size;
		}
	}
	index = -1;
	return 0;
}

std::vector<struct count_base_address_sums> LogRecord::count_base_addresses() {
	PROFILE_FUNCTION();
	// auto guard = ReadGuard(*this);
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

std::vector<ftl_record> LogRecord::remove_base_address(const uint64_t base_address) {
	PROFILE_FUNCTION();
	// auto guard = WriteGuard(*this);
	std::vector<ftl_record> removing;
	for (uint64_t i = 0; i < logs.size(); i++) {
		if (logs[i].base_address == base_address) {
			removing.push_back(logs[i]);
			logs.erase(logs.begin() + i);
			i--;
		}
	}
	return removing;
}

bool DataMap::get_data_zone(const uint64_t l_address, ftl_record &record, uint64_t &idx) {
	PROFILE_FUNCTION();
	for (uint64_t i = 0; i < data_zones.size(); i++) {
		// auto guard = ReadGuard(data_zones[i]);
		if (data_zones[i].record.base_address == l_address && data_zones[i].record.flag == status_flag::Valid) {
			record = data_zones[i].record;
			idx = i;
			return true;
		}
	}

	for (uint64_t i = 0; i < data_zones.size(); i++) {
		// auto guard = WriteGuard(data_zones[i]);
		if (data_zones[i].record.flag == status_flag::Invalid) {
			data_zones[i].record.flag = status_flag::Valid;
			record = data_zones[i].record;
			idx = i;
			return true;
		}
	}
	return false;
}

bool DataMap::remove_unused_data_zone(const uint64_t base_address, ftl_record &record) {
	PROFILE_FUNCTION();
	for (uint64_t i = 0; i < records.data_zones.size(); i++) {
		if (records.data_zones[i].record.base_address == base_address) {
			record = records.data_zones[i].record;
			records.data_zones.erase(records.data_zones.begin() + i);
			return true;
		}
	}
	return false;
}

bool DataMap::find_in_log(const uint64_t address, ftl_record &record) {
	PROFILE_FUNCTION();
	for (auto &t : log_zones) {
		// auto guard = ReadGuard(t);
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
	PROFILE_FUNCTION();
	const auto base_address = closest_lower_multiple(device->tparams.zns_zone_capacity, address);
	for (auto &t : data_zones) {
		// auto guard = ReadGuard(t);
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
	PROFILE_FUNCTION();
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

uint64_t DataMap::get_num_free_log_zones() {
	PROFILE_FUNCTION();
	uint64_t num_free_zones = 0;
	for (uint i = 0; i < records.log_zones.size(); i++) {
		if (records.log_zones[i].zone_size != records.dd->zone_size) {
			num_free_zones++;
		}
	}

	return num_free_zones;
}