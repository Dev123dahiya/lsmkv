// Bloom filter over a table's keys. A point lookup that misses can skip
// reading the data block entirely, which is where most of the read speedup in
// an LSM tree comes from: without it, a miss costs one block read per table.
//
// Probes use the Kirsch-Mitzenmacher construction: two hash values derived
// from one 32-bit hash, combined as h + i*delta, rather than k independent
// hashes.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lsmkv {

uint32_t BloomHash(std::string_view key) noexcept;

class BloomFilterBuilder {
 public:
  explicit BloomFilterBuilder(int bits_per_key) : bits_per_key_(bits_per_key) {
    // k = ln(2) * m/n minimises the false positive rate. Clamped because the
    // cost of probing grows linearly while the benefit flattens out.
    probes_ = static_cast<int>(static_cast<double>(bits_per_key) * 0.69314718);
    if (probes_ < 1) probes_ = 1;
    if (probes_ > 30) probes_ = 30;
  }

  void Add(std::string_view key) { hashes_.push_back(BloomHash(key)); }

  size_t KeyCount() const noexcept { return hashes_.size(); }

  // Appends the filter to `dst`: the bit array followed by one byte holding
  // the probe count, so a reader does not need to be told the parameters.
  void Finish(std::string* dst);

  void Reset() { hashes_.clear(); }

 private:
  int bits_per_key_;
  int probes_;
  std::vector<uint32_t> hashes_;
};

// Tests a filter produced by BloomFilterBuilder::Finish. A filter that is
// empty or malformed answers "maybe", so a damaged filter costs performance
// rather than correctness.
bool BloomMayContain(std::string_view filter, std::string_view key) noexcept;

}  // namespace lsmkv
