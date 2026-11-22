// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// A CPU model of the guest's half-precision matrix multiply-add (HMMA.16816.F16) against the ways
// the host can run it (docs/design/game-reconstruction-and-motion.md, 2.4): the guest rounds its
// fp16 accumulator after each K=8 group, EDEN_NX2_HMMA_K16_ONCE after each K=16 product, an fp32
// accumulator only once per output. The stacks are shaped like Ocarina of Time's output pass
// (per-pixel layers of 16 and 64 channels, ReLU after the wide ones), with seeded weights: no game
// data. The reference is exact arithmetic on the same fp16 inputs and weights.

#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr int BLOCKS = 6;
constexpr int WIDE = 64;
constexpr int NARROW = 16;
constexpr int PIXELS = 256;

enum class Mode {
    Guest,      ///< fp16 accumulator rounded after each K=8 group
    K16Once,    ///< rounded after each K=16 product
    Fp32,       ///< fp32 accumulator, rounded once per output
    Reference,  ///< double, never rounded
};

/// Nearest-even binary16 value of a float, subnormals included (the data never overflows).
float RoundToHalf(float value) {
    const float magnitude = std::fabs(value);
    int exponent{};
    std::frexp(magnitude, &exponent);
    // The half grid's spacing at this magnitude: 2^(exponent - 11), at least 2^-24.
    const int spacing = (std::max)(exponent - 11, -24);
    const float rounded = std::nearbyint(std::ldexp(magnitude, -spacing));
    return std::copysign(std::ldexp(rounded, spacing), value);
}

double Ulp16(double value) {
    int exponent{};
    std::frexp(std::fabs(value), &exponent);
    return std::ldexp(1.0, (std::max)(exponent - 11, -24));
}

struct SplitMix {
    std::uint64_t state;

    std::uint64_t Next() {
        state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    /// [-1, 1) in steps of 2^-23
    double Uniform() {
        return static_cast<double>(Next() >> 40) / static_cast<double>(1U << 24) * 2.0 - 1.0;
    }
};

float Half(double value) {
    return RoundToHalf(static_cast<float>(value));
}

struct Network {
    std::vector<std::array<std::array<float, NARROW>, WIDE>> expand;   ///< [block][out][in]
    std::vector<std::array<std::array<float, WIDE>, NARROW>> contract; ///< [block][out][in]
    std::vector<std::array<float, NARROW>> pixels;

    explicit Network(std::uint64_t seed) : expand(BLOCKS), contract(BLOCKS), pixels(PIXELS) {
        SplitMix rng{seed};
        const double expand_scale = std::sqrt(6.0 / NARROW);
        const double contract_scale = std::sqrt(3.0 / WIDE);
        for (auto& block : expand) {
            for (auto& row : block) {
                for (float& weight : row) {
                    weight = Half(rng.Uniform() * expand_scale);
                }
            }
        }
        for (auto& block : contract) {
            for (auto& row : block) {
                for (float& weight : row) {
                    weight = Half(rng.Uniform() * contract_scale);
                }
            }
        }
        for (auto& pixel : pixels) {
            for (float& value : pixel) {
                value = Half(rng.Uniform());
            }
        }
    }
};

/// One output channel: the sum over k of a[k] * w[k] in K=16 products, as the mode rounds it.
template <size_t K>
double Dot(Mode mode, const std::array<double, K>& a, const std::array<float, K>& w) {
    if (mode == Mode::Reference) {
        double sum = 0.0;
        for (size_t k = 0; k < K; ++k) {
            sum += a[k] * static_cast<double>(w[k]);
        }
        return sum;
    }
    const size_t group = mode == Mode::Guest ? 8 : 16;
    float accumulator = 0.0f;
    for (size_t k = 0; k < K; ++k) {
        // fp16 times fp16 is exact in fp32, so a fused or separate multiply-add rounds the same.
        accumulator += static_cast<float>(a[k]) * w[k];
        if (mode != Mode::Fp32 && (k + 1) % group == 0) {
            accumulator = RoundToHalf(accumulator);
        }
    }
    return RoundToHalf(accumulator);
}

std::vector<double> Run(Mode mode, const Network& network) {
    std::vector<double> outputs;
    outputs.reserve(PIXELS * NARROW);
    for (const auto& pixel : network.pixels) {
        std::array<double, NARROW> narrow{};
        std::copy(pixel.begin(), pixel.end(), narrow.begin());
        for (int block = 0; block < BLOCKS; ++block) {
            std::array<double, WIDE> wide{};
            for (int out = 0; out < WIDE; ++out) {
                wide[out] = (std::max)(Dot(mode, narrow, network.expand[block][out]), 0.0);
            }
            for (int out = 0; out < NARROW; ++out) {
                narrow[out] = Dot(mode, wide, network.contract[block][out]);
            }
        }
        outputs.insert(outputs.end(), narrow.begin(), narrow.end());
    }
    return outputs;
}

struct Error {
    double max{};
    double mean{};
};

Error Compare(const std::vector<double>& values, const std::vector<double>& reference) {
    Error error;
    for (size_t index = 0; index < values.size(); ++index) {
        const double difference = std::fabs(values[index] - reference[index]);
        error.max = (std::max)(error.max, difference);
        error.mean += difference;
    }
    error.mean /= static_cast<double>(values.size());
    return error;
}

} // Anonymous namespace

TEST_CASE("The fp16 rounding model is binary16 nearest-even", "[video_core][tensor]") {
    REQUIRE(RoundToHalf(1.0f) == 1.0f);
    REQUIRE(RoundToHalf(1.0f + 0.5f / 1024.0f) == 1.0f);               // tie to even
    REQUIRE(RoundToHalf(1.0f + 1.5f / 1024.0f) == 1.0f + 2.0f / 1024.0f); // tie to even
    REQUIRE(RoundToHalf(1.0f + 0.6f / 1024.0f) == 1.0f + 1.0f / 1024.0f);
    REQUIRE(RoundToHalf(-3.14159f) == -3.140625f);
    REQUIRE(RoundToHalf(65504.0f) == 65504.0f);
    REQUIRE(RoundToHalf(std::ldexp(1.0f, -24)) == std::ldexp(1.0f, -24)); // smallest subnormal
    REQUIRE(RoundToHalf(std::ldexp(1.0f, -26)) == 0.0f);
    REQUIRE(RoundToHalf(std::ldexp(3.0f, -26)) == std::ldexp(1.0f, -24));
    REQUIRE(Ulp16(1.0) == std::ldexp(1.0, -10));
    REQUIRE(Ulp16(3.5) == std::ldexp(1.0, -9));
}

// A host path may stray from exact arithmetic about as far as the guest does: one rounding per
// K=16 product must not be further from the reference than the guest's two, by more than one fp16
// ulp at the largest output, and must be no worse on average. An fp32 accumulator is closer still.
TEST_CASE("One rounding per K=16 product stays within the guest's own error",
          "[video_core][tensor]") {
    REQUIRE(std::fegetround() == FE_TONEAREST);
    for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL, 4ULL}) {
        const Network network{seed};
        const std::vector<double> reference = Run(Mode::Reference, network);
        double peak = 0.0;
        for (const double value : reference) {
            peak = (std::max)(peak, std::fabs(value));
        }
        const Error guest = Compare(Run(Mode::Guest, network), reference);
        const Error k16_once = Compare(Run(Mode::K16Once, network), reference);
        const Error fp32 = Compare(Run(Mode::Fp32, network), reference);
        INFO("seed " << seed << ", largest output " << peak << ": guest " << guest.max << " / "
                     << guest.mean << ", K16 once " << k16_once.max << " / " << k16_once.mean
                     << ", fp32 " << fp32.max << " / " << fp32.mean);
        // The stacks carry signal and the guest's own rounding is visible but small.
        REQUIRE(peak > 1.0);
        REQUIRE(guest.mean > 0.0);
        REQUIRE(guest.max < 16.0 * Ulp16(peak));
        REQUIRE(k16_once.max <= guest.max + Ulp16(peak));
        REQUIRE(k16_once.mean <= guest.mean);
        REQUIRE(fp32.max <= k16_once.max + Ulp16(peak));
        REQUIRE(fp32.mean <= k16_once.mean);
    }
}
