#include "lib/printk.h"
#include "lib/heap.h"
#include "display/video_fb.h"

#include "hwinit/btn.h"
#include "hwinit/hwinit.h"
#include "hwinit/di.h"
#include "hwinit/mc.h"
#include "hwinit/t210.h"
#include "hwinit/sdmmc.h"
#include "hwinit/timer.h"
#include "hwinit/util.h"
#include "hwinit/fuse.h"
#include "hwinit/kfuse.h"
#include "lib/decomp.h"
#include "lib/crc32.h"
#include "lib/ff.h"
#include "storage.h"
#include "rcm_usb.h"
#include <string.h>
#define XVERSION 1

static int initialize_mount(FATFS* outFS, u8 devNum)
{
	sdmmc_t* currCont = get_controller_for_index(devNum);
    sdmmc_storage_t* currStor = get_storage_for_index(devNum);

    if (currCont == NULL || currStor == NULL)
        return 0;

    if (currStor->sdmmc != NULL)
        return 1; //already initialized

    if (devNum == 0) //maybe support more ?
    {
        if (sdmmc_storage_init_sd(currStor, currCont, SDMMC_1, SDMMC_BUS_WIDTH_4, 11) && f_mount(outFS, "", 1) == FR_OK)
            return 1;
        else
        {
            if (currStor->sdmmc != NULL)
                sdmmc_storage_end(currStor, 0);

            memset(currCont, 0, sizeof(sdmmc_t));
            memset(currStor, 0, sizeof(sdmmc_storage_t));
        }
    }

	return 0;
}

static void deinitialize_storage()
{
    f_unmount("");
    for (u32 i=0; i<FF_VOLUMES; i++)
    {
        sdmmc_storage_t* stor = get_storage_for_index((u8)i);
        if (stor != NULL && stor->sdmmc != NULL)
        {
            if (!sdmmc_storage_end(stor, 1))
                dbg_print("sdmmc_storage_end for storage idx %u FAILED!\n", i);
            else
                memset(stor, 0, sizeof(sdmmc_storage_t));
        }
    }
}

static __attribute__((noinline)) int write_data_to_file(const char* outFilename, const unsigned char* dataSrc, size_t dataLen)
{
    FIL currFil;
    memset(&currFil, 0, sizeof(FIL));

    FRESULT retVal = f_open(&currFil, outFilename, FA_WRITE | FA_CREATE_ALWAYS);
    if (retVal != FR_OK)
    {
        printk("\nError %d opening file %s for writing\n", (int)retVal, outFilename);
        return -(int)retVal;
    }
    
    size_t totalBytesWritten = 0;
    if (dataSrc != NULL && dataLen > 0)
    {
        static const size_t FILE_CHUNK_SIZE = 4096;
        static const size_t FILE_SECTOR_SIZE = 512;
        unsigned char iramBuffer[FILE_CHUNK_SIZE+FILE_SECTOR_SIZE];
        unsigned char* chunkBuffer = (void*)ALIGN_UP((uintptr_t)iramBuffer, FILE_SECTOR_SIZE);

        size_t bytesRemaining = dataLen;
        while (bytesRemaining > 0)
        {
            UINT bytesWritten = 0;
            UINT bytesToWrite = bytesRemaining;
            if (bytesToWrite > FILE_CHUNK_SIZE)
                bytesToWrite = FILE_CHUNK_SIZE;

            memcpy(chunkBuffer, &dataSrc[totalBytesWritten], bytesToWrite);                
            retVal = f_write(&currFil, chunkBuffer, bytesToWrite, &bytesWritten);
            if (retVal != FR_OK)
            {
                printk("\nError %d writing %u bytes at offset %u to file\n", (int)retVal, bytesToWrite, totalBytesWritten);
                break;
            }
            else if (bytesWritten < bytesToWrite)
                printk("\nWarning: only %u out of %u bytes written at offset %u\n", bytesWritten, bytesToWrite, totalBytesWritten);

            totalBytesWritten += bytesWritten;
            bytesRemaining -= bytesWritten;
        }
    }

    retVal = f_close(&currFil);
    if (retVal != FR_OK)
        printk("\nError %d closing file\n", (int)retVal);

    printk("%u bytes written.\n", totalBytesWritten);
    return (int)totalBytesWritten;
}

#define IPATCH_BASE         (0x6001dc00)
#define IPATCH_SELECT       (0x0)       //bitfield of which patches are active, where 1 means first slot, 2 the 2nd, 4 the 3rd, etc
#define IPATCH_REGS_START   (0x4)       //first slot patchdata goes here, one u32 each, so next one would be at 0x8, etc
                                        //each slot patchdata u32 has format ((address >> 1) & 0xFFFF) << 16 | (data & 0xFFFF)
#define IPATCH(off)         _REG(IPATCH_BASE, off)

static uint32_t get_ipatch_slots_active()
{
    return IPATCH(IPATCH_SELECT);
}

static void set_ipatch_slots_active(uint32_t newBitmask)
{
    IPATCH(IPATCH_SELECT) = newBitmask;
}

int main(void) 
{
    u32* lfb_base;    

    config_hw();
    display_enable_backlight(0);
    display_init();

    // Set up the display, and register it as a printk provider.
    lfb_base = display_init_framebuffer();
    video_init(lfb_base);

    //Tegra/Horizon configuration goes to 0x80000000+, package2 goes to 0xA9800000, we place our heap in between.
	heap_init(0x90020000);

    printk("                                  romdump v%d by rajkosto\n", XVERSION);
    printk("\n atmosphere base by team reswitched, hwinit by naehrwert, some parts taken from coreboot\n\n");

    /* Turn on the backlight after initializing the lfb */
    /* to avoid flickering. */
    display_enable_backlight(1);

    static const uint32_t FUSE_DATA_SIZE = 0x100 * sizeof(uint32_t);
    unsigned char* fuseData = malloc(FUSE_DATA_SIZE);
    for (uint32_t i=0; i<0x100; i++)
    {
        uint32_t currWord = fuse_hw_read(i);
        memcpy(&fuseData[i*sizeof(uint32_t)], &currWord, sizeof(currWord));
    }

    static const uint32_t KFUSE_DATA_SIZE = KFUSE_NUM_WORDS * sizeof(uint32_t);
    unsigned char* kfuseData = malloc(KFUSE_DATA_SIZE);
    if (!kfuse_read((u32*)kfuseData))
    {
        free(kfuseData); 
        kfuseData = NULL;
        printk("ERROR READING KFUSE DATA\n");
    }

    static const uint32_t USB_BUFFER_SIZE = 4096;
    char* usbAlloc = malloc(USB_BUFFER_SIZE*2);
    char* usbBuffer = (void*)ALIGN_UP((uintptr_t)usbAlloc, USB_BUFFER_SIZE);
    memset(usbBuffer, 0, USB_BUFFER_SIZE);
    uint32_t numBytesUsed = 0;

    if (fuseData != NULL)
    {
        numBytesUsed += snprintfk(&usbBuffer[numBytesUsed], USB_BUFFER_SIZE-numBytesUsed-1, "FUSE DATA: ");
        for (uint32_t i=0; i<FUSE_DATA_SIZE; i++)
        {
            if ((i&15) == 0)
                usbBuffer[numBytesUsed++] = '\n';

            numBytesUsed += snprintfk(&usbBuffer[numBytesUsed], USB_BUFFER_SIZE-numBytesUsed-1,
                            "%02X", (uint32_t)fuseData[i]);
        }
        usbBuffer[numBytesUsed++] = '\n';
    }
    else
        numBytesUsed += snprintfk(&usbBuffer[numBytesUsed], USB_BUFFER_SIZE-numBytesUsed-1, "ERROR READING FUSE DATA!\n");

    if (kfuseData != NULL)
    {
        numBytesUsed += snprintfk(&usbBuffer[numBytesUsed], USB_BUFFER_SIZE-numBytesUsed-1, "KFUSE DATA: ");
        for (uint32_t i=0; i<KFUSE_DATA_SIZE; i++)
        {
            if ((i&15) == 0)
                usbBuffer[numBytesUsed++] = '\n';

            numBytesUsed += snprintfk(&usbBuffer[numBytesUsed], USB_BUFFER_SIZE-numBytesUsed-1,
                            "%02X", (uint32_t)kfuseData[i]);
        }        
        usbBuffer[numBytesUsed++] = '\n';
    }
    else
        numBytesUsed += snprintfk(&usbBuffer[numBytesUsed], USB_BUFFER_SIZE-numBytesUsed-1, "ERROR READING KFUSE DATA!\n");

    mc_enable_ahb_redirect();

    FATFS fs;
    memset(&fs, 0, sizeof(FATFS));

    if (!initialize_mount(&fs, 0))
		printk("Failed to mount SD card! Only dumping to screen/USB...\n");
    else
    {
        static const char* FUSE_OUTPUT_FILENAME = "fuse.bin";
        printk("Writing FUSE DATA to %s...", FUSE_OUTPUT_FILENAME);
        write_data_to_file(FUSE_OUTPUT_FILENAME, fuseData, FUSE_DATA_SIZE);

        static const char* KFUSE_OUTPUT_FILENAME = "kfuse.bin";
        printk("Writing KFUSE DATA to %s...", KFUSE_OUTPUT_FILENAME);
        write_data_to_file(KFUSE_OUTPUT_FILENAME, kfuseData, KFUSE_DATA_SIZE);

        static const char* BOOTROM_OUTPUT_FILENAME = "bootrom.bin";
        static const unsigned char* BOOTROM_DATA = (void*)0x00100000;
        static const size_t BOOTROM_SIZE = 96*1024;
        printk("Writing BOOTROM to %s...", BOOTROM_OUTPUT_FILENAME);

        //we want bootrom without any patches, so backup the patches used, clear them, then restore after dump
        uint32_t activeSlots = get_ipatch_slots_active(); set_ipatch_slots_active(0);
        write_data_to_file(BOOTROM_OUTPUT_FILENAME, BOOTROM_DATA, BOOTROM_SIZE);
        set_ipatch_slots_active(activeSlots);
    }

    if (fuseData != NULL)
    {
        printk("FUSE DATA: ");
        for (uint32_t i=0; i<FUSE_DATA_SIZE; i++)
        {
            if ((i&15) == 0)
                video_clear_line();

            printk("%02X ", (uint32_t)fuseData[i]);
        }
        video_clear_line();
    }
        
    if (rcm_usb_device_ready())
    {
        rcm_usb_device_write_ep1_in_sync((u8*)usbBuffer, numBytesUsed, NULL);
        rcm_usb_device_reset_ep1();
    }

    if (usbAlloc != NULL) { free(usbAlloc); usbAlloc = NULL; }
    if (kfuseData != NULL) { free(kfuseData); kfuseData = NULL; }
    if (fuseData != NULL) { free(fuseData); fuseData = NULL; }

    deinitialize_storage();
    printk("\nPress the POWER button to turn off the console.\n");
    while (btn_read() != BTN_POWER) { sleep(10000); }

    // Tell the PMIC to turn everything off
    shutdown_using_pmic();

    /* Do nothing for now */
    return 0;
}
