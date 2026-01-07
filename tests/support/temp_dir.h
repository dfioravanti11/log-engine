#pragma once

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <string>

#include "storage/record_batch.h"

// Test-only helpers. Tests are not subject to ER-1 — they are allowed to reach for
// std::filesystem to set up and inspect the very files the code under test may only
// touch through io::Disk.
namespace testsupport {

class TempDirTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("logengine_storage_" + std::to_string(::getpid()) + "_" + std::to_string(counter_++));
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  [[nodiscard]] std::string dir() const { return dir_.string(); }
  [[nodiscard]] std::string path(const std::string& name) const { return (dir_ / name).string(); }

  std::filesystem::path dir_;
  static inline int counter_ = 0;
};

// Deterministic payload for record i, so a verifier can recompute what it should see
// rather than remember it.
inline std::string record_payload(base::u64 i, std::size_t size = 32) {
  std::string s = "record-" + std::to_string(i) + "-";
  s.resize(size, '.');
  return s;
}

}  // namespace testsupport
