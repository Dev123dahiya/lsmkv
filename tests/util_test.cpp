#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "harness.h"
#include "util/arena.h"
#include "util/bloom.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace lsmkv {
namespace {

std::string Key(int i) { return "key" + std::to_string(i); }

}  // namespace

// The published CRC-32C check values (RFC 3720 appendix B). These pin the
// implementation to the standard rather than to itself.
TEST(Crc32c, StandardVectors) {
  std::string zeros(32, '\x00');
  CHECK_EQ(crc32c::Value(zeros), 0x8a9136aau);

  std::string ones(32, '\xff');
  CHECK_EQ(crc32c::Value(ones), 0x62a8ab43u);

  std::string up(32, '\0');
  for (int i = 0; i < 32; ++i) up[static_cast<size_t>(i)] = static_cast<char>(i);
  CHECK_EQ(crc32c::Value(up), 0x46dd794eu);

  std::string down(32, '\0');
  for (int i = 0; i < 32; ++i) down[static_cast<size_t>(i)] = static_cast<char>(31 - i);
  CHECK_EQ(crc32c::Value(down), 0x113fdb5cu);

  CHECK_EQ(crc32c::Value(std::string_view("123456789")), 0xe3069283u);
}

// The SSE4.2 path and the table path must agree on every length and
// alignment, including the ones where the hardware loop switches between
// byte, word and tail handling.
TEST(Crc32c, HardwareMatchesPortable) {
  std::mt19937 rng(1234);
  std::string buf(1024, '\0');
  for (char& c : buf) c = static_cast<char>(rng());

  for (size_t len = 0; len <= 300; ++len) {
    for (size_t off = 0; off < 9; ++off) {
      const uint32_t a = crc32c::Extend(0, buf.data() + off, len);
      const uint32_t b = crc32c::ExtendPortable(0, buf.data() + off, len);
      CHECK_EQ(a, b);
    }
  }
  std::printf("       (hardware CRC32C in use: %s)\n",
              crc32c::HardwareAccelerated() ? "yes" : "no");
}

// Checksumming in pieces must equal checksumming in one go, which is what
// lets the log writer checksum a header and payload separately.
TEST(Crc32c, ExtendIsIncremental) {
  const std::string data = "the quick brown fox jumps over the lazy dog, repeatedly and at length";
  const uint32_t whole = crc32c::Value(data);
  for (size_t split = 0; split <= data.size(); ++split) {
    const uint32_t part = crc32c::Extend(0, data.data(), split);
    const uint32_t both = crc32c::Extend(part, data.data() + split, data.size() - split);
    CHECK_EQ(both, whole);
  }
}

TEST(Crc32c, MaskRoundTrips) {
  for (uint32_t v : {0u, 1u, 0xffffffffu, 0x12345678u, 0xa282ead8u}) {
    CHECK_EQ(crc32c::Unmask(crc32c::Mask(v)), v);
    // The mask exists so that a zeroed region does not look like a valid
    // checksum of itself.
    CHECK_NE(crc32c::Mask(v), v);
  }
}

TEST(Coding, FixedRoundTrip) {
  std::string s;
  PutFixed32(&s, 0x12345678u);
  PutFixed64(&s, 0x0123456789abcdefull);
  CHECK_EQ(s.size(), 12u);
  CHECK_EQ(DecodeFixed32(s.data()), 0x12345678u);
  CHECK_EQ(DecodeFixed64(s.data() + 4), 0x0123456789abcdefull);
  // Little-endian on disk, whatever the host would have chosen.
  CHECK_EQ(static_cast<uint8_t>(s[0]), 0x78u);
  CHECK_EQ(static_cast<uint8_t>(s[3]), 0x12u);
}

TEST(Coding, VarintRoundTrip) {
  std::string s;
  std::vector<uint64_t> values;
  for (uint64_t v = 0; v < 128; ++v) values.push_back(v);
  for (int shift = 7; shift < 64; ++shift) {
    const uint64_t base = 1ull << shift;
    values.push_back(base - 1);
    values.push_back(base);
    values.push_back(base + 1);
  }
  values.push_back(UINT64_MAX);
  for (uint64_t v : values) PutVarint64(&s, v);

  std::string_view in(s);
  for (uint64_t expected : values) {
    uint64_t got = 0;
    CHECK(GetVarint64(&in, &got));
    CHECK_EQ(got, expected);
  }
  CHECK(in.empty());
}

TEST(Coding, VarintLengthMatchesEncoding) {
  for (uint64_t v : {uint64_t{0}, uint64_t{1}, uint64_t{127}, uint64_t{128}, uint64_t{16383},
                     uint64_t{16384}, UINT64_MAX}) {
    std::string s;
    PutVarint64(&s, v);
    CHECK_EQ(static_cast<int>(s.size()), VarintLength(v));
  }
}

// A truncated buffer must be reported, not read past. The decoder leaves the
// input untouched on failure so a caller can retry with more bytes.
TEST(Coding, VarintRejectsTruncatedInput) {
  std::string s;
  PutVarint64(&s, 0xdeadbeefull);
  for (size_t cut = 0; cut < s.size(); ++cut) {
    std::string_view in(s.data(), cut);
    uint64_t got = 0;
    CHECK(!GetVarint64(&in, &got));
    CHECK_EQ(in.size(), cut);
  }
  std::string_view full(s);
  uint64_t got = 0;
  CHECK(GetVarint64(&full, &got));
  CHECK_EQ(got, 0xdeadbeefull);
}

TEST(Coding, Varint32RejectsOversizedValue) {
  std::string s;
  PutVarint64(&s, 1ull << 40);
  std::string_view in(s);
  uint32_t got = 0;
  CHECK(!GetVarint32(&in, &got));
  CHECK_EQ(in.size(), s.size());  // unchanged on failure
}

TEST(Coding, LengthPrefixedRoundTrip) {
  std::string s;
  const std::vector<std::string> parts = {"", "a", "hello", std::string(1000, 'x')};
  for (const auto& p : parts) PutLengthPrefixed(&s, p);

  std::string_view in(s);
  for (const auto& expected : parts) {
    std::string_view got;
    CHECK(GetLengthPrefixed(&in, &got));
    CHECK_EQ(got, std::string_view(expected));
  }
  CHECK(in.empty());

  std::string_view truncated(s.data(), s.size() - 1);
  std::string_view got;
  CHECK(GetLengthPrefixed(&truncated, &got));  // first parts still decode
}

TEST(Arena, AllocationsDoNotOverlap) {
  Arena arena;
  std::mt19937 rng(7);
  std::vector<std::pair<char*, size_t>> blocks;
  for (int i = 0; i < 2000; ++i) {
    const size_t n = (i % 61 == 0) ? rng() % 6000 : rng() % 40 + 1;
    char* p = arena.Allocate(n);
    CHECK(p != nullptr);
    std::memset(p, i & 0xff, n);  // ASan catches an overlapping range here
    blocks.emplace_back(p, n);
  }
  for (size_t i = 0; i < blocks.size(); ++i) {
    const auto [p, n] = blocks[i];
    for (size_t j = 0; j < n; ++j) CHECK_EQ(static_cast<uint8_t>(p[j]), (i & 0xff));
  }
  CHECK(arena.MemoryUsage() > 0);
}

TEST(Arena, AlignedAllocationsAreAligned) {
  Arena arena;
  std::mt19937 rng(11);
  for (int i = 0; i < 500; ++i) {
    arena.Allocate(rng() % 7 + 1);  // push the cursor off alignment
    char* p = arena.AllocateAligned(rng() % 64 + 1);
    CHECK_EQ(reinterpret_cast<uintptr_t>(p) % 8, 0u);
  }
}

TEST(Bloom, NoFalseNegatives) {
  for (int bits : {4, 10, 16}) {
    BloomFilterBuilder builder(bits);
    for (int i = 0; i < 5000; ++i) builder.Add(Key(i));
    std::string filter;
    builder.Finish(&filter);
    // A bloom filter is allowed to say "maybe" for a key it never saw, but
    // never "no" for one it did. A false negative loses data.
    for (int i = 0; i < 5000; ++i) CHECK(BloomMayContain(filter, Key(i)));
  }
}

// The measured rate should be near the theoretical (1 - e^(-kn/m))^k. The
// bound here is loose enough not to be flaky but tight enough to catch a
// filter that is doing nothing useful.
TEST(Bloom, FalsePositiveRateIsNearTheory) {
  struct Expect {
    int bits;
    double max_rate;
  };
  for (const auto [bits, max_rate] : {Expect{4, 0.20}, Expect{10, 0.02}, Expect{16, 0.005}}) {
    BloomFilterBuilder builder(bits);
    constexpr int kKeys = 10000;
    for (int i = 0; i < kKeys; ++i) builder.Add(Key(i));
    std::string filter;
    builder.Finish(&filter);

    int false_positives = 0;
    constexpr int kProbes = 20000;
    for (int i = 0; i < kProbes; ++i) {
      if (BloomMayContain(filter, Key(1'000'000 + i))) ++false_positives;
    }
    const double rate = static_cast<double>(false_positives) / kProbes;
    std::printf("       %2d bits/key: %.3f%% false positives (%.1f bytes/key)\n", bits,
                rate * 100.0, static_cast<double>(filter.size()) / kKeys);
    CHECK(rate < max_rate);
  }
}

TEST(Bloom, EmptyAndMalformedFiltersAnswerMaybe) {
  // Degrading to "maybe" costs a block read; answering "no" would lose data.
  CHECK(BloomMayContain("", "anything"));
  CHECK(BloomMayContain("x", "anything"));
  std::string bad_probe_count(32, '\0');
  bad_probe_count.push_back(static_cast<char>(200));
  CHECK(BloomMayContain(bad_probe_count, "anything"));
}

TEST(Bloom, EmptyKeySetStillWorks) {
  BloomFilterBuilder builder(10);
  std::string filter;
  builder.Finish(&filter);
  CHECK(filter.size() >= 2);
  // Nothing was added, so nothing should match beyond chance; with no keys
  // every bit is clear, so every probe must miss.
  for (int i = 0; i < 100; ++i) CHECK(!BloomMayContain(filter, Key(i)));
}

}  // namespace lsmkv
