#include <algorithm>

#include "ftl.h"
#include "zns_device.h"

template class FtlTable<ftl_record, int>;
template class FtlTable<std::vector<ftl_record>, ftl_record>;

typedef FtlTable<std::vector<ftl_record>, ftl_record> LogRecord;

template <class T, class S> void FtlTable<T, S>::reset() {
	auto guard = WriteGuard(*this);
	table.clear();
}

template <> bool FtlTable<ftl_record, int>::has(uint64_t address) {
	auto guard = ReadGuard(*this);
	for (const auto &t : table) {
		if (t.l_address == address && t.flag == status_flag::Valid) {
			return true;
		}
	}

	return false;
}

template <> uint64_t FtlTable<ftl_record, int>::update_write_ptr(int index) {
	auto guard = WriteGuard(*this);
	table[index].write_ptr++;
	return table[index].write_ptr;
}

template <> void FtlTable<ftl_record, int>::reset_write_ptr(uint index) {
	auto guard = WriteGuard(*this);
	table[index].write_ptr = 0;
}

template <> bool LogRecord::has(uint64_t address) {
	auto guard = ReadGuard(*this);
	for (const auto &i : table) {
		for (const auto &j : i) {
			if (j.l_address == address && j.flag == status_flag::Valid) {
				return true;
			}
		}
	}

	return false;
}

template <class T, class S> void FtlTable<T, S>::reserve(uint64_t size) {
	auto guard = WriteGuard(*this);
	this->table.reserve(size);
}

template <class T, class S> void FtlTable<T, S>::push_back(T record) {
	auto guard = WriteGuard(*this);
	this->table.push_back(record);
}

template <> void LogRecord::push_back(uint index, ftl_record record) {
	auto guard = WriteGuard(*this);
	this->table[index].push_back(record);
}

template <> int FtlTable<ftl_record, int>::get_unused_block() {
	auto guard = WriteGuard(*this);
	for (uint i = 0; i < table.size(); i++) {
		if (table[i].flag == status_flag::Invalid) {
			table[i].flag = status_flag::Valid;
			return i;
		}
	}

	return -1;
}

template <> void FtlTable<ftl_record, int>::remove_index(int index) {
	auto guard = WriteGuard(*this);
	table.erase(table.begin() + index);
}

template <class T, class S> void FtlTable<T, S>::set(const uint index, const T record) {
	auto guard = WriteGuard(*this);
	table[index] = record;
}

template <>
int FtlTable<ftl_record, int>::does_base_address_exist(const uint64_t &address, const struct user_zns_device *my_dev) {
	auto guard = ReadGuard(*this);
	uint64_t l_address = base_l_address(address, my_dev->tparams.zns_zone_capacity);
	for (uint i = 0; i < table.size(); i++) {
		if (table[i].l_address == l_address && table[i].flag == status_flag::Valid) {
			return i;
		}
	}
	return -1;
}

template <class T, class S> T FtlTable<T, S>::get(const uint index) {
	auto guard = ReadGuard(*this);
	return table[index];
}

template <> uint32_t LogRecord::table_index_size(uint index) {
	auto guard = ReadGuard(*this);
	return table[index].size();
}

template <>
int LogRecord::get_record(const int index, const uint64_t l_address, ftl_record &result, uint &result_index) {
	auto guard = ReadGuard(*this);
	for (uint i = 0; i < table[index].size(); i++) {
		if (table[index][i].l_address == l_address) {
			result = table[index][i];
			result_index = i;
			return 0;
		}
	}
	return -1;
}

template <> void FtlTable<ftl_record, int>::set_invalid(int index) {
	auto guard = WriteGuard(*this);
	table[index].flag = status_flag::Invalid;
}

template <> void LogRecord::clear_index(int index) {
	auto guard = WriteGuard(*this);
	table[index].clear();
}

template <> int64_t LogRecord::insertion_pos(uint index, uint64_t l_address) {
	auto guard = ReadGuard(*this);
	for (int64_t i = table[index].size() - 1; i > -1; i--) {
		if (!(table[index][i].l_address > l_address)) {
			return i + 1;
		}
	}
	return 0;
}

template <> uint64_t LogRecord::size_at(uint index) {
	auto guard = ReadGuard(*this);
	return table[index].size();
}

template<>
void LogRecord::record_transaction(std::function<void(std::vector<std::vector<ftl_record>> &table)> f) {
	auto guard = WriteGuard(*this);
	f(table);
}