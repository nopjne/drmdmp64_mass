/**
 * SPX-License-Identifier: BSD-2-Clause 
 * Copyright (c) 2023 - NopJne
 * 
 * Virtual FAT16 disk construction.
 * Exposes the EEPROM, FlashRam, SRAM and catridge ROM as a file on a mass storage device.
 * Z64 are the byteflipped equivalents of the same data as the N64.
 * Filenames are separate to allow direct interaction with emulators and the savegames based on byteflipping modes.
 * ex When loading ROM.N64 the ROM.EEP will have an incorrect format for the emulator to work. 
 *    However ROMF.Z64 (RomF stands for Rom Flipped) will also have ROMF.EEP and ROMF.FLA flipped the same way as the .Z64.
 */

#include "bsp/board.h"
#include "tusb.h"
#include "n64cartinterface.h"

#if CFG_TUD_MSC

// whether host does safe-eject
static bool ejected = false;

#define CLUSTER_UP_SHIFT 0u
#define CLUSTER_UP_MUL (1u << CLUSTER_UP_SHIFT)
#define VOLUME_SIZE (CLUSTER_UP_MUL * 256u * 1024u * 1024u)
#define SECTOR_SIZE 512u
#define SECTOR_COUNT (VOLUME_SIZE / SECTOR_SIZE)

uint8_t CartTestText[2 * 1024] = 
"\nCart tester report:\n\n"
"    EEPROM    - Not present\n"
"    SRAM      - Not present\n"
"    FlashRam  - Not present\n"
"    CIC       - PAL 6102\n"
"    Romsize   - 16MB\n"
"    RomName   - Placeholder\n"
"    RomID     - 00000000\n"
"    RomRegion - Europe\n";

#define vd_sector_count() SECTOR_COUNT
enum
{
  DISK_BLOCK_NUM  = SECTOR_COUNT, // 8KB is the smallest size that windows allow to mount
  DISK_BLOCK_SIZE = 512
};

// Fri, 05 Sep 2008 16:20:51
#define RASPBERRY_PI_TIME_FRAC 100
#define RASPBERRY_PI_TIME ((16u << 11u) | (20u << 5u) | (51u >> 1u))
#define RASPBERRY_PI_DATE ((28u << 9u) | (9u << 5u) | (5u))
//#define NO_PARTITION_TABLE

#define CLUSTER_SIZE (32768u * CLUSTER_UP_MUL)
#define CLUSTER_SHIFT (6u + CLUSTER_UP_SHIFT)

static_assert(CLUSTER_SIZE == SECTOR_SIZE << CLUSTER_SHIFT, "");

#define CLUSTER_COUNT (VOLUME_SIZE / CLUSTER_SIZE)

static_assert(CLUSTER_COUNT <= 65526, "FAT16 limit");

#ifdef NO_PARTITION_TABLE
#define VOLUME_SECTOR_COUNT SECTOR_COUNT
#else
#define VOLUME_SECTOR_COUNT (SECTOR_COUNT-1)
#endif

#define FAT_COUNT 2u
#define MAX_ROOT_DIRECTORY_ENTRIES 512
#define ROOT_DIRECTORY_SECTORS (MAX_ROOT_DIRECTORY_ENTRIES * 32u / SECTOR_SIZE)

#define lsb_hword(x) (((uint)(x)) & 0xffu), ((((uint)(x))>>8u)&0xffu)
#define lsb_word(x) (((uint)(x)) & 0xffu), ((((uint)(x))>>8u)&0xffu),  ((((uint)(x))>>16u)&0xffu),  ((((uint)(x))>>24u)&0xffu)

#define SECTORS_PER_FAT (2 * (CLUSTER_COUNT + SECTOR_SIZE - 1) / SECTOR_SIZE)
static_assert(SECTORS_PER_FAT < 65536, "");

static_assert(VOLUME_SIZE >= 16 * 1024 * 1024, "volume too small for fat16");

// we are a hard drive - SCSI inquiry defines removability
#define IS_REMOVABLE_MEDIA false
#define MEDIA_TYPE (IS_REMOVABLE_MEDIA ? 0xf0u : 0xf8u)

enum partition_type {
    PT_FAT12 = 1,
    PT_FAT16 = 4,
    PT_FAT16_LBA = 0xe,
};

static const uint8_t boot_sector[] = {
        // 00 here should mean not bootable (according to spec) -- still windows unhappy without it
        0xeb, 0x3c, 0x90,
        // 03 id
        'M', 'S', 'W', 'I', 'N', '4', '.', '1',
//        'U', 'F', '2', ' ', 'U', 'F', '2', ' ',
        // 0b bytes per sector
        lsb_hword(512),
        // 0d sectors per cluster
        (CLUSTER_SIZE / SECTOR_SIZE),
        // 0e reserved sectors
        lsb_hword(1),
        // 10 fat count
        FAT_COUNT,
        // 11 max number root entries
        lsb_hword(MAX_ROOT_DIRECTORY_ENTRIES),
        // 13 number of sectors, if < 32768
#if VOLUME_SECTOR_COUNT < 65536
        lsb_hword(VOLUME_SECTOR_COUNT),
#else
        lsb_hword(0),
#endif
        // 15 media descriptor
        MEDIA_TYPE,
        // 16 sectors per FAT
        lsb_hword(SECTORS_PER_FAT),
        // 18 sectors per track (non LBA)
        lsb_hword(1),
        // 1a heads (non LBA)
        lsb_hword(1),
        // 1c hidden sectors 1 for MBR
        lsb_word(SECTOR_COUNT - VOLUME_SECTOR_COUNT),
// 20 sectors if >32K
#if VOLUME_SECTOR_COUNT >= 65536
        lsb_word(VOLUME_SECTOR_COUNT),
#else
        lsb_word(0),
#endif
        // 24 drive number
        0,
        // 25 reserved (seems to be chkdsk flag for clean unmount - linux writes 1)
        0,
        // 26 extended boot sig
        0x29,
        // 27 serial number
        0, 0, 0, 0,
        // 2b label
        'D', 'r', 'e', 'a', 'm', 'D', 'u', 'm', 'p', '6', '4',
        'F', 'A', 'T', '1', '6', ' ', ' ', ' ',
        0xeb, 0xfe // while(1);
};
static_assert(sizeof(boot_sector) == 0x40, "");

#define BOOT_OFFSET_SERIAL_NUMBER 0x27
#define BOOT_OFFSET_LABEL 0x2b

#define ATTR_READONLY       0x01u
#define ATTR_HIDDEN         0x02u
#define ATTR_SYSTEM         0x04u
#define ATTR_VOLUME_LABEL   0x08u
#define ATTR_DIR            0x10u
#define ATTR_ARCHIVE        0x20u

#define EEPROM_SIZE (32 * 1024)
#define EEPROM_CLUSTER_START 0
#define FLASHRAM_SIZE (128 * 1024)
#define FLASHRAM_CLUSTER_START (EEPROM_CLUSTER_START + (EEPROM_SIZE / CLUSTER_SIZE))
#define N64ROM_SIZE (64 * 1024 * 1024)
#define N64ROM_CLUSTER_START (FLASHRAM_CLUSTER_START + (FLASHRAM_SIZE / CLUSTER_SIZE))
#define Z64ROM_CLUSTER_START (N64ROM_CLUSTER_START + (N64ROM_SIZE / CLUSTER_SIZE))
#define FLASHRAMFLIP_CLUSTER_START (Z64ROM_CLUSTER_START + (N64ROM_SIZE / CLUSTER_SIZE))
#define EEPROMFLIP_CLUSTER_START (FLASHRAMFLIP_CLUSTER_START + (FLASHRAM_SIZE / CLUSTER_SIZE))
#define CARTTEST_CLUSTER_START (EEPROMFLIP_CLUSTER_START + (EEPROM_SIZE / CLUSTER_SIZE))

#define MBR_OFFSET_SERIAL_NUMBER 0x1b8

struct dir_entry {
    uint8_t name[11];
    uint8_t attr;
    uint8_t reserved;
    uint8_t creation_time_frac;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access_date;
    uint16_t cluster_hi;
    uint16_t last_modified_time;
    uint16_t last_modified_date;
    uint16_t cluster_lo;
    uint32_t size;
};

typedef struct _LongFileName
{
   uint8_t sequenceNo;            // Sequence number, 0xe5 for
                                  // deleted entry
   uint8_t fileName_Part1[10];    // file name part
   uint8_t fileattribute;         // File attibute
   uint8_t reserved_1;
   uint8_t checksum;              // Checksum
   uint8_t fileName_Part2[12];    // WORD reserved_2;
   uint8_t fileName_Part3[4];
   uint8_t fileName_Part4[2];
} LFN;

static_assert(sizeof(struct dir_entry) == 32, "");

// Invoked when received SCSI_CMD_INQUIRY
// Application fill vendor id, product id and revision with string up to 8, 16, 4 characters respectively
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
  (void) lun;

  const char vid[] = "DreamDmp";
  const char pid[] = "Mass Storage";
  const char rev[] = "1.0";

  memcpy(vendor_id  , vid, strlen(vid));
  memcpy(product_id , pid, strlen(pid));
  memcpy(product_rev, rev, strlen(rev));
}

// Invoked when received Test Unit Ready command.
// return true allowing host to read/write this LUN e.g SD card inserted
bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
  (void) lun;

  // RAM disk is ready until ejected
  if (ejected) {
    // Additional Sense 3A-00 is NOT_FOUND
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0x00);
    return false;
  }

  return true;
}

// Invoked when received SCSI_CMD_READ_CAPACITY_10 and SCSI_CMD_READ_FORMAT_CAPACITY to determine the disk size
// Application update block count and block size
void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size)
{
  (void) lun;

  *block_count = DISK_BLOCK_NUM;
  *block_size  = DISK_BLOCK_SIZE;
}

// Invoked when received Start Stop Unit command
// - Start = 0 : stopped power mode, if load_eject = 1 : unload disk storage
// - Start = 1 : active mode, if load_eject = 1 : load disk storage
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
  (void) lun;
  (void) power_condition;

  if ( load_eject )
  {
    if (start)
    {
      // load disk storage
    }else
    {
      // unload disk storage
      ejected = true;
    }
  }

  return true;
}

void init_dir_entry(struct dir_entry *entry, const char *fn, const char *uniname, uint32_t cluster, uint len, uint8_t attribute) {
    LFN *lfnentry = (LFN*)entry;
    memset(lfnentry, 0, sizeof(LFN));
    lfnentry->sequenceNo = 0x41;
    lfnentry->fileattribute = 0x0f;
    lfnentry->checksum = 0x0f;
    memcpy(lfnentry->fileName_Part1, uniname, sizeof(lfnentry->fileName_Part1));
    memcpy(lfnentry->fileName_Part2, uniname + sizeof(lfnentry->fileName_Part1), sizeof(lfnentry->fileName_Part2));
    memcpy(lfnentry->fileName_Part3, uniname + sizeof(lfnentry->fileName_Part1) + sizeof(lfnentry->fileName_Part2), sizeof(lfnentry->fileName_Part3));
    lfnentry->fileName_Part3[3] = 0xFF;
    lfnentry->fileName_Part3[2] = 0xFF;
    lfnentry->fileName_Part4[0] = 0xFF;
    lfnentry->fileName_Part4[1] = 0xFF;
    entry++;
    entry->creation_time_frac = RASPBERRY_PI_TIME_FRAC;
    entry->creation_time = RASPBERRY_PI_TIME;
    entry->creation_date = RASPBERRY_PI_DATE;
    entry->last_modified_time = RASPBERRY_PI_TIME;
    entry->last_modified_date = RASPBERRY_PI_DATE;
    memcpy(entry->name, fn, 11);
    entry->attr = attribute;
    entry->cluster_hi = (uint16_t)(cluster >> 16);
    entry->cluster_lo = (uint16_t)(cluster & 0xFFFF);
    entry->size = len;
}

static struct {
    uint32_t serial_number32;
    bool serial_number_valid;
} boot_device_state;

uint32_t msc_get_serial_number32() {
    if (!boot_device_state.serial_number_valid) {
        boot_device_state.serial_number32 = time_us_32();
        boot_device_state.serial_number_valid = true;
    }
    return boot_device_state.serial_number32;
}

#define min(x, y) (x < y ? x : y)
static volatile uint32_t lock = 0;
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buf, uint32_t buf_size)
{
    (void)lun;
    (void)offset;
    assert(offset == 0);
    
    if (!lba) {
        memset(buf, 0, buf_size);
        uint8_t *ptable = buf + SECTOR_SIZE - 2 - 64;
        static_assert(!((SECTOR_COUNT - 1u) >> 24), "");
        static const uint8_t _ptable_data4[] = {
                PT_FAT16_LBA, 0, 0, 0,
                lsb_word(1), // sector 1
                // sector count, but we know the MS byte is zero
                (SECTOR_COUNT - 1u) & 0xffu,
                ((SECTOR_COUNT - 1u) >> 8u) & 0xffu,
                ((SECTOR_COUNT - 1u) >> 16u) & 0xffu,
        };
        memcpy(ptable + 4, _ptable_data4, sizeof(_ptable_data4));
        ptable[64] = 0x55;
        ptable[65] = 0xaa;

        uint32_t sn = msc_get_serial_number32();
        memcpy(buf + MBR_OFFSET_SERIAL_NUMBER, &sn, 4);
        lock = 0;
        return (int32_t)512;
    }
    lba--;

    if (!lba) {
        memset(buf, 0, buf_size);
        uint32_t sn = msc_get_serial_number32();
        memcpy(buf, boot_sector, sizeof(boot_sector));
        memcpy(buf + BOOT_OFFSET_SERIAL_NUMBER, &sn, 4);

        static_assert(!((SECTOR_COUNT - 1u) >> 24), "");
        uint8_t *ptable = buf + SECTOR_SIZE - 2 - 64;
        static const uint8_t _ptable_data4[] = {
                PT_FAT16_LBA, 0, 0, 0,
                lsb_word(1), // sector 1
                // sector count, but we know the MS byte is zero
                (SECTOR_COUNT - 1u) & 0xffu,
                ((SECTOR_COUNT - 1u) >> 8u) & 0xffu,
                ((SECTOR_COUNT - 1u) >> 16u) & 0xffu,
        };
        memcpy(ptable + 4, _ptable_data4, sizeof(_ptable_data4));

        ptable[64] = 0x55;
        ptable[65] = 0xaa;

        memcpy(buf + MBR_OFFSET_SERIAL_NUMBER, &sn, 4);
    } else {
        lba--;
        if (lba < SECTORS_PER_FAT * FAT_COUNT) {
            // mirror
            while (lba >= SECTORS_PER_FAT) lba -= SECTORS_PER_FAT;
            if (!lba) {
                memset(buf, 0x00, buf_size);
                uint16_t *p = (uint16_t *) buf;
                p[0] = 0xff00u | MEDIA_TYPE;
                p[1] = 0xffff;
                p[2] = 0xffff; // cluster2 rom.eep 
                p[3] = 0x0004; // cluster[3..6] rom.fla
                p[4] = 0x0005; // cluster[3..6] rom.fla
                p[5] = 0x0006; // cluster[3..6] rom.fla
                p[6] = 0xFFFF; // cluster[3..6] rom.fla
                for (uint32_t x = 7; x < 0x100; x += 1) {
                   p[x] = (uint16_t)(x + 1); //cluster[7..807] rom.n64
                }
            } else {
              uint16_t *p = (uint16_t *) buf;
              if (lba < 8) {
                for (uint32_t x = 0; x < 0x100; x += 1) {
                    p[x] = (uint16_t)((lba * 0x100) + x + 1); //cluster[0x7..0x807] rom.n64
                }
              } else if (lba == 8) {
                for (uint32_t x = 0; x < 0x6; x += 1) {
                    p[x] = (uint16_t)((lba * 0x100) + x + 1); //cluster[7..807] rom.n64
                }
                p[6] = 0xffff;
                for (uint32_t x = 7; x < 0x100; x += 1) {
                    p[x] = (uint16_t)((lba * 0x100) + x + 1); //cluster[808..1008] rom.z64
                }
              } else if (lba < 16) {
                for (uint32_t x = 0; x < 0x100; x += 1) {
                    p[x] = (uint16_t)((lba * 0x100) + x + 1); //cluster[808..1008] rom.z64
                }
              } else if (lba == 16) {
                for (uint32_t x = 0; x < 0x06; x += 1) {
                    p[x] = (uint16_t)((lba * 0x100) + x + 1); //cluster[808..1008] rom.z64
                }
                p[6] = 0xffff;
                p[7] = (uint16_t)(0x0008 + (lba * 0x100)); // Flipped FLA.
                p[8] = (uint16_t)(0x0009 + (lba * 0x100));
                p[9] = (uint16_t)(0x000A + (lba * 0x100));
                p[10] = 0xffff;              // Flipped FLA end
                p[11] = 0xffff;              // Flipped EEPROM
                p[12] = 0xffff;              // Cart test file.
              } else {
                memset(buf, 0, buf_size);
              }
            }
        } else {
            lba -= SECTORS_PER_FAT * FAT_COUNT;
            if (lba < ROOT_DIRECTORY_SECTORS) {
                // we don't support that many directory entries actually
                if (!lba) {
                    memset(buf, 0, buf_size);
                    // root directory -- Do not use lower case letters, windows will show the file but it won't be able to "find" the data for the file.
                    struct dir_entry *entries = (struct dir_entry *) buf;
                    memcpy(entries[0].name, (boot_sector + BOOT_OFFSET_LABEL), 11);
                    entries[0].attr = ATTR_VOLUME_LABEL | ATTR_ARCHIVE;
                    uint32_t cluster_offset = 2;
                    uint32_t size = 2 * 1024;
                    assert(cluster_offset == (EEPROM_CLUSTER_START + 2));
                    init_dir_entry(++entries, "ROM     EEP", "r\0o\0m\0.\0e\0e\0p\0\0\0\0\0\0\0\0\0\0", cluster_offset, gEepromSize, 0);
                    entries++;

                    cluster_offset += (size / CLUSTER_SIZE) + 1;
                    assert(cluster_offset == (FLASHRAM_CLUSTER_START + 2));
                    size = 128 * 1024;
                    if ((gSRAMPresent != false) || (gFramPresent != false)) {
                      init_dir_entry(++entries, "ROM     FLA", "r\0o\0m\0.\0f\0l\0a\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", cluster_offset, size, 0); // DaisyDrive64 doesn't differentiate between SRAM and FRAM for filenames.
                    } else {
                      init_dir_entry(++entries, "ROM     FLA", "r\0o\0m\0.\0f\0l\0a\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", cluster_offset, 0, 0); // DaisyDrive64 doesn't differentiate between SRAM and FRAM for filenames.
                    }
                    entries++;

                    cluster_offset += size / CLUSTER_SIZE;
                    size = (64 * 1024 * 1024);
                    assert(cluster_offset == (N64ROM_CLUSTER_START + 2));
                    init_dir_entry(++entries, "ROM     N64", "r\0o\0m\0.\0n\0\\6\0\\4\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", cluster_offset, gRomSize, ATTR_READONLY);
                    entries++;

                    cluster_offset += size / CLUSTER_SIZE;
                    size = (64 * 1024 * 1024);
                    assert(cluster_offset == (Z64ROM_CLUSTER_START + 2));
                    init_dir_entry(++entries, "ROMF    Z64", "r\0o\0m\0.\0z\0\\6\0\\4\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", cluster_offset, gRomSize, ATTR_READONLY); // Same as N64 just byteflipped.
                    entries++;

                    cluster_offset += size / CLUSTER_SIZE;
                    size = 128 * 1024;
                    assert(cluster_offset == (FLASHRAMFLIP_CLUSTER_START + 2));
                    if ((gSRAMPresent != false) || (gFramPresent != false)) {
                      if (gFramPresent != false) {
                        init_dir_entry(++entries, "ROMF    FLA", "R\0O\0M\0F\0.\0f\0l\0a\0s\0h\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0" , cluster_offset, size, 0); // Same as N64 just byteflipped, fla for Ares emulator support.
                      } else {
                        init_dir_entry(++entries, "ROMF    RAM", "R\0O\0M\0F\0.\0r\0a\0m\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", cluster_offset, size, 0); // Same as N64 just byteflipped, ram for Ares emulator support.
                      }
                    } else {
                      init_dir_entry(++entries, "ROMF    FLA", "R\0O\0M\0F\0.\0f\0l\0a\0s\0h\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", cluster_offset, 0, 0); // Same as N64 just byteflipped, ram for Ares emulator support.
                    }
                    entries++;

                    cluster_offset += size / CLUSTER_SIZE;
                    size = 2 * 1024;
                    assert(cluster_offset == (EEPROMFLIP_CLUSTER_START + 2));
                    if (gEepromSize != 0) {
                      init_dir_entry(++entries, "ROMF    EEP", "R\0O\0M\0F\0.\0e\0e\0p\0r\0o\0m\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", cluster_offset, gEepromSize, 0);
                      entries++;
                    }

                    cluster_offset += (size / CLUSTER_SIZE) + 1;
                    size = 2 * 1024;
                    assert(cluster_offset == (CARTTEST_CLUSTER_START + 2));
                    init_dir_entry(++entries, "CARTTESTTXT", "c\0a\0r\0t\0t\0e\0s\0t\0.\0t\0x\0t\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", cluster_offset, size, ATTR_READONLY);
                    entries++;
                } else {
                  memset(buf, 0, buf_size);
                }
            } else {
                lba -= ROOT_DIRECTORY_SECTORS;
                uint cluster = lba >> CLUSTER_SHIFT;
                uint cluster_offset = lba - (cluster << CLUSTER_SHIFT);
                {
                  // Lookup cluster by entry
                  if (cluster == CARTTEST_CLUSTER_START) {
                    memset(buf, 0, SECTOR_SIZE);
                      if (cluster_offset == 0) {
                        const char* NotPresent = "Not present";
                        const char* Failed = "Failed";
                        const char* OK = "OK!";
                        const char* PAL = "PAL";
                        const char* NTSC = "NTSC";
                        const char* EepString = Failed;
                        const char* CICString = Failed;
                        if (gEepromSize == 0) {
                          EepString = NotPresent;
                        } else if (gEepromSize == 0x200) {
                          EepString = "4K OK!";
                        } else if (gEepromSize == 0x800) {
                          EepString = "16K OK!";
                        }
                        if (gCICType == CIC_TYPE_INVALID) {
                          CICString = Failed;
                        } else if (gCICType == CIC_TYPE_PAL) {
                          CICString = PAL;
                        } else if (gCICType == CIC_TYPE_NTSC) {
                          CICString = NTSC;
                        }

                        sprintf(buf,
                        "\nCart tester report:\n\n"
                        "    EEPROM     - %s\n"
                        "    SRAM       - %s\n"
                        "    FlashRam   - %s (%02X)\n"
                        "    CIC        - %s %s\n"
                        "    Romsize    - %luMB\n"
                        "    RomName    - %s\n"
                        "    RomID      - %04X %c%c\n"
                        "    CartType   - %c\n"
                        "    RomRegion  - %c\n"
                        "    RomVersion - %02X\n",
                        EepString,
                        (gSRAMPresent != 0) ? OK : NotPresent,
                        (gFramPresent != 0) ? OK : NotPresent, gFlashType,
                        CICString,
                        gCICName,
                        (gRomSize / (1024 * 1024)),
                        (char*)gGameTitle,
                        gGameCode[1], ((gGameCode[1] >> 8) & 0xFF), (gGameCode[1] & 0xFF),
                        gGameCode[0] & 0xFF,
                        ((gGameCode[2] >> 8) & 0xFF),
                        (gGameCode[2] & 0xFF)
                        );
                      } else {
                        memset(buf, 0, SECTOR_SIZE);
                      }
                  } else if (cluster == EEPROMFLIP_CLUSTER_START) {
                      uint32_t address = (((uint32_t)cluster - (EEPROMFLIP_CLUSTER_START)) * CLUSTER_SIZE) + (cluster_offset * SECTOR_SIZE);
                      ReadEepromData(address / 64, buf);
                  } else if (cluster >= FLASHRAMFLIP_CLUSTER_START) {
                      // Read SRAM/FRAM -- check if the cart responds to Flashram info request first, if not treat as SRAM.
                      // Also support Dezaemon's banked SRAM.
                      uint32_t address = (((uint32_t)cluster - (FLASHRAMFLIP_CLUSTER_START)) * CLUSTER_SIZE) + (cluster_offset * SECTOR_SIZE);
                      if (gFramPresent != 0) {
                        FlashRamRead512B(address, (uint16_t*)buf, true);
                      } else {
                        SRAMRead512B(address, (uint16_t*)buf, true);
                      }
                  } else if (cluster >= Z64ROM_CLUSTER_START) {
                      // Read Z64 rom
                      uint32_t address = (((uint32_t)cluster - (Z64ROM_CLUSTER_START)) * CLUSTER_SIZE) + (cluster_offset * SECTOR_SIZE);
                      address += 0x10000000;
                      set_address(address);
                      for (uint32_t i = 0; i < 256; i += 1) {
                          ((uint16_t*)buf)[i] = flip16(read16());
                      }
                  } else if (cluster >= N64ROM_CLUSTER_START) {
                      // Read N64 rom
                      volatile uint32_t n64romstart = N64ROM_CLUSTER_START;
                      n64romstart = n64romstart;
                      uint32_t address = (((uint32_t)cluster - (N64ROM_CLUSTER_START)) * CLUSTER_SIZE) + (cluster_offset * SECTOR_SIZE);
                      address += 0x10000000;
                      uint32_t written = 0;
                      while (buf_size) {
                        set_address(address);
                        for (uint32_t i = 0; i < 256; i += 1) {
                            ((uint16_t*)buf)[i + (((uint32_t)written)/2)] = read16();
                        }
                        buf_size -= 512;
                        address += 512;
                        written += 512;
                      }
                  } else if (cluster >= FLASHRAM_CLUSTER_START) {
                      // Read SRAM/FRAM -- check if the cart responds to Flashram info request first, if not treat as SRAM.
                      // Also support Dezaemon's banked SRAM.
                      uint32_t address = (((uint32_t)cluster - (FLASHRAM_CLUSTER_START)) * CLUSTER_SIZE) + (cluster_offset * SECTOR_SIZE);
                      if (gFramPresent != 0) {
                        FlashRamRead512B(address, (uint16_t*)buf, false);
                      } else {
                        SRAMRead512B(address, (uint16_t*)buf, true);
                      }

                  } else if (cluster == EEPROM_CLUSTER_START) {
                      uint32_t address = (((uint32_t)cluster - (FLASHRAM_CLUSTER_START)) * CLUSTER_SIZE) + (cluster_offset * SECTOR_SIZE);
                      ReadEepromData(address / 64, buf);
                  }
                }
            }
        }
    }

    return (int32_t)512;
}

//#define CFG_EXAMPLE_MSC_READONLY
bool tud_msc_is_writable_cb (uint8_t lun)
{
  (void) lun;

#ifdef CFG_EXAMPLE_MSC_READONLY
  return false;
#else
  return true;
#endif
}

// Callback invoked when received WRITE10 command.
// Process data in buffer to disk's storage and return number of written bytes
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,  uint8_t* buffer, uint32_t bufsize)
{
  (void) lun;

  // out of ramdisk
  if ( lba >= DISK_BLOCK_NUM ) return -1;

  (void)lun;
    (void)offset;
    assert(offset == 0);
    if (!lba) {
       return 512; // Not writable.
    }
    lba--;

    if (!lba) {
        return 512; // Not writable.
    } else {
        lba--;
        if (lba < SECTORS_PER_FAT * FAT_COUNT) {
            return 512; // Not writable.
        } else {
            lba -= SECTORS_PER_FAT * FAT_COUNT;
            if (lba < ROOT_DIRECTORY_SECTORS) {
                // we don't support that many directory entries actually
                return 512; // Not writable.
            } else {
                lba -= ROOT_DIRECTORY_SECTORS;
                uint cluster = lba >> CLUSTER_SHIFT;
                uint cluster_offset = lba - (cluster << CLUSTER_SHIFT);
                {
                  // Lookup cluster by entry
                  if (cluster == CARTTEST_CLUSTER_START) {
                        return 512; // Not writable.
                  } else if (cluster == EEPROMFLIP_CLUSTER_START) {
                      uint32_t address = (((uint32_t)cluster - (EEPROMFLIP_CLUSTER_START)) * CLUSTER_SIZE) + (cluster_offset * SECTOR_SIZE);
                      WriteEepromData(address / 64, buffer);
                  } else if ((cluster >= FLASHRAMFLIP_CLUSTER_START) && (cluster < FLASHRAMFLIP_CLUSTER_START + 4)) {
                      // Read SRAM/FRAM -- check if the cart responds to Flashram info request first, if not treat as SRAM.
                      // Also support Dezaemon's banked SRAM.
                      uint32_t address = (((uint32_t)cluster - (FLASHRAMFLIP_CLUSTER_START)) * CLUSTER_SIZE) + (cluster_offset * SECTOR_SIZE);

                      if (gFramPresent != 0) {
                        FlashRamWrite512B(address, buffer, true);

                      } else {
                        address += 0x08000000;
                        SRAMWrite512B(address, buffer, true);
                      }
                  } else if (cluster >= Z64ROM_CLUSTER_START) {
                      return 512; // Read only. 
                  } else if (cluster >= N64ROM_CLUSTER_START) {
                      return 512; // Read only.
                  } else if ((cluster >= FLASHRAM_CLUSTER_START) && ((cluster < (FLASHRAM_CLUSTER_START + 4)))) {
                      // Read SRAM/FRAM -- check if the cart responds to Flashram info request first, if not treat as SRAM.
                      // TODO: support Dezaemon's banked SRAM.
                      uint32_t address = (((uint32_t)cluster - (FLASHRAM_CLUSTER_START)) * CLUSTER_SIZE) + (cluster_offset * SECTOR_SIZE);
                      if (gFramPresent != 0) {
                        FlashRamWrite512B(address, buffer, false);
                      } else {
                        address += 0x08000000;
                        SRAMWrite512B(address, buffer, false);
                      }

                  } else if (cluster == EEPROM_CLUSTER_START) {
                      uint32_t address = (((uint32_t)cluster - (FLASHRAM_CLUSTER_START)) * CLUSTER_SIZE) + (cluster_offset * SECTOR_SIZE);
                      WriteEepromData(address / 64, buffer);
                  }
                }
            }
        }
    }

  return (int32_t) bufsize;
}

// Callback invoked when received an SCSI command not in built-in list below
// - READ_CAPACITY10, READ_FORMAT_CAPACITY, INQUIRY, MODE_SENSE6, REQUEST_SENSE
// - READ10 and WRITE10 has their own callbacks
int32_t tud_msc_scsi_cb (uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize)
{
  // read10 & write10 has their own callback and MUST not be handled here

  void const* response = NULL;
  int32_t resplen = 0;

  // most scsi handled is input
  bool in_xfer = true;

  switch (scsi_cmd[0])
  {
    default:
      // Set Sense = Invalid Command Operation
      tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);

      // negative means error -> tinyusb could stall and/or response with failed status
      resplen = -1;
    break;
  }

  // return resplen must not larger than bufsize
  if ( resplen > bufsize ) resplen = bufsize;

  if ( response && (resplen > 0) )
  {
    if(in_xfer)
    {
      memcpy(buffer, response, (size_t) resplen);
    }else
    {
      // SCSI output
    }
  }

  return (int32_t) resplen;
}

#endif
