#include <algorithm>
#include <cstdint>

#include "ftl.h"

DataMap::DataMap(const user_zns_device *_device, const device_data *_dd) : device(_device), dd(_dd) {
	log_zones.reserve(device->tparams.zns_num_zones);
	for (uint32_t i = 0; i < device->tparams.zns_num_zones; i++) {
		if (i < dd->log_zones) {
			log_zones.push_back(LogRecord(i * dd->zone_size, dd->zone_size, dd));
		} else {
			data_zones.push_back(DataRecord(
					{.l_address = 0, .p_address = i * dd->zone_size, .base_address = 0, .flag = status_flag::Invalid}));
		}
	}
}

void DataMap::add_or_update(const uint64_t idx, const ftl_record record) {
	auto guard = WriteGuard(log_zones[idx]);
	log_zones[idx].logs.push_back(record);
}

uint64_t DataMap::get_free_log_zone(int &index) {
	for (uint i = 0; i < log_zones.size(); i++) {
		auto guard = ReadGuard(log_zones[i]);
		if (log_zones[i].logs.size() != dd->zone_size) {
			index = i;
			return log_zones[i].logs.size();
		}
	}
	return 0;
}

uint64_t DataMap::get_log_p_address(uint64_t idx) {
	auto guard = ReadGuard(log_zones[idx]);
	return log_zones[idx].get_p_address();
}

std::vector<struct sums> LogRecord::count_base_addresses() {
	auto guard = ReadGuard(*this);
	std::vector<struct sums> sums;
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
	std::sort(sums.rbegin(), sums.rend(), [](const struct sums &a, const struct sums &b) { return a.count < b.count; });
	return sums;
}

std::vector<ftl_record> LogRecord::remove_base_address(const uint64_t base_address) {
	auto guard = WriteGuard(*this);
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

bool DataMap::get_data_zone(const uint64_t l_address, ftl_record &record, bool &reset_zone, uint64_t &idx) {
	reset_zone = false;
	for (uint64_t i = 0; i < data_zones.size(); i++) {
		auto guard = ReadGuard(data_zones[i]);
		if (data_zones[i].record.base_address == l_address && data_zones[i].record.flag == status_flag::Valid) {
			record = data_zones[i].record;
			idx = i;
			return true;
		}
	}

	for (uint64_t i = 0; i < data_zones.size(); i++) {
		auto guard = WriteGuard(data_zones[i]);
		if (data_zones[i].record.flag == status_flag::Invalid) {
			data_zones[i].record.flag = status_flag::Valid;
			record = data_zones[i].record;
			reset_zone = true;
			idx = i;
			return true;
		}
	}
	return false;
}