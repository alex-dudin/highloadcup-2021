#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>

struct go_rng_seed_block
{
    unsigned start_seed;
    unsigned length;
};

extern const go_rng_seed_block go_rng_seed_superblocks[4];

extern const int64_t rng_cooked[607];

class go_rng_seed_recovery
{
public:
    explicit go_rng_seed_recovery(unsigned raw_seed)
    {
        set_state(raw_seed);
    }

    unsigned current_seed() const
    {
        return seeds[offset];
    }

    void set_state(unsigned raw_seed)
    {
        offset = 0;
        rand_state = raw_seed;
        vec0_ptr = vec0;

        reset_state();
        
        for (int i = 0; i < RNG_LEN; ++i)
        {
            seeds[i] = rand_state;
            int64_t u = int64_t(next_rand()) << 40;
            u ^= int64_t(next_rand()) << 20;
            u ^= int64_t(next_rand());
            vec0[i] = u;
            vec0[i + RNG_LEN] = u;
        }
    }

    void next_state()
    {
        reset_state();

        const int i = offset++;
        if (offset == RNG_LEN)
            offset = 0;

        seeds[i] = rand_state;
        int64_t u = int64_t(next_rand()) << 40;
        u ^= int64_t(next_rand()) << 20;
        u ^= int64_t(next_rand());
        vec0[i] = u;
        vec0[i + RNG_LEN] = u;

        vec0_ptr = vec0 + offset;
    }

    void reset_state()
    {
        tap = RNG_LEN - 1;
        feed = RNG_LEN - RNG_TAP - 1;
        count = 0;
    }

    int32_t int31()
    {
        return static_cast<int32_t>(int63() >> 32);
    }

    int64_t int63()
    {
        return static_cast<int64_t>(uint64() & RNG_MASK);
    }

    int32_t int31n(int32_t n)
    {
        if (n <= 0)
            throw std::invalid_argument("invalid argument to int31n");

        if ((n & (n - 1)) == 0)
            return int31() & (n - 1);

        const int32_t max = static_cast<int32_t>((1u << 31) - 1 - (1u << 31) % static_cast<uint32_t>(n));
        int32_t v = int31();
        while (v > max)
        {
            v = int31();
        }
        return v % n;
    }

    int32_t int31n_fast(uint16_t n)
    {
        auto v = static_cast<unsigned>((uint64() & RNG_MASK) >> 32);
        if (v >= 0x7fff0000)
        {
            const auto max = (1u << 31) - 1 - (1u << 31) % n;
            while (v > max)
            {
                v = static_cast<unsigned>(int31());
            }
        }
        return static_cast<int32_t>(v % n);
    }
    
    template <uint32_t n>
    int32_t int31n()
    {
        static_assert(n > 0, "invalid argument to int31n");

        if ((n & (n - 1)) == 0)
            return int31() & (n - 1);

        auto v = static_cast<uint32_t>((uint64() & RNG_MASK) >> 32);
        constexpr auto max = (1u << 31) - 1 - (1u << 31) % n;
        while (v > max)
        {
            v = static_cast<uint32_t>(int31());
        }
        return static_cast<int32_t>(v % n);
    }
    
    int64_t int63n(int64_t n)
    {
        if (n <= 0)
            throw std::invalid_argument("invalid argument to int63n");

        if ((n & (n - 1)) == 0)
            return int63() & (n - 1);

        const int64_t max = static_cast<int64_t>((1ull << 63) - 1 - (1ull << 63) % static_cast<uint64_t>(n));
        int64_t v = int63();
        while (v > max)
        {
            v = int63();
        }
        return v % n;
    }

    int64_t intn(int64_t n)
    {
        if (n <= 0)
            throw std::invalid_argument("invalid argument to intn");

        if (n <= std::numeric_limits<int32_t>::max())
            return int31n(static_cast<int32_t>(n));

        return int63n(n);
    }

    uint32_t uint32()
    {
        return static_cast<uint32_t>(int63() >> 31);
    }

    uint64_t uint64()
    {
        const auto feed_prev = feed;
        int64_t x;
        if (count < RNG_TAP)
        {
            x = (vec0_ptr[feed] ^ rng_cooked[feed]) + (vec0_ptr[tap] ^ rng_cooked[tap]);

            count++;
            tap--;
            feed--;
        }
        else
        {
            if (count < RNG_LEN)
            {
                x = (vec0_ptr[feed] ^ rng_cooked[feed]) + vec1[tap];
                count++;
            }
            else
            {
                x = vec1[feed] + vec1[tap];
            }

            tap--;
            if (tap < 0)
                tap += RNG_LEN;

            feed--;
            if (feed < 0)
                feed += RNG_LEN;
        }
        vec1[feed_prev] = x;
        return static_cast<uint64_t>(x);
    }

    uint64_t peek_uint64()
    {
        int64_t x;
        if (count < RNG_TAP)
        {
            x = (vec0_ptr[feed] ^ rng_cooked[feed]) + (vec0_ptr[tap] ^ rng_cooked[tap]);
        }
        else
        {
            if (count < RNG_LEN)
            {
                x = (vec0_ptr[feed] ^ rng_cooked[feed]) + vec1[tap];
            }
            else
            {
                x = vec1[feed] + vec1[tap];
            }
        }
        return static_cast<uint64_t>(x);
    }

    template <unsigned n>
    std::array<uint64_t, n> peek_n_uint64()
    {
        std::array<uint64_t, n> result;
        const auto original_feed = feed;
        const auto original_tap = tap;
        const auto original_count = count;
        for (unsigned i = 0; i < n; ++i)
        {
            int64_t x;
            if (count < RNG_TAP)
            {
                x = (vec0_ptr[feed] ^ rng_cooked[feed]) + (vec0_ptr[tap] ^ rng_cooked[tap]);
            }
            else
            {
                if (count < RNG_LEN)
                {
                    x = (vec0_ptr[feed] ^ rng_cooked[feed]) + vec1[tap];
                }
                else
                {
                    x = vec1[feed] + vec1[tap];
                }
            }

            tap--;
            if (tap < 0)
                tap += RNG_LEN;

            feed--;
            if (feed < 0)
                feed += RNG_LEN;

            count++;

            result[i] = static_cast<uint64_t>(x);
        }
        feed = original_feed;
        tap = original_tap;
        count = original_count;
        return result;
    }
    
    int32_t peek_int31n_fast(uint16_t n)
    {
        auto v = static_cast<unsigned>((peek_uint64() & RNG_MASK) >> 32);
        if (v >= 0x7fff0000)
            return -1;

        return static_cast<int32_t>(v % n);
    }

    static uint32_t get_raw_seed(int32_t seed);
    static int32_t recover_normal_seed(uint32_t raw_seed);

private:
    static constexpr int RNG_LEN = 607;
    static constexpr int RNG_TAP = 273;
    static constexpr uint64_t RNG_MASK = (UINT64_C(1) << 63) - 1;

    int tap;
    int feed;
    int64_t vec0[RNG_LEN * 2];
    int64_t vec1[RNG_LEN];
    int64_t* vec0_ptr;
    unsigned seeds[RNG_LEN];
    int offset;
    unsigned rand_state;
    unsigned count;

    unsigned next_rand()
    {
        const uint64_t product = (uint64_t)rand_state * 48271;
        const uint32_t x = (product & 0x7fffffff) + (product >> 31);
        return rand_state = (x & 0x7fffffff) + (x >> 31);
    }
};
