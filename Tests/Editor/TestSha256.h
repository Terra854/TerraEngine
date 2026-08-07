// TestSha256.h - small portable SHA-256 helper for immutable fixture checks.
//
// Tests hash bytes themselves so a nearby expected literal cannot accidentally
// bless an edited external fixture. This header has no operating-system crypto
// dependency and is intentionally used only outside runtime hot paths.

#pragma once

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::EditorTests
{
	constexpr std::uint32_t RotateSha256Right(std::uint32_t value, std::uint32_t bits) noexcept
	{
		return (value >> bits) | (value << (32U - bits));
	}

	inline std::string ComputeTestSha256(std::string_view input)
	{
		constexpr std::array<std::uint32_t, 64U> roundConstants = {
			0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
			0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
			0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
			0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
			0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
			0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
			0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
			0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
			0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
			0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
			0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
			0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
			0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
			0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
			0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
			0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
		std::array<std::uint32_t, 8U> state = {
			0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
			0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
		std::vector<std::uint8_t> bytes(input.begin(), input.end());
		const std::uint64_t bitLength = static_cast<std::uint64_t>(bytes.size()) * 8ULL;
		bytes.push_back(0x80U);
		while ((bytes.size() % 64U) != 56U)
		{
			bytes.push_back(0U);
		}
		for (int shift = 56; shift >= 0; shift -= 8)
		{
			bytes.push_back(static_cast<std::uint8_t>((bitLength >> shift) & 0xffULL));
		}

		for (std::size_t block = 0U; block < bytes.size(); block += 64U)
		{
			std::array<std::uint32_t, 64U> words{};
			for (std::size_t index = 0U; index < 16U; ++index)
			{
				const std::size_t offset = block + index * 4U;
				words[index] = (static_cast<std::uint32_t>(bytes[offset]) << 24U)
					| (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U)
					| (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U)
					| static_cast<std::uint32_t>(bytes[offset + 3U]);
			}
			for (std::size_t index = 16U; index < words.size(); ++index)
			{
				const std::uint32_t s0 = RotateSha256Right(words[index - 15U], 7U)
					^ RotateSha256Right(words[index - 15U], 18U) ^ (words[index - 15U] >> 3U);
				const std::uint32_t s1 = RotateSha256Right(words[index - 2U], 17U)
					^ RotateSha256Right(words[index - 2U], 19U) ^ (words[index - 2U] >> 10U);
				words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
			}
			std::uint32_t a = state[0U];
			std::uint32_t b = state[1U];
			std::uint32_t c = state[2U];
			std::uint32_t d = state[3U];
			std::uint32_t e = state[4U];
			std::uint32_t f = state[5U];
			std::uint32_t g = state[6U];
			std::uint32_t h = state[7U];
			for (std::size_t round = 0U; round < roundConstants.size(); ++round)
			{
				const std::uint32_t sigma1 = RotateSha256Right(e, 6U)
					^ RotateSha256Right(e, 11U) ^ RotateSha256Right(e, 25U);
				const std::uint32_t choose = (e & f) ^ ((~e) & g);
				const std::uint32_t first = h + sigma1 + choose
					+ roundConstants[round] + words[round];
				const std::uint32_t sigma0 = RotateSha256Right(a, 2U)
					^ RotateSha256Right(a, 13U) ^ RotateSha256Right(a, 22U);
				const std::uint32_t second = sigma0 + ((a & b) ^ (a & c) ^ (b & c));
				h = g;
				g = f;
				f = e;
				e = d + first;
				d = c;
				c = b;
				b = a;
				a = first + second;
			}
			state[0U] += a;
			state[1U] += b;
			state[2U] += c;
			state[3U] += d;
			state[4U] += e;
			state[5U] += f;
			state[6U] += g;
			state[7U] += h;
		}
		std::ostringstream output;
		output << std::hex << std::setfill('0');
		for (std::uint32_t word : state)
		{
			output << std::setw(8) << word;
		}
		return output.str();
	}
}
