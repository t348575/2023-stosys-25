#include <memory>

#include "S2FileSystem.h"

namespace rdb = ROCKSDB_NAMESPACE;

int main() {
    rdb::S2FileSystem *z = new rdb::S2FileSystem("s2fs:nvme0n1:///tmp/testdb/", true);
    std::unique_ptr<rdb::FSSequentialFile> file;
    z->NewSequentialFile("/asd", rdb::FileOptions(), &file, nullptr);
    return 0;
}