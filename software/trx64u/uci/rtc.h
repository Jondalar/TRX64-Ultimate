// trx64u's UCI library (uci/, U18): a header of the firmware that the UCI sources include and SDK 0.4.6 no longer ships (MultiCore T239.3 D48).
// PROVENANCE: TREX/1541ultimate:software/io/rtc/rtc.h (1541ultimate, Gideon Zweijtzer, GPL-3.0; our fork at 0103c585), byte for byte the
// header TRX64U-MultiCore's host-app carries at t239.3 6e280710 and SDK 0.4.5 shipped in base/inc. Changes here: this header only.
#ifndef RTC_H
#define RTC_H
/*
-------------------------------------------------------------------------------
--
--  (C) COPYRIGHT 2009, Gideon's Hardware and Programmable Logic Solutions
--
-------------------------------------------------------------------------------
-- Project    : Ultimate 1541
-- Title      : rtc.h
-------------------------------------------------------------------------------
*/
#include <stdint.h>
#include "config.h"
#include "iomap.h"

class RtcConfigStore : public ConfigStore
{
public:
	RtcConfigStore(const char *name, t_cfg_definition *defs) : ConfigStore(NULL, name, defs, NULL) { }
	~RtcConfigStore() {  }

	void read(bool ignore) override { }
	void write(void) override { }
    void effectuate(void) override { set_effectuated(); }
	void at_open_config(void);
	void at_close_config(void);

	// The clock is not configuration: these items are the current date and
	// time, read from the RTC when the menu opens. Writing them to a .cfg
	// would capture the moment the file was saved, and loading it would set
	// the clock back to then.
	bool save_to_cfg_file(void) { return false; }
};

class Rtc
{
private:
	void write_byte(int addr, uint8_t val);
	void read_all();
    bool capable;
	uint8_t rtc_regs[16];
	RtcConfigStore *cfg;
public:
	Rtc();
	~Rtc();
	void init(void);
	
	bool is_valid(void);
	int  get_correction(void);
	void get_time_from_chip(void);
	void set_time_in_chip(int, int y, int M, int D, int wd, int h, int m, int s);

	void get_time(int &y, int &M, int &D, int &wd, int &h, int &m, int &s);
	void set_time(int y, int M, int D, int wd, int h, int m, int s);
    void set_time_utc(int seconds);
	const char* get_time_string(char *dest, int len);
	const char* get_date_string(char *dest, int len);
	const char* get_long_date(char *dest, int len);
	uint32_t get_fat_time(void);
};

extern "C" uint32_t get_fattime(void);

extern Rtc rtc;
#endif
