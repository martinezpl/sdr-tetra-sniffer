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
	check(radio_settled_rate(5999999, 6000000) == 6000000, "a rounded read-back keeps the asked rate");
	check(radio_settled_rate(0, 3200000) == 3200000, "no read-back keeps the asked rate");
	check(radio_settled_rate(2400000, 3200000) == 2400000, "a real snap is kept");
	pass("radio_settled_rate");
	{
		Radio* r = nullptr;
		RadioOpen cfg{};
		RadioErr err = radio_open(&r, cfg);
		if (err == RadioErr::ok) {
			radio_close(r);
			std::cout << "SKIP radio_open soapy empty (device present)\n";
		} else if (err == RadioErr::not_found && r == nullptr) {
			pass("radio_open soapy empty");
		} else {
			if (r) radio_close(r);
			std::cout << "SKIP radio_open soapy empty (" << radio_error(err) << ")\n";
		}
	}

	{
		check(std::strstr(radio_error(RadioErr::bad_rate), "sample rate") != nullptr,
		      "bad_rate names rate");
		check(std::strstr(radio_error(RadioErr::bad_rate), "tune") == nullptr,
		      "bad_rate is not tune");
		check(std::strstr(radio_error(RadioErr::bad_tune), "tune") != nullptr,
		      "bad_tune names tune");
		check(std::strstr(radio_error(RadioErr::bad_channel), "RX channel") != nullptr,
		      "bad_channel names channel");
		check(std::strstr(radio_error(RadioErr::bad_antenna), "antenna") != nullptr,
		      "bad_antenna names antenna");
		pass("radio_error distinguishes rate from tune");
	}

	{
		radio_fake_plug(true);
		Radio* r = nullptr;
		RadioOpen cfg{};
		check(radio_open(&r, cfg) == RadioErr::ok && r, "open fake");
		check(radio_antenna(r) && radio_antenna(r)[0] == 0, "fake antenna empty");
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
		check(radio_rate(r) == radio_max_rate(r, 0), "rate 0 is the widest rate");
		radio_close(r);
		cfg.max_rate_hz = 3200000;
		check(radio_open(&r, cfg) == RadioErr::ok && r, "open rate 0 with cap");
		check(radio_rate(r) == 3200000, "cap holds the auto rate");
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
		radio_fake_plug(true);
		Radio* r = nullptr;
		RadioOpen cfg{};
		check(radio_open(&r, cfg) == RadioErr::ok && r, "open for rerate");
		const float old_iq[] = {1.f, 2.f, 3.f, 4.f};
		radio_fake_queue(old_iq, 2);
		check(radio_set_rate(r, 2400000) == RadioErr::ok, "set rate");
		float out[4] = {};
		check(radio_read(r, out, 2) == 0, "pre-rate buffer dropped");
		const float new_iq[] = {5.f, 6.f};
		radio_fake_queue(new_iq, 1);
		check(radio_read(r, out, 1) == 1 && out[0] == 5.f && out[1] == 6.f,
		      "post-rate samples");
		radio_close(r);
		pass("radio_set_rate drops pre-rate buffer");
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
		check(ms <= RADIO_READ_TIMEOUT_MS + 100, "timeout within 200ms");
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

		UsbId rtl{0x0bda, 0x2838};
		RadioDetect r8 = radio_detect(0, &rtl, 1);
		check(r8.err == RadioErr::not_recognized && r8.name && std::strcmp(r8.name, "RTL-SDR") == 0,
		      "RTL-SDR");
		check(radio_detect(1, &rtl, 1).err == RadioErr::ok, "RTL + soapy");

		UsbId hackrf{0x1d50, 0x6089};
		RadioDetect h = radio_detect(0, &hackrf, 1);
		check(h.err == RadioErr::not_recognized && h.name && std::strcmp(h.name, "HackRF") == 0,
		      "HackRF");
		check(radio_detect(1, &hackrf, 1).err == RadioErr::ok, "HackRF + soapy");

		UsbId airspy{0x1d50, 0x60a1};
		RadioDetect a = radio_detect(0, &airspy, 1);
		check(a.err == RadioErr::not_recognized && a.name && std::strcmp(a.name, "Airspy") == 0,
		      "Airspy");

		UsbId lime_mini{0x0403, 0x601f};
		check(radio_detect(0, &lime_mini, 1).err == RadioErr::not_recognized, "LimeSDR Mini");
		UsbId lime_usb{0x1d50, 0x6108};
		RadioDetect lime = radio_detect(0, &lime_usb, 1);
		check(lime.err == RadioErr::not_recognized && lime.name &&
			      std::strcmp(lime.name, "LimeSDR") == 0,
		      "LimeSDR-USB");

		UsbId sdrplay{0x1df7, 0x3000};
		check(radio_detect(0, &sdrplay, 1).err == RadioErr::not_recognized, "SDRplay");

		UsbId bladerf1{0x1d50, 0x6066};
		RadioDetect b1 = radio_detect(0, &bladerf1, 1);
		check(b1.err == RadioErr::not_recognized && b1.name && std::strcmp(b1.name, "bladeRF") == 0,
		      "bladeRF 1");

		UsbId unknown{0x1234, 0x0001};
		RadioDetect u = radio_detect(0, &unknown, 1);
		check(u.err == RadioErr::not_found, "unknown not_found");

		UsbId usrp{0x2500, 0x0020};
		RadioDetect b210 = radio_detect(0, &usrp, 1);
		check(b210.err == RadioErr::not_recognized && b210.name &&
			      std::strcmp(b210.name, "USRP B200/B210") == 0,
		      "USRP B210");

		RadioDetect soapy = radio_detect(1, nullptr, 0);
		check(soapy.err == RadioErr::ok, "soapy ok");

		pass("radio_detect");
	}

	{
		UsbId rtl{0x0bda, 0x2838};
		radio_fake_usb(&rtl, 1);
		Radio* poison = (Radio*)0x1;
		RadioOpen cfg{};
		RadioErr err = radio_open(&poison, cfg);
		check(err == RadioErr::not_recognized, "open known USB");
		check(poison == (Radio*)0x1, "handle unchanged");
		const char* msg = radio_error(RadioErr::not_recognized);
		// The hint depends on this host: its rtlsdr module is loaded, or not.
		check(std::strstr(msg, "RTL-SDR found") != nullptr &&
			      (std::strstr(msg, "\"rtlsdr\" is loaded") != nullptr ||
			       std::strstr(msg, "apt install soapysdr-module-rtlsdr") != nullptr ||
			       std::strstr(msg, "brew install soapyrtlsdr") != nullptr),
		      "hint names the module");
		radio_fake_usb(nullptr, 0);
		pass("radio_open not_recognized");
	}

	return 0;
}
