#include <memory>
#include <iostream>
#include <fstream>

#include "S2FileSystem.h"

namespace rdb = ROCKSDB_NAMESPACE;

std::vector<uint8_t> read_file(const std::string name) {
    std::ifstream file(name, std::ios::binary | std::ios::ate);
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    file.read((char*)buffer.data(), size);
    return buffer;
}

int main(int argc, char **argv) {
    rdb::S2FileSystem z = rdb::S2FileSystem("s2fs:nvme0n1:///tmp/testdb/", true);

    std::vector<std::string> dirs;
    z.GetChildren("/tmp/testdb", rdb::IOOptions(), &dirs, nullptr);
    std::cout << "Children of /tmp/testdb are: " << std::endl;
    for (const auto &t: dirs) {
        uint64_t file_size;
        z.GetFileSize("/tmp/testdb/" + t, rdb::IOOptions(), &file_size, nullptr);
        std::cout << "/tmp/testdb/" << t << " with size " << std::to_string(file_size) << std::endl;
    }

    // auto actual = read_file("run_1/MANIFEST-000004");
    // std::unique_ptr<rdb::FSSequentialFile> s2fs_manifest;
    // if (!z.NewSequentialFile("/tmp/testdb/MANIFEST-000004", rdb::FileOptions(), &s2fs_manifest, nullptr).ok()) {
    //     std::cerr << "Could not open s2fs manifest" << std::endl;
    // }

    // std::vector<uint8_t> scratch;
    // scratch.resize(32768);
    // rdb::Slice slice;
    // if (!s2fs_manifest->Read(32768, rdb::IOOptions(), &slice, (char*)&scratch[0], nullptr).ok()) {
    //     std::cerr << "Could not read from s2fs manifest" << std::endl;
    // }

    // if (slice.size() != actual.size()) {
    //     std::cerr << "Files dont match " << slice.size() << " " << actual.size() << std::endl;
    // }

    // for (uint64_t i = 0; i < slice.size(); i++) {
    //     if (scratch[i] != actual[i]) {
    //         std::cout << "Not matching " << std::to_string(i) << " " << scratch[i] << " " << actual[i] << std::endl;
    //     }
    // }

    // std::unique_ptr<rdb::FSSequentialFile> file;
    // if (!z.NewSequentialFile("/tmp/testdb/000005.log", rdb::FileOptions(), &file, nullptr).ok()) {
    //     std::cerr << "Could not open 000005.log" << std::endl;
    //     return -1;
    // }

    // std::vector<uint8_t> actual_file = read_file("run_1/000005.log");
    // std::vector<uint8_t> s2fs_file;
    // rdb::Slice slice;
    // do {
    //     std::vector<uint8_t> temp_buffer;
    //     temp_buffer.resize(4096);
    //     if (!file->Read(4096, rdb::IOOptions(), &slice, (char*)&temp_buffer[0], nullptr).ok()) {
    //         std::cerr << "Could not read" << std::endl;
    //     }
    //     s2fs_file.insert(s2fs_file.end(), slice.data(), slice.data() + slice.size());
    // } while(slice.size() == 4096);

    // if (s2fs_file.size() != actual_file.size()) {
    //     std::cerr << "Files dont match " << s2fs_file.size() << " " << actual_file.size() << std::endl;
    // }

    // for (uint64_t i = 0; i < s2fs_file.size(); i++) {
    //     if (s2fs_file[i] != actual_file[i]) {
    //         std::cout << "Not matching " << std::to_string(i) << " " << s2fs_file[i] << " " << actual_file[i] << std::endl;
    //     }
    // }
    // if (argc == 2) {
    //     z.DeleteDir("/a", rdb::IOOptions(), nullptr);
    //     return 0;
    // }

    // if (argc == 3) {
    //     std::vector<std::string> dirs1;
    //     z.GetChildren("/a", rdb::IOOptions(), &dirs1, nullptr);
    //     std::cout << "Children of /a are: " << std::endl;
    //     for (const auto &t: dirs1) {
    //         std::cout << "/a/" << t << std::endl;
    //     }
    //     return 0;
    // }

    // if (argc == 4) {
    //     uint64_t f_size;
    //     z.GetFileSize("/a/c", rdb::IOOptions(), &f_size, nullptr);
    //     std::cout << "size: " << f_size << std::endl;
    //     return 0;
    // }

    // std::unique_ptr<rdb::FSSequentialFile> file;
    // z.CreateDirIfMissing("/a", rdb::IOOptions(), nullptr);
    // z.CreateDir("/a", rdb::IOOptions(), nullptr);
    // z.CreateDirIfMissing("/b", rdb::IOOptions(), nullptr);
    // z.CreateDirIfMissing("/a/b", rdb::IOOptions(), nullptr);
    // if (argc == 1) {
        // z.NewSequentialFile("/a/c", rdb::FileOptions(), &file, nullptr);

        // z.RenameFile("/a/c", "/a/d", rdb::IOOptions(), nullptr);

        // std::vector<std::string> dirs;
        // z.GetChildren("/a", rdb::IOOptions(), &dirs, nullptr);
        // std::cout << "Children of /a are: " << std::endl;
        // for (const auto &t: dirs) {
        //     std::cout << "/a/" << t << std::endl;
        // }
    // }

    // rdb::RichInode i, bi;
    // std::string bp;
    // rdb::S2FileSystem::FindInodeDirs ds;
    // z.FindInodeWithBaseWithDirs("/a", i, bi, bp, ds);
    // std::cout << "Children of / are: " << std::endl;
    // for (const auto &t: ds.dir_entries) {
    //     std::cout << "/" << t.name << std::endl;
    // }

    // auto const num_action = 188416;
    // // if (argc == 1) {
    //     std::unique_ptr<rdb::FSWritableFile> wr_file;
    //     if (!z.NewWritableFile("/a/c", rdb::FileOptions(), &wr_file, nullptr).ok()) {
    //         std::cout << "Could not create writeable file" << std::endl;
    //     }
        
    //     std::vector<uint8_t> buffer;
    //     for (uint32_t i = 0; i < num_action; i++) {
    //         buffer.push_back(i % 0xFF);
    //     }
    //     buffer.push_back('\n');

    //     auto s = rdb::Slice((char*)&buffer[0], num_action);
    //     wr_file->Append(s, rdb::IOOptions(), nullptr);
    //     wr_file->Close(rdb::IOOptions(), nullptr);
    // }
    // z.NewSequentialFile("/a/d", rdb::FileOptions(), &file, nullptr);
    // z.RenameFile("/a/c", "/a/d", rdb::IOOptions(), nullptr);
    //     std::vector<std::string> dirs1;
    //     z.GetChildren("/a", rdb::IOOptions(), &dirs1, nullptr);
    //     std::cout << "Children of /a are: " << std::endl;
    //     for (const auto &t: dirs1) {
    //         std::cout << "/a/" << t << std::endl;
    //     }
    //     dirs1.clear();
    //     z.GetChildren("/tmp/testdb", rdb::IOOptions(), &dirs1, nullptr);
    //     std::cout << "Children of / are: " << std::endl;
    //     for (const auto &t: dirs1) {
    //         std::cout << "/" << t << std::endl;
    //     }
    // std::vector<char> temp_buffer;
    // temp_buffer.resize(num_action);
    // z.NewSequentialFile("/a/c", rdb::FileOptions(), &file, nullptr);

    // rdb::Slice slice;
    // if (!file->Read(num_action, rdb::IOOptions(), &slice, &temp_buffer[0], nullptr).ok()) {
    //     std::cout << "Could not read" << std::endl;
    // }

    // for (uint32_t i = 0; i < num_action; i++) {
    //     if ((uint8_t)*(slice.data() + i) != (uint8_t)(i % 0xFF)) {
    //         std::cout << "Not matching " << std::to_string(i) << " " << std::to_string((uint8_t)*(slice.data() + i)) << " " << std::to_string(i % 0xFF) << std::endl;
    //     }
    // }

    // std::unique_ptr<rdb::FSRandomAccessFile> r_file;
    // z.NewRandomAccessFile("/a/c", rdb::FileOptions(), &r_file, nullptr);
    // r_file->Read(255, 128, rdb::IOOptions(), &slice, &temp_buffer[0], nullptr);
    // for (uint32_t i = 0; i < 128; i++) {
    //     if ((uint8_t)*(slice.data() + i) != (uint8_t)i) {
    //         std::cout << "Not matching " << std::to_string(i) << " " << std::to_string((uint8_t)*(slice.data() + i)) << " " << std::to_string(i) << std::endl;
    //     }
    // }

    // std::cout << "All done!" << std::endl;

    return 0;
}