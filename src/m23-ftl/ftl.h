#include <functional>
#include <shared_mutex>
#include <vector>

#ifndef FTL_H
#define FTL_H

enum status_flag { Valid = 0, Invalid = 2 };

struct ftl_record {
	uint64_t l_address, p_address, write_ptr;
	status_flag flag;
};

// varieties:
// template class FtlTable<ftl_record, int>;
// template class FtlTable<std::vector<ftl_record>, ftl_record>;

template <class T, class S> class FtlTable {
public:
	// both
	T get(const uint index);
	void set(const uint index, const T record);
	void reset();
	void push_back(const T record);
	void record_transaction(const std::function<void(std::vector<T> &table)> f);

	// FtlTable<ftl_record, int>
	uint64_t increment_write_ptr(const int index, const uint64_t value);
	void reset_write_ptr(const uint index);
	void set_flag(int index, status_flag flag);
	int get_unused_block();
	int does_address_exist(const uint64_t l_address, ftl_record *record);

	// FtlTable<std::vector<ftl_record>, ftl_record>
	bool has(const uint64_t address);
	uint64_t size_at(const uint index);
	void push_back(const uint index, const S record);
	void clear_index(const int index);
	int64_t insertion_pos(const uint index, const uint64_t l_address);
	uint32_t table_index_size(uint index);
	int get_record(const int index, const uint64_t l_address, S &result, uint &result_index);

private:
	class WriteGuard {
	public:
		WriteGuard(FtlTable &ftl) : ftl(ftl) { ftl.rw_lock.lock(); };
		~WriteGuard() { ftl.rw_lock.unlock(); }

	private:
		FtlTable &ftl;
	};

	class ReadGuard {
	public:
		ReadGuard(FtlTable &ftl) : ftl(ftl) { ftl.rw_lock.lock_shared(); };
		~ReadGuard() { ftl.rw_lock.unlock_shared(); }

	private:
		FtlTable &ftl;
	};

	std::vector<T> table;
	std::shared_mutex rw_lock;
};

#endif