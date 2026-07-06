// conv_bench: microbenchmark + A/B correctness check of the libcorrect Viterbi
// decoder (the RyFi FEC hot spot; ~38% of pipeline CPU). Encodes a deterministic
// payload with the exact RyFi code (rate 1/2, K=7), decodes it repeatedly from soft
// symbols, times it, and verifies every decode round-trips byte-exact.
//
//   conv_bench [iters] [payload-bytes]      (defaults: 200, 4096)
//
// On aarch64 run it twice to A/B the NEON ACS against the scalar path:
//   ./conv_bench                            (NEON)
//   LIBCORRECT_NO_NEON=1 ./conv_bench       (scalar reference)
// Both must print the same "decoded output" checksum; the ratio of MB/s is the win.
//
// Copyright 2026 RFNM. SPDX-License-Identifier: GPL-3.0-or-later
extern "C" {
#include "correct.h"
}

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char** argv) {
	const int iters = (argc > 1) ? atoi(argv[1]) : 200;
	const size_t bytes = (argc > 2) ? (size_t)atol(argv[2]) : 4096;

	correct_convolutional* conv = correct_convolutional_create(2, 7, correct_conv_r12_7_polynomial);

	// deterministic payload
	std::vector<uint8_t> payload(bytes);
	uint32_t rng = 0x52464E4D; // 'RFNM'
	for(size_t i = 0; i < bytes; i++) {
		rng = rng * 1664525u + 1013904223u;
		payload[i] = (uint8_t)(rng >> 24);
	}

	// encode, then expand the packed encoded bits into clean soft symbols
	const size_t enc_len_bits = correct_convolutional_encode_len(conv, bytes);
	std::vector<uint8_t> encoded((enc_len_bits + 7) / 8);
	correct_convolutional_encode(conv, payload.data(), bytes, encoded.data());
	std::vector<uint8_t> soft(enc_len_bits);
	for(size_t b = 0; b < enc_len_bits; b++) {
		soft[b] = (encoded[b / 8] >> (7 - (b % 8))) & 1 ? 255 : 0;
	}

	std::vector<uint8_t> out(bytes + 16);
	uint64_t checksum = 0;
	const auto t0 = std::chrono::steady_clock::now();
	int bad = 0;
	for(int it = 0; it < iters; it++) {
		ssize_t n = correct_convolutional_decode_soft(conv, soft.data(), enc_len_bits, out.data());
		if(n != (ssize_t)bytes || memcmp(out.data(), payload.data(), bytes)) {
			bad++;
		}
		for(size_t i = 0; i < bytes; i++) {
			checksum = checksum * 31 + out[i];
		}
	}
	const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

	printf("decoded %d x %zu bytes in %.3f s = %.2f MB/s (payload)\n", iters, bytes, dt, (double)iters * bytes / dt / 1e6);
	printf("decoded output checksum: %016llx\n", (unsigned long long)checksum);
	printf(bad ? "CONV FAILED (%d bad decodes)\n" : "CONV OK\n", bad);
	correct_convolutional_destroy(conv);
	return bad ? 1 : 0;
}
