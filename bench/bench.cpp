// ryfi_bench: hardware-free loopback benchmark and correctness check of the full RyFi
// modem chain (packet -> RS -> conv -> framing -> RRC mod | AGC | FIR -> demod ->
// packet). The pipeline is unthrottled, so the measured sample rate is the machine's
// DSP capacity for this chain; xRealtime = capacity / the rate the link needs. This is
// the number that qualifies a CPU (i.MX8MP A53) for a given baudrate.
//
//   ryfi_bench [baudrate] [seconds] [min-xrt]     (defaults: 720e3, 5, 0)
//
// The exit verdict is modem correctness (every received packet byte-exact); the
// throughput is reported alongside and only gates the verdict when min-xrt is given
// (use 1.0 on the target CPU; under qemu the speed measures the emulator, not the A53).
//
// Copyright 2026 RFNM. SPDX-License-Identifier: GPL-3.0-or-later
#include "ryfi/transmitter.h"
#include "ryfi/receiver.h"
#include "dsp/loop/fast_agc.h"
#include "dsp/taps/low_pass.h"
#include "dsp/filter/fir.h"
#include "dsp/sink/null_sink.h"
#include "flog/flog.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

static std::atomic<uint64_t> rxPackets{0};
static std::atomic<uint64_t> rxGood{0};
static std::atomic<uint64_t> rxBytes{0};

// deterministic payload so the receiver can verify every byte from the sequence number
static void fillPayload(uint8_t* buf, int size, uint32_t seq) {
	buf[0] = seq & 0xFF;
	buf[1] = (seq >> 8) & 0xFF;
	buf[2] = (seq >> 16) & 0xFF;
	buf[3] = (seq >> 24) & 0xFF;
	for(int i = 4; i < size; i++) {
		buf[i] = (uint8_t)(i * 7 + seq);
	}
}

static void packetHandler(ryfi::Packet pkt) {
	rxPackets++;
	rxBytes += pkt.size();
	if(pkt.size() < 4) {
		return;
	}
	const uint8_t* d = pkt.data();
	uint32_t seq = d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24);
	uint8_t ref[4096];
	int size = pkt.size() > (int)sizeof(ref) ? (int)sizeof(ref) : pkt.size();
	fillPayload(ref, size, seq);
	if(!memcmp(ref, d, size)) {
		rxGood++;
	}
}

int main(int argc, char** argv) {
	double baudrate = (argc > 1) ? atof(argv[1]) : 720e3;
	double seconds = (argc > 2) ? atof(argv[2]) : 5.0;
	double minXrt = (argc > 3) ? atof(argv[3]) : 0.0;
	double samplerate = 2.0 * baudrate;
	constexpr int PKT_SIZE = 1024;

	flog::info("ryfi_bench: {} baud, {} S/s, {} s run", baudrate, samplerate, seconds);

	// TX chain, exactly like main(): transmitter -> AGC
	ryfi::Transmitter tx(baudrate, samplerate);
	dsp::loop::FastAGC<dsp::complex_t> agc(tx.out, 0.5, 1e6, 0.00001, 0.00001);

	// counting bridge: AGC out -> RX chain in, tallying every sample that crosses
	dsp::stream<dsp::complex_t> bridge;
	std::atomic<uint64_t> samples{0};
	std::atomic<bool> bridgeRun{true};
	std::thread bridgeThread([&] {
		while(bridgeRun) {
			int n = agc.out.read();
			if(n < 0) {
				break;
			}
			memcpy(bridge.writeBuf, agc.out.readBuf, n * sizeof(dsp::complex_t));
			agc.out.flush();
			if(!bridge.swap(n)) {
				break;
			}
			samples += n;
		}
	});

	// RX chain, exactly like main(): FIR lowpass -> receiver -> null sink on softOut
	double rxBandwidth = 1.6 * baudrate;
	dsp::tap lpTaps = dsp::taps::lowPass(rxBandwidth / 2.0, rxBandwidth / 20.0f, samplerate);
	dsp::filter::FIR<dsp::complex_t, float> lp(&bridge, lpTaps);
	ryfi::Receiver rx(&lp.out, baudrate, samplerate);
	rx.onPacket.bind(packetHandler);
	dsp::sink::Null<dsp::complex_t> ns(rx.softOut, NULL, NULL);

	tx.start();
	agc.start();
	lp.start();
	rx.start();
	ns.start();

	// feed packets as fast as the transmitter queue accepts them
	std::atomic<bool> feedRun{true};
	std::atomic<uint64_t> txPackets{0};
	std::thread feedThread([&] {
		uint8_t buf[PKT_SIZE];
		uint32_t seq = 0;
		while(feedRun) {
			fillPayload(buf, PKT_SIZE, seq);
			if(tx.send(ryfi::Packet(buf, PKT_SIZE))) {
				seq++;
				txPackets++;
			} else {
				std::this_thread::sleep_for(std::chrono::microseconds(200));
			}
		}
	});

	const auto t0 = std::chrono::steady_clock::now();
	std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
	const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	feedRun = false;
	feedThread.join();

	// let in-flight samples drain until the receive count stops moving
	uint64_t lastCount = 0;
	do {
		lastCount = rxPackets;
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
	} while(rxPackets != lastCount);

	const double msps = (double)samples / dt / 1e6;
	const double xrt = msps * 1e6 / samplerate;
	printf("\n==== ryfi_bench results ====\n");
	printf("pipeline throughput : %.2f MS/s (%.1fx realtime at %.0f S/s)\n", msps, xrt, samplerate);
	printf("packets tx/rx/good  : %llu / %llu / %llu\n", (unsigned long long)txPackets, (unsigned long long)rxPackets, (unsigned long long)rxGood);
	printf("payload bytes ok    : %llu\n", (unsigned long long)rxBytes.load());
	const bool modemOk = (rxGood > 0) && (rxGood == rxPackets);
	const bool speedOk = (minXrt <= 0.0) || (xrt >= minXrt);
	printf("modem   : %s (every received packet byte-exact)\n", modemOk ? "OK" : "FAILED");
	if(minXrt > 0.0) {
		printf("speed   : %s (%.1fx realtime, need %.1fx)\n", speedOk ? "OK" : "FAILED", xrt, minXrt);
	}
	const bool ok = modemOk && speedOk;
	printf(ok ? "BENCH OK\n" : "BENCH FAILED\n");
	fflush(stdout);

	// best-effort teardown; the report is already out and some SDR++ dsp blocks are
	// unhappy about teardown order, so never let a stuck join eat the result
	bridgeRun = false;
	agc.out.stopReader();
	tx.stop();
	agc.stop();
	if(bridgeThread.joinable()) {
		bridgeThread.join();
	}
	_exit(ok ? 0 : 1);
}
