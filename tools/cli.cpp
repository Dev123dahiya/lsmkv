// A small command line over the database, so it can be poked at without
// writing a program.
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include "lsmkv/db.h"

namespace {

void Usage() {
  std::fprintf(stderr,
               "usage: lsmkv_cli <db-path> <command> [args]\n"
               "\n"
               "  put <key> <value>      store a value\n"
               "  get <key>              print a value, exit 1 if absent\n"
               "  delete <key>           remove a key\n"
               "  scan [from] [to]       print key=value in order\n"
               "  compact                merge everything down\n"
               "  stats                  per-level files and counters\n"
               "  load                   read 'key<TAB>value' lines from stdin\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    Usage();
    return 2;
  }
  const std::string path = argv[1];
  const std::string command = argv[2];

  lsmkv::Options options;
  std::unique_ptr<lsmkv::DB> db;
  const lsmkv::Status open = lsmkv::DB::Open(options, path, &db);
  if (!open.ok()) {
    std::fprintf(stderr, "%s\n", open.ToString().c_str());
    return 1;
  }

  if (command == "put" && argc == 5) {
    const lsmkv::Status s = db->Put(lsmkv::WriteOptions(), argv[3], argv[4]);
    if (!s.ok()) {
      std::fprintf(stderr, "%s\n", s.ToString().c_str());
      return 1;
    }
    return 0;
  }

  if (command == "get" && argc == 4) {
    std::string value;
    const lsmkv::Status s = db->Get(lsmkv::ReadOptions(), argv[3], &value);
    if (s.IsNotFound()) return 1;
    if (!s.ok()) {
      std::fprintf(stderr, "%s\n", s.ToString().c_str());
      return 1;
    }
    std::fwrite(value.data(), 1, value.size(), stdout);
    std::fputc('\n', stdout);
    return 0;
  }

  if (command == "delete" && argc == 4) {
    const lsmkv::Status s = db->Delete(lsmkv::WriteOptions(), argv[3]);
    return s.ok() ? 0 : 1;
  }

  if (command == "scan") {
    const std::string from = argc > 3 ? argv[3] : "";
    const std::string to = argc > 4 ? argv[4] : "";
    auto iter = db->NewIterator(lsmkv::ReadOptions());
    if (from.empty()) iter->SeekToFirst(); else iter->Seek(from);
    for (; iter->Valid(); iter->Next()) {
      if (!to.empty() && iter->key() > to) break;
      std::printf("%.*s=%.*s\n", static_cast<int>(iter->key().size()), iter->key().data(),
                  static_cast<int>(iter->value().size()), iter->value().data());
    }
    if (!iter->status().ok()) {
      std::fprintf(stderr, "%s\n", iter->status().ToString().c_str());
      return 1;
    }
    return 0;
  }

  if (command == "compact") {
    const lsmkv::Status s = db->CompactRange("", "");
    if (!s.ok()) {
      std::fprintf(stderr, "%s\n", s.ToString().c_str());
      return 1;
    }
    return 0;
  }

  if (command == "stats") {
    std::printf("%s", db->Stats().c_str());
    return 0;
  }

  if (command == "load") {
    std::string line;
    uint64_t n = 0;
    lsmkv::WriteBatch batch;
    while (std::getline(std::cin, line)) {
      const size_t tab = line.find('\t');
      if (tab == std::string::npos) continue;
      batch.Put(line.substr(0, tab), line.substr(tab + 1));
      if (++n % 1000 == 0) {
        db->Write(lsmkv::WriteOptions(), &batch);
        batch.Clear();
      }
    }
    if (batch.Count() > 0) db->Write(lsmkv::WriteOptions(), &batch);
    std::printf("loaded %llu keys\n", static_cast<unsigned long long>(n));
    return 0;
  }

  Usage();
  return 2;
}
