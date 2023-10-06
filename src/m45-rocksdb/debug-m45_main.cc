#include <memory>
#include <iostream>

#include "S2FileSystem.h"

namespace rdb = ROCKSDB_NAMESPACE;

int main() {
    rdb::S2FileSystem z = rdb::S2FileSystem("s2fs:nvme0n1:///tmp/testdb/", true);
    std::unique_ptr<rdb::FSSequentialFile> file;
    z.CreateDirIfMissing("/a", rdb::IOOptions(), nullptr);
    z.CreateDir("/a", rdb::IOOptions(), nullptr);
    rdb::INode n;
    z.ReadINode({.block=0,.offset=0}, n);
    z.CreateDirIfMissing("/b", rdb::IOOptions(), nullptr);
    z.CreateDirIfMissing("/a/b", rdb::IOOptions(), nullptr);
    z.NewSequentialFile("/a/c", rdb::FileOptions(), &file, nullptr);

    z.RenameFile("/a/c", "/a/d", rdb::IOOptions(), nullptr);

    std::vector<std::string> dirs;
    z.GetChildren("/a", rdb::IOOptions(), &dirs, nullptr);
    std::cout << "Children of /a are: " << std::endl;
    for (const auto &t: dirs) {
        std::cout << "/a/" << t << std::endl;
    }

    std::unique_ptr<rdb::FSWritableFile> wr_file;
    if (!z.NewWritableFile("/a/c", rdb::FileOptions(), &wr_file, nullptr).ok()) {
        std::cout << "Could not create writeable file" << std::endl;
    }

    auto const num_action = 58344;

    std::vector<uint8_t> buffer;
    for (uint32_t i = 0; i < num_action; i++) {
        buffer.push_back(i % 0xFF);
    }

    auto s = rdb::Slice((char*)&buffer[0], num_action);
    wr_file->Append(s, rdb::IOOptions(), nullptr);
    wr_file->Close(rdb::IOOptions(), nullptr);

    std::vector<char> temp_buffer;
    temp_buffer.resize(num_action);
    z.NewSequentialFile("/a/c", rdb::FileOptions(), &file, nullptr);

    rdb::Slice slice;
    if (!file->Read(num_action, rdb::IOOptions(), &slice, &temp_buffer[0], nullptr).ok()) {
        std::cout << "Could not read" << std::endl;
    }

    for (uint32_t i = 0; i < num_action; i++) {
        if ((uint8_t)*(slice.data() + i) != (uint8_t)(i % 0xFF)) {
            std::cout << "Not matching " << std::to_string(i) << " " << std::to_string((uint8_t)*(slice.data() + i)) << " " << std::to_string(i % 0xFF) << std::endl;
        }
    }

    std::unique_ptr<rdb::FSRandomAccessFile> r_file;
    z.NewRandomAccessFile("/a/c", rdb::FileOptions(), &r_file, nullptr);
    r_file->Read(255, 128, rdb::IOOptions(), &slice, &temp_buffer[0], nullptr);
    for (uint32_t i = 0; i < 128; i++) {
        if ((uint8_t)*(slice.data() + i) != (uint8_t)i) {
            std::cout << "Not matching " << std::to_string(i) << " " << std::to_string((uint8_t)*(slice.data() + i)) << " " << std::to_string(i) << std::endl;
        }
    }

    z.DeleteDir("/a", rdb::IOOptions(), nullptr);
    dirs.clear();
    z.GetChildren("/", rdb::IOOptions(), &dirs, nullptr);
    std::cout << "Children of / are: " << std::endl;
    for (const auto &t: dirs) {
        std::cout << "/" << t << std::endl;
    }

    std::cout << "All done!" << std::endl;

    return 0;
}