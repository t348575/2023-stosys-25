#include <cstdint>
#include <iostream>

#include "ftl.h"
#include "visual_profiler.h"

DataMap *DataMap::records = nullptr;

inline uint64_t closest_lower_multiple(uint64_t n, uint64_t x) { return x - (x % n); }

inline int ss_zns_device_zone_reset(int fd, uint32_t nsid, uint64_t slba) {
	PROFILE_FUNCTION();
	return nvme_zns_mgmt_send(fd, nsid, slba, false, NVME_ZNS_ZSA_RESET, 0, nullptr);
}

void DataMap::init(const user_zns_device *_device, const device_data *_dd) {
	if (inited) {
		return;
	}

	device = _device;
	dd = _dd;
	PROFILE_FUNCTION();
	log_zones.reserve(device->tparams.zns_num_zones);
	uint32_t i = 0;
	for (; i < dd->log_zones / 2; i++) {
		log_zones.push_back(LogRecord(i * dd->zone_size, dd->zone_size, false));
		ss_zns_device_zone_reset(dd->fd, dd->nsid, i * dd->zone_size);
	}
	for (; i < dd->log_zones; i++) {
		log_zones.push_back(LogRecord(i * dd->zone_size, dd->zone_size, true));
		ss_zns_device_zone_reset(dd->fd, dd->nsid, i * dd->zone_size);
	}
	for (uint32_t j = 0; i < device->tparams.zns_num_zones; i++, j++) {
		data_zones.push_back(DataRecord({.l_address = 0,
										 .p_address = i * dd->zone_size,
										 .base_address = j * device->tparams.zns_zone_capacity,
										 .flag = status_flag::Invalid}));
		ss_zns_device_zone_reset(dd->fd, dd->nsid, i * dd->zone_size);
	}
	inited = true;
}

void DataMap::add_or_update(const uint64_t idx, const ftl_record record) {
	PROFILE_FUNCTION();
	log_zones[idx].logs.push_back(record);
	log_zones[idx].zone_size++;
}

uint64_t DataMap::get_free_log_zone(int &index) {
	PROFILE_FUNCTION();
	auto const size = log_zones.size();
	for (uint64_t i = 0; i < size; i++) {
		if (log_zones[i].zone_size != dd->zone_size) {
			index = i;
			return log_zones[i].zone_size;
		}
	}
	index = -1;
	return 0;
}

uint64_t DataMap::count_free_log_pages() {
	PROFILE_FUNCTION();
	uint64_t count = 0;
	for (const auto &t : log_zones) {
		count += dd->zone_size - t.zone_size;
	}
	return count;
}

std::vector<struct count_base_address_sums> DataMap::count_base_addresses() {
	PROFILE_FUNCTION();
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
	PROFILE_FUNCTION();
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
	PROFILE_FUNCTION();
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
	PROFILE_FUNCTION();
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
	PROFILE_FUNCTION();
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
	PROFILE_FUNCTION();
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
	PROFILE_FUNCTION();
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