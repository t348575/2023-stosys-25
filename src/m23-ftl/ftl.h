#include <shared_mutex>
#include <vector>

enum status_flag { Valid = 0, Erased = 1, Invalid = 2 };

struct ftl_info {
	uint64_t l_address;
	uint64_t p_address;
	status_flag flag;
};

class FtlTable {
public:
	void reset();
	bool push_back(ftl_info info);
	bool has(uint64_t index);
	void reserve(uint64_t size);
	void set_zone_info(uint32_t zone_size) {
		zone_size = zone_size;
		zone_idx = 0;
	}
	void reset_zone_info() { zone_idx = 0; }
    bool read(uint64_t address, char *buffer);

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

	std::vector<std::pair<ftl_info, char[4096]>> table;
	uint32_t zone_idx;
	uint32_t zone_size;
	std::shared_mutex rw_lock;
};