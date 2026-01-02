#include "io/real/real_disk.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using base::Slice;

class RealDiskTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("logengine_test_" + std::to_string(::getpid()) + "_" +
            std::to_string(counter_++));
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  [[nodiscard]] std::string path(const char* name) const {
    return (dir_ / name).string();
  }

  std::filesystem::path dir_;
  static int counter_;
};

int RealDiskTest::counter_ = 0;

TEST_F(RealDiskTest, WriteReadRoundTrip) {
  io::real::RealDisk disk;
  auto file = disk.open(path("a.log"), io::OpenMode::kCreate);
  ASSERT_TRUE(file.ok());

  const std::string data = "segment bytes";
  auto written = disk.pwrite(file.value(), Slice::from_string(data), 0);
  ASSERT_TRUE(written.ok());
  EXPECT_EQ(written.value(), data.size());

  std::vector<base::u8> out(data.size());
  auto got = disk.pread(file.value(), base::MutSlice(out.data(), out.size()), 0);
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value(), data.size());
  EXPECT_EQ(Slice(out.data(), out.size()), Slice::from_string(data));
}

TEST_F(RealDiskTest, PositionalWritesDoNotDisturbEachOther) {
  io::real::RealDisk disk;
  auto file = disk.open(path("b.log"), io::OpenMode::kCreate);
  ASSERT_TRUE(file.ok());

  ASSERT_TRUE(disk.pwrite(file.value(), Slice::from_string("BBBB"), 4).ok());
  ASSERT_TRUE(disk.pwrite(file.value(), Slice::from_string("AAAA"), 0).ok());

  std::vector<base::u8> out(8);
  auto got = disk.pread(file.value(), base::MutSlice(out.data(), out.size()), 0);
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(Slice(out.data(), 8), Slice::from_string("AAAABBBB"));
}

// A read past EOF is a short read, not an error. Recovery depends on this: the tail
// scan walks off the end of a torn segment and must treat that as the normal
// stopping condition rather than a failure (§16.2).
TEST_F(RealDiskTest, ShortReadAtEofIsNotAnError) {
  io::real::RealDisk disk;
  auto file = disk.open(path("c.log"), io::OpenMode::kCreate);
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE(disk.pwrite(file.value(), Slice::from_string("abc"), 0).ok());

  std::vector<base::u8> out(64);
  auto got = disk.pread(file.value(), base::MutSlice(out.data(), out.size()), 0);
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value(), 3u);

  auto past = disk.pread(file.value(), base::MutSlice(out.data(), out.size()), 100);
  ASSERT_TRUE(past.ok());
  EXPECT_EQ(past.value(), 0u);
}

TEST_F(RealDiskTest, SizeAndTruncate) {
  io::real::RealDisk disk;
  auto file = disk.open(path("d.log"), io::OpenMode::kCreate);
  ASSERT_TRUE(file.ok());

  ASSERT_TRUE(disk.pwrite(file.value(), Slice::from_string("0123456789"), 0).ok());
  auto size = disk.size(file.value());
  ASSERT_TRUE(size.ok());
  EXPECT_EQ(size.value(), 10u);

  ASSERT_TRUE(disk.truncate(file.value(), 4).ok());
  size = disk.size(file.value());
  ASSERT_TRUE(size.ok());
  EXPECT_EQ(size.value(), 4u);
}

TEST_F(RealDiskTest, FsyncSucceeds) {
  io::real::RealDisk disk;
  auto file = disk.open(path("e.log"), io::OpenMode::kCreate);
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE(disk.pwrite(file.value(), Slice::from_string("durable"), 0).ok());
  EXPECT_TRUE(disk.fsync(file.value()).ok());
}

TEST_F(RealDiskTest, DataSurvivesReopen) {
  const std::string p = path("f.log");
  {
    io::real::RealDisk disk;
    auto file = disk.open(p, io::OpenMode::kCreate);
    ASSERT_TRUE(file.ok());
    ASSERT_TRUE(disk.pwrite(file.value(), Slice::from_string("persisted"), 0).ok());
    ASSERT_TRUE(disk.fsync(file.value()).ok());
    disk.close(file.value());
  }

  io::real::RealDisk disk;
  auto file = disk.open(p, io::OpenMode::kRead);
  ASSERT_TRUE(file.ok());
  std::vector<base::u8> out(9);
  auto got = disk.pread(file.value(), base::MutSlice(out.data(), out.size()), 0);
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(Slice(out.data(), got.value()), Slice::from_string("persisted"));
}

TEST_F(RealDiskTest, OpenMissingFileIsNotFound) {
  io::real::RealDisk disk;
  auto file = disk.open(path("nope.log"), io::OpenMode::kRead);
  ASSERT_FALSE(file.ok());
  EXPECT_EQ(file.error(), base::ErrorCode::kNotFound);
}

TEST_F(RealDiskTest, OperationsOnClosedFileFail) {
  io::real::RealDisk disk;
  auto file = disk.open(path("g.log"), io::OpenMode::kCreate);
  ASSERT_TRUE(file.ok());
  disk.close(file.value());

  auto written = disk.pwrite(file.value(), Slice::from_string("x"), 0);
  ASSERT_FALSE(written.ok());
  EXPECT_EQ(written.error(), base::ErrorCode::kNotFound);
}

TEST_F(RealDiskTest, MakeDirectoriesIsRecursiveAndIdempotent) {
  io::real::RealDisk disk;
  const std::string nested = (dir_ / "topic-0" / "sub").string();
  EXPECT_TRUE(disk.make_directories(nested).ok());
  EXPECT_TRUE(disk.make_directories(nested).ok());
  EXPECT_TRUE(std::filesystem::exists(nested));
}

}  // namespace
