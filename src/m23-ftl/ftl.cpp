#include "ftl.h"

void FtlTable::reset() {
	auto guard = WriteGuard(*this);
	table.clear();
}

bool FtlTable::has(uint64_t index) {
	auto guard = ReadGuard(*this);
	for (const auto &t : table) {
		if (t.l_address == index) {
			return true;
		}
	}

	return false;
}

void FtlTable::reserve(uint64_t size) {
	auto guard = WriteGuard(*this);
	this->table.reserve(size);
}

bool FtlTable::push_back(ftl_info info) {
	auto guard = WriteGuard(*this);
	this->table.push_back(info);
	zone_idx += 1;
	return zone_idx + 1 == zone_size;
}

bool FtlTable::read(uint64_t address, char *buffer) {
    auto guard = WriteGuard(*this);
    for (const auto &t: this->table) {
        if (t.l_address == address) {
            
        }
    }
}