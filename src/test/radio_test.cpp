#include "radio.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>

static int fails;

static void check(bool ok, const char* name)
{
	if (!ok) {
		fails++;
		std::cerr << "FAIL " << name << "\n";
	}
}

static void pass(const char* name)
{
	if (fails) std::exit(1);
	std::cout << "PASS " << name << "\n";
}

int main()
{
	{
		radio_fake_plug(true);
		Radio* r = nullptr;
		RadioOpen cfg{};
		check(radio_open(&r, cfg) == RadioErr::ok && r, "open fake");
		radio_close(r);
		pass("radio_open fake");
	}

	{
		radio_fake_plug(true);
		Radio* r = nullptr;
		RadioOpen cfg{};
		cfg.rate_hz = 0;
		check(radio_open(&r, cfg) == RadioErr::ok && r, "open rate 0");
		check(radio_max_rate(r, 0) > 0, "max rate positive");
		radio_close(r);
		pass("radio_open rate 0");
	}

	{
		radio_fake_plug(true);
		Radio* r = nullptr;
		RadioOpen cfg{};
		check(radio_open(&r, cfg) == RadioErr::ok && r, "open for retune");
		const float old_iq[] = {1.f, 2.f, 3.f, 4.f};
		radio_fake_queue(old_iq, 2);
		check(radio_set_center(r, 400000000) == RadioErr::ok, "set center");
		float out[4] = {};
		check(radio_read(r, out, 2) == 0, "pre-tune buffer dropped");
		const float new_iq[] = {5.f, 6.f};
		radio_fake_queue(new_iq, 1);
		check(radio_read(r, out, 1) == 1 && out[0] == 5.f && out[1] == 6.f,
		      "post-tune samples");
		radio_close(r);
		pass("radio_set_center drops pre-tune buffer");
	}

	{
		Radio* poison = (Radio*)0x1;
		radio_fake_plug(false);
		RadioOpen cfg{};
		check(radio_open(&poison, cfg) == RadioErr::not_found, "open unplugged");
		check(poison == (Radio*)0x1, "handle unchanged");
		check(std::strstr(radio_error(RadioErr::not_found), "no SDR found") != nullptr,
		      "no SDR found");
		pass("failed radio_open leaves handle");
	}

	{
		radio_fake_plug(true);
		Radio* r = nullptr;
		RadioOpen cfg{};
		check(radio_open(&r, cfg) == RadioErr::ok && r, "open for timeout");
		float buf[8] = {};
		auto t0 = std::chrono::steady_clock::now();
		int n = radio_read(r, buf, 4);
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				  std::chrono::steady_clock::now() - t0)
				  .count();
		check(n == 0, "timeout returns 0");
		check(ms <= 150, "timeout within 150ms");
		radio_close(r);
		pass("radio_read timeout");
	}

	{
		radio_close(nullptr);
		radio_fake_plug(true);
		Radio* r = nullptr;
		RadioOpen cfg{};
		check(radio_open(&r, cfg) == RadioErr::ok && r, "open for close");
		radio_close(r);
		radio_close(nullptr);
		pass("radio_close");
	}

	{
		RadioDetect empty = radio_detect(0, nullptr, 0);
		check(empty.err == RadioErr::not_found, "empty not_found");

		UsbId hackrf{0x1d50, 0x6089};
		RadioDetect h = radio_detect(0, &hackrf, 1);
		check(h.err == RadioErr::not_recognized && h.name && std::strcmp(h.name, "HackRF") == 0,
		      "HackRF");

		UsbId airspy{0x1d50, 0x60a1};
		RadioDetect a = radio_detect(0, &airspy, 1);
		check(a.err == RadioErr::not_recognized && a.name && std::strcmp(a.name, "Airspy") == 0,
		      "Airspy");

		UsbId unknown{0x1234, 0x0001};
		RadioDetect u = radio_detect(0, &unknown, 1);
		check(u.err == RadioErr::not_found, "unknown not_found");

		RadioDetect soapy = radio_detect(1, nullptr, 0);
		check(soapy.err == RadioErr::ok, "soapy ok");

		pass("radio_detect");
	}

	return 0;
}
