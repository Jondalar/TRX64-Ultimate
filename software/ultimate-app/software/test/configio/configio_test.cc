/*
 * configio_test.cc
 *
 * Host unit tests for the .cfg save/load rules in ConfigIO.
 *
 * Two behaviours are covered here because neither can be reached from the
 * device test harness:
 *
 *   1. Which stores are written to a .cfg file. Stores backed by a device
 *      rather than by an Ultimate flash page (the SID replacements) have to be
 *      written; the RTC store has to stay out. On real hardware a SID
 *      replacement store only exists when such a cartridge is plugged in, so
 *      the rule itself can only be proven here, where a page-less store can
 *      simply be constructed.
 *
 *   2. What an unknown item or store in a .cfg does. Loading a file written by
 *      a machine with different hardware is the normal case for this, and it
 *      must warn rather than fail: the warning goes to stdout, which the
 *      firmware routes to syslog.
 */

#include <stdio.h>
#include <string.h>

#include "flash.h" /* config.h's own includes first, so the next two lines only touch config.h */
#include "indexed_list.h"
#include "mystring.h"
#include "file.h"
#define class struct /* test only: reach ConfigStore::pack/unpack, the flash image */
#define private public
#include "config.h"
#undef private
#undef class
#include "configio.h"
#include "file.h"
#include "stream_textlog.h"

static int checks = 0;
static int failures = 0;

static void check(bool ok, const char *what)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", what);
    } else {
        failures++;
        printf("FAIL %s\n", what);
    }
}

/* A File that lives in memory, so the tests never touch a filesystem. */
class MemFile : public File
{
    char buffer[4096];
    uint32_t length;
    uint32_t pos;
public:
    MemFile() : File(NULL), length(0), pos(0) { buffer[0] = 0; }

    void load(const char *text)
    {
        length = (uint32_t)strlen(text);
        if (length > sizeof(buffer) - 1) {
            length = sizeof(buffer) - 1;
        }
        memcpy(buffer, text, length);
        buffer[length] = 0;
        pos = 0;
    }

    const char *text()
    {
        buffer[length] = 0;
        return buffer;
    }

    FRESULT read(void *dest, uint32_t len, uint32_t *transferred) override
    {
        uint32_t avail = length - pos;
        if (len > avail) {
            len = avail;
        }
        memcpy(dest, buffer + pos, len);
        pos += len;
        *transferred = len;
        return FR_OK;
    }

    FRESULT write(const void *src, uint32_t len, uint32_t *transferred) override
    {
        if (length + len > sizeof(buffer) - 1) {
            len = sizeof(buffer) - 1 - length;
        }
        memcpy(buffer + length, src, len);
        length += len;
        *transferred = len;
        return FR_OK;
    }
};

/* Stands in for the RTC store: page-less, but deliberately not saved. */
class ExcludedStore : public ConfigStore
{
public:
    ExcludedStore(const char *name, t_cfg_definition *defs)
        : ConfigStore(NULL, name, defs, NULL) { }
    bool save_to_cfg_file(void) override { return false; }
};

static const char *chips[] = { "6581", "8580" };

/* Real ARMSID enum labels. They carry leading spaces so the menu can right
   align them, and one contains a space in the middle. Both shapes end up in a
   .cfg verbatim, so both have to survive a round trip. */
static const char *filtlow8[] = { "  30", " ~45", " ~70", "  100" };
static const char *filthi8[] = { "12 kHz", "10 kHz", "8 kHz" };

static t_cfg_definition sid_defs[] = {
    { 0x01, CFG_TYPE_ENUM,  "Emulation Mode", "%s", chips, 0, 1, 0 },
    { 0x02, CFG_TYPE_ENUM,  "8580 Lowest Filt Freq", "%s", filtlow8, 0, 3, 0 },
    { 0x03, CFG_TYPE_ENUM,  "8580 Highest Filt Freq", "%s", filthi8, 0, 2, 0 },
    { CFG_TYPE_END, CFG_TYPE_END, "", "", NULL, 0, 0, 0 } };

static t_cfg_definition clock_defs[] = {
    { 0x01, CFG_TYPE_VALUE, "Time Zone", "%d", NULL, -12, 12, 0 },
    { CFG_TYPE_END, CFG_TYPE_END, "", "", NULL, 0, 0, 0 } };

/* A renamed category and item (T25.5: UltiSID -> reSID8): the old names have to keep working. */
static const char *curves[] = { "Chip Model", "6581 Follin", "6581 Galway", "8580" };
static t_cfg_definition renamed_defs[] = {
    { 0x90, CFG_TYPE_ENUM,  "reSID8 1 Filter Curve", "%s", curves, 0, 3, 0 },
    { 0x91, CFG_TYPE_ENUM,  "reSID8 2 Filter Curve", "%s", curves, 0, 3, 0 },
    { CFG_TYPE_END, CFG_TYPE_END, "", "", NULL, 0, 0, 0 } };

static bool contains(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != NULL;
}

int main(int argc, char **argv)
{
    ConfigManager *cm = ConfigManager::getConfigManager();

    /* A SID replacement store: no flash page, but it must be saved. */
    ConfigStore *sid = new ConfigStore(NULL, "SID Socket 1: PDsid", sid_defs, NULL);
    cm->add_custom_store(sid);

    /* The RTC store: no flash page, and it must not be saved. */
    ExcludedStore *clock = new ExcludedStore("Clock Settings", clock_defs);
    cm->add_custom_store(clock);

    printf("-- which stores reach a .cfg file\n");
    MemFile out;
    ConfigIO::S_write_to_file(&out);
    const char *written = out.text();

    check(contains(written, "[SID Socket 1: PDsid]"),
          "a page-less SID store is written to the .cfg");
    check(contains(written, "Emulation Mode=6581"),
          "the SID store's item is written with its value");
    check(!contains(written, "[Clock Settings]"),
          "a store that opts out is kept out of the .cfg");

    printf("-- reading a .cfg back\n");
    sid->set_value(0x01, 1); // 8580, so the read below has something to change
    check(sid->get_value(0x01) == 1, "precondition: the SID store reads 8580");

    MemFile in;
    StreamTextLog log(4096);
    in.load("[SID Socket 1: PDsid]\nEmulation Mode=6581\n\n");
    bool ok = ConfigIO::S_read_from_file(&in, &log);
    check(ok, "a well-formed .cfg loads without error");
    check(sid->get_value(0x01) == 0, "the value from the file is applied to the store");

    printf("-- enum labels with spaces survive a round trip\n");
    /* An ARMSID writes "8580 Lowest Filt Freq= ~45" into a .cfg, padding and
       all. The padding is written verbatim and read back as the same choice,
       including a label whose own name contains spaces. */
    sid->set_value(0x02, 1);   // " ~45"
    sid->set_value(0x03, 0);   // "12 kHz"
    MemFile pad;
    ConfigIO::S_write_to_file(&pad);
    check(contains(pad.text(), "8580 Lowest Filt Freq= ~45"),
          "a padded enum label is written with its spaces intact");
    check(contains(pad.text(), "8580 Highest Filt Freq=12 kHz"),
          "an enum label containing a space is written whole");

    sid->set_value(0x02, 0);
    sid->set_value(0x03, 2);
    MemFile padback;
    StreamTextLog logpad(4096);
    padback.load("[SID Socket 1: PDsid]\n8580 Lowest Filt Freq= ~45\n"
                 "8580 Highest Filt Freq=12 kHz\n\n");
    check(ConfigIO::S_read_from_file(&padback, &logpad),
          "reading those values back is not an error");
    check(sid->get_value(0x02) == 1, "the padded label is matched exactly");
    check(sid->get_value(0x03) == 0, "the label with an inner space is matched");

    printf("-- a hand-edited file may space things how it likes\n");
    /* Nobody types the menu's alignment padding. "8580 Lowest Filt Freq=30" is
       what a person writes for the choice the firmware spells "  30", and it
       used to be rejected as an invalid choice. */
    sid->set_value(0x02, 3);
    sid->set_value(0x03, 2);
    sid->set_value(0x01, 0);
    MemFile hand;
    StreamTextLog log_hand(4096);
    hand.load("[SID Socket 1: PDsid]\n"
              "8580 Lowest Filt Freq=30\n"          /* label is "  30" */
              "8580 Highest Filt Freq=  12 kHz  \n" /* inner space kept */
              "  Emulation Mode  =  8580  \n"       /* spaces around both */
              "\n");
    check(ConfigIO::S_read_from_file(&hand, &log_hand),
          "a hand-spaced file loads without error");
    check(sid->get_value(0x02) == 0, "an unpadded value matches a padded label");
    check(sid->get_value(0x03) == 0, "outer spaces are ignored, the inner one is not");
    check(sid->get_value(0x01) == 1, "a padded item name finds its item");

    printf("-- tolerance is not laxity\n");
    sid->set_value(0x02, 1);
    MemFile bogus;
    StreamTextLog log_bogus(4096);
    bogus.load("[SID Socket 1: PDsid]\n8580 Lowest Filt Freq=31\n\n");
    check(!ConfigIO::S_read_from_file(&bogus, &log_bogus),
          "a value that is not a choice is still an error");
    check(sid->get_value(0x02) == 1, "and the store keeps its previous value");

    printf("-- an unknown item is a warning, not a failure\n");
    MemFile unknown_item;
    StreamTextLog log2(4096);
    unknown_item.load("[SID Socket 1: PDsid]\nFilter Strength=7\nEmulation Mode=8580\n\n");
    ok = ConfigIO::S_read_from_file(&unknown_item, &log2);
    check(ok, "an unknown item does not fail the load");
    check(sid->get_value(0x01) == 1,
          "items after the unknown one are still applied");

    printf("-- an unknown store is a warning, not a failure\n");
    MemFile unknown_store;
    StreamTextLog log3(4096);
    unknown_store.load("[SID Socket 2: PDsid]\nEmulation Mode=6581\n\n");
    ok = ConfigIO::S_read_from_file(&unknown_store, &log3);
    check(ok, "a store this machine does not have does not fail the load");
    check(sid->get_value(0x01) == 1, "an unknown store does not touch another store");

    printf("-- a malformed file is still an error\n");
    MemFile broken;
    StreamTextLog log4(4096);
    broken.load("[SID Socket 1: PDsid]\nEmulation Mode\n\n");
    ok = ConfigIO::S_read_from_file(&broken, &log4);
    check(!ok, "a line with no '=' is reported as an error");

    MemFile orphan;
    StreamTextLog log5(4096);
    orphan.load("Emulation Mode=6581\n\n");
    ok = ConfigIO::S_read_from_file(&orphan, &log5);
    check(!ok, "an item outside any store is reported as an error");

    printf("-- a renamed category and item are found by their old names\n");
    ConfigStore *rs8 = new ConfigStore(NULL, "reSID8 Configuration", renamed_defs, NULL);
    rs8->set_alt_name("UltiSID Configuration");
    rs8->find_item(0x90)->set_legacy_name("UltiSID 1 Filter Curve");
    rs8->find_item(0x91)->set_legacy_name("UltiSID 2 Filter Curve");
    cm->add_custom_store(rs8);
    check(cm->find_store("reSID8 Configuration") == rs8, "the store is found by its name");
    check(cm->find_store("UltiSID Configuration") == rs8, "the store is found by the name it had");
    check(rs8->find_item("reSID8 2 Filter Curve") == rs8->find_item(0x91), "an item is found by its name");
    check(rs8->find_item("UltiSID 2 Filter Curve") == rs8->find_item(0x91), "an item is found by the name it had");
    check(rs8->find_item("UltiSID 3 Filter Curve") == NULL, "another name is still not found");

    MemFile oldcfg;
    StreamTextLog log6(4096);
    oldcfg.load("[UltiSID Configuration]\nUltiSID 1 Filter Curve=8580\nreSID8 2 Filter Curve=6581 Galway\n\n");
    check(ConfigIO::S_read_from_file(&oldcfg, &log6), "a .cfg with the old names loads without error");
    check(rs8->get_value(0x90) == 3, "the old item name sets the item");
    check(rs8->get_value(0x91) == 2, "the new item name works in the same file");

    MemFile newcfg;
    ConfigIO::S_write_to_file(&newcfg);
    check(contains(newcfg.text(), "[reSID8 Configuration]"), "a .cfg is written with the new category name");
    check(contains(newcfg.text(), "reSID8 1 Filter Curve=8580"), "a .cfg is written with the new item name");
    check(!contains(newcfg.text(), "UltiSID"), "the old names are not written");

    printf("-- an overridden item (T72: a cartridge switches it off)\n");
    ConfigItem *ov = sid->find_item(0x01);
    char line[80];
    ov->setValueQuietly(1);
    ov->setOverride("Disabled (cartridge)|Disabled (cart)");
    ov->get_display_string(line, 38, 'A', 'I');
    check(contains(line, "Disabled (cartridge)") && !contains(line, "8580"), "the long text replaces the value");
    check(strstr(line, "Disabled")[-1] == 'I', "and the item is drawn in the inactive colour");
    ov->get_display_string(line, 30, 'A', 'I');
    check(contains(line, "Disabled (cart)") && !contains(line, "(cartridge)"), "a narrow row gets the short text");
    check(ov->getValue() == 1 && ov->isOverridden(), "the saved value is untouched");
    MemFile ovout;
    ConfigIO::S_write_to_file(&ovout);
    check(contains(ovout.text(), "Emulation Mode=8580"), "the .cfg still gets the saved value");
    ov->setOverride(NULL);
    ov->get_display_string(line, 38, 'A', 'I');
    check(contains(line, "8580") && strstr(line, "8580")[-1] == 'A' && !ov->isOverridden(), "clearing the override shows the value again");

    printf("-- a one-shot value (T63: a boot hotkey)\n");
    ConfigItem *os = sid->find_item(0x02);
    os->setValueQuietly(1);                 // the saved value
    sid->set_oneshot(0x02, 3);              // the hotkey
    check(sid->get_value(0x02) == 3 && os->getValue() == 3, "the effective value is the hotkey's");
    check(os->isOneShot() && os->getSavedValue() == 1, "the saved value waits unchanged");
    os->get_display_string(line, 38, 'A', 'I');
    check(contains(line, "(boot key)"), "the menu says so");
    MemFile osout;
    ConfigIO::S_write_to_file(&osout);
    check(contains(osout.text(), "8580 Lowest Filt Freq= ~45\n") && !contains(osout.text(), "Lowest Filt Freq=  100"), "the .cfg gets the saved value");
    uint8_t blob[512];
    int blen = sid->pack(blob, sizeof(blob));
    sid->set_oneshot(0x02, 2);              // a second key press keeps the first saved value
    check(os->getSavedValue() == 1, "a second one-shot does not replace the saved value");
    sid->unpack(blob, blen);                // what a flash write would have stored
    check(sid->get_value(0x02) == 1, "the flash image holds the saved value, not the hotkey's");
    sid->set_oneshot(0x02, 3);
    sid->set_value(0x01, 1);                // the user changes another item and saves
    sid->pack(blob, sizeof(blob));
    check(os->getSavedValue() == 1 && os->isOneShot(), "changing another item keeps the one-shot");
    os->setValue(2);                        // the user edits the item itself
    check(!os->isOneShot() && os->getSavedValue() == 2 && os->getValue() == 2, "an edit makes the new value the saved one");

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
