#include <cstdint>

#include "ftl.h"

DataMap::DataMap(const user_zns_device *_device, const device_data *_dd) : device(_device), dd(_dd) {
	data.reserve(device->tparams.zns_num_zones);
	for (uint32_t i = 0; i < device->tparams.zns_num_zones; i++) {
		if (i < dd->log_zones) {
			data.push_back(LogRecord(device->tparams.zns_zone_capacity * i, dd->zone_size, false));
		} else {
			data.push_back(LogRecord(device->tparams.zns_zone_capacity * i, dd->zone_size, true));
		}
	}
}

bool DataMap::find(const uint64_t address, ftl_record &record) {
	const auto b = closest_lower_multiple(device->tparams.zns_zone_capacity, address);
	for (auto &t : data) {
		if (t.base_address == b) {
			auto guard = ReadGuard(t);
			for (const auto &inner : t.logs) {
				if (inner.l_address == address) {
					record = inner;
					return true;
				}
			}
		}
	}
	return false;
}

void DataMap::add_or_update(const ftl_record record) {
	const auto b = closest_lower_multiple(device->tparams.zns_zone_capacity, record.l_address);
	for (auto &t : data) {
		if (t.base_address == b) {
			auto guard = WriteGuard(t);
			for (auto &inner : t.logs) {
				if (inner.l_address == record.l_address) {
					inner = record;
					return;
				}
			}

			t.logs.push_back(record);
		}
	}
}