/*
 * spi_mdm6600.c -- Serial peheripheral interface framing layer for MDM6600 modem.
 *
 * Copyright (C) 2009 Texas Instruments
 * Authors: Umesh Bysani <bysani@ti.com> and
 *      Shreekanth D.H <sdh@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <asm/uaccess.h>
#include <linux/irq.h>
#include <mach/gpio.h>
//#include <plat/omap-pm.h> :  need to max of vdd2 in dma tx
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/spi/spi.h>
#include <linux/workqueue.h>
#include <linux/earlysuspend.h>
#include <linux/spinlock.h>
#include <linux/io.h>
#include <linux/spi/mdm_6600_spi.h>
#include <linux/delay.h> 
#include <linux/proc_fs.h>
#include <linux/uaccess.h>  // copy_from/to_user()

#include <linux/io.h>
#include "../mux.h"

#include <linux/wakelock.h>

#define CONFIG_EARLY_SUSPEND_TEST
#define SPI_LOG_ENABLE_SHIM

#define WAKE_LOCK_RESUME
#define SPI2SPI_TEST

#define MSPI_DRIVER_NAME                            "mdm6600"
#define MSPI_DRIVER_VERSION                         "23-mar-12"

#define MSPI_DRV_DEBUG_LEVEL                        10
#define MSPI_EXTENDED_DEBUG_OUTPUT_FORMAT

#define MSPI_MAX_RX_FRAME_BUFS                      10
#define MSPI_DATA_TABLE_SIZE                        1
#define MSPI_DEFAULT_TABLE_ENTRY                    0

#define MSPI_WAKE_LOCK_TIMEOUT                      msecs_to_jiffies(600)

// AP_SLEEP_CHK [LGE-SPI]
#if defined(CONFIG_PRODUCT_LGE_HUB)
#define OMAP_MODEM_WAKE 65
#elif defined(CONFIG_PRODUCT_LGE_BLACK)
#define OMAP_MODEM_WAKE 65
#elif defined(CONFIG_PRODUCT_LGE_LU6800)
#define OMAP_MODEM_WAKE 65
#else
#error need OMAP_MODEM_WAKE define.
#endif
/*------------------------------------------------------------------*/
/* Driver Name and Version                                          */
/*------------------------------------------------------------------*/

static const char mspi_drv_driver_name[]            = MSPI_DRIVER_NAME;
static const char mspi_drv_driver_version[]         = MSPI_DRIVER_VERSION;

/*------------------------------------------------------------------*/
/* END Driver Name and Version                                      */
/*------------------------------------------------------------------*/

/* Global variable used to trigger system shutdown is started */
static int ifx_shutdown                             = 0;

/* ################################################################################################################ */

/* Structure used to store private data */
struct ifx_spi_data 
{
    dev_t           devt;
    struct spi_device           *spi;
    struct tty_struct           *ifx_tty;

/* users shows how much spi devices was opened from user space. In current implementation it should be not greater then 1*/
    unsigned int                 users;
    struct work_struct           ifx_work;
    struct workqueue_struct     *ifx_wq;

    unsigned int                 ifx_receiver_buf_size;
    unsigned int                 ifx_receiver_more_data;
    unsigned char               *ifx_tx_buffer;
    unsigned char               *ifx_rx_buffer;
#ifdef WAKE_LOCK_RESUME
    struct wake_lock             wake_lock;
#endif
    unsigned                     mrdy_gpio;
    unsigned                     srdy_gpio;
    struct work_struct           ifx_free_irq_work;
};

/*spi_data_table is consist of 1 spi_data for each mcspi port */
struct ifx_spi_data           *spi_data_table[IFX_N_SPI_MINORS];
struct tty_driver             *ifx_spi_tty_driver;


/*------------------------------------------------------------------*/
/* Debug                                                            */
/*------------------------------------------------------------------*/
#ifdef MSPI_DRV_DEBUG_LEVEL
static unsigned int debug_level                     = 0;
#endif
#ifdef MSPI_EXTENDED_DEBUG_OUTPUT_FORMAT
#define MSPI_DEBUG_OUTPUT_FORMAT                    MSPI_DRIVER_NAME "(" MSPI_DRIVER_VERSION ")"
#else
#define MSPI_DEBUG_OUTPUT_FORMAT                    MSPI_DRIVER_NAME
#endif
#define MSPI_ERR(msg,args...)                       {printk(KERN_ERR MSPI_DEBUG_OUTPUT_FORMAT ":%s: " msg "\n",__func__, ## args);}
#ifdef MSPI_DRV_DEBUG_LEVEL
#define MSPI_ISDBG(level)                           (((level) <= MSPI_DRV_DEBUG_LEVEL) && ((level) <= debug_level))
#define MSPI_DBG(level,msg,args...)                 { if (MSPI_ISDBG(level)) {printk(KERN_INFO MSPI_DEBUG_OUTPUT_FORMAT ": " msg "\n", ## args);}; }
// Unconditional
#define UNMSPI_DBG(msg,args...)                     { {printk(KERN_INFO MSPI_DEBUG_OUTPUT_FORMAT ": " msg "\n", ## args);}; }
#define MSPI_DRV_DEBUG_FS_LEVEL
#else
#define MSPI_ISDBG(level)                           0
#define MSPI_DBG(level,msg,args...)
#endif

static volatile unsigned int mspi_frame_tx_count    = 0;
/*------------------------------------------------------------------*/
/* END Debug                                                        */
/*------------------------------------------------------------------*/


/*------------------------------------------------------------------*/
/* TX: send from AP to CP                                           */
/*------------------------------------------------------------------*/
#ifdef MSPI_EXTENDED_HEADER
#define MSPI_MAX_TX_FRAME_BUFS                      6
#else
#define MSPI_MAX_TX_FRAME_BUFS                      8
#endif

static u8 *mspi_tx_buffer_start                     = NULL;
static u8 *mspi_tx_buffer_end                       = NULL;
static u8 *mspi_tx_buffer_pos                       = NULL;
static u8 *mux_tx_buffer_pos                        = NULL;
static u8 *mspi_tx_buffer_backup                    = NULL;


/* shows how much data present in the tx buffer */
static unsigned int mspi_tx_buffer_data_count       = 0;

/* watermarks used for upper TTY client throttling */
#define MSPI_TX_BUFFER_WTRMK_HIGH                   (((MSPI_MAX_TX_FRAME_BUFS - 1) * MSPI_SPI_FULL_FRAME_LENGTH) - IFX_SPI_HEADER_SIZE)
#define MSPI_TX_BUFFER_WTRMK_LOW                    (MSPI_SPI_FULL_FRAME_LENGTH * 2)

static spinlock_t spi_nodes_lock                    = SPIN_LOCK_UNLOCKED;

/* This falg is used to trigger the necessity to wakeup tty client. It becomes 1 after TX High watermark is reached
   Later, when the buffer is processed and reaches Low watermark point, the flag clears to 0, and tty client wakes up */
static u8 mspi_tty_client_wakeup                    = 0;
/*------------------------------------------------------------------*/
/* END TX: send from AP to CP                                       */
/*------------------------------------------------------------------*/


/*------------------------------------------------------------------*/
/* RX: recv from CP to AP                                           */
/*------------------------------------------------------------------*/
// MSPI_MAX_RX_FRAME_BUFS > 5

static unsigned char *mspi_rx_buffer_start          = NULL;
static unsigned char *mspi_rx_buffer_end            = NULL;
static unsigned int mux_rx_buffer_curr_size         = 0;
#define MSPI_RX_BUFFER_SIZE                         (MSPI_MAX_RX_FRAME_BUFS * MSPI_SPI_FULL_FRAME_LENGTH)
#define MSPI_RX_BUFFER_WTRMK_HIGH                   ((MSPI_MAX_RX_FRAME_BUFS - 1) * MSPI_SPI_FULL_FRAME_LENGTH)
#define MSPI_RX_BUFFER_WTRMK_LOW                    (MSPI_SPI_FULL_FRAME_LENGTH)
static spinlock_t mux_rx_buff_lock                  = SPIN_LOCK_UNLOCKED;

static struct semaphore mspi_rx_completion          = __SEMAPHORE_INITIALIZER(mspi_rx_completion, 0);

#define MSPI_CAN_RECEIVE                            ( (rts_mspi_data_flag >= 1) && \
                                                      (mux_rx_buffer_curr_size < MSPI_RX_BUFFER_WTRMK_LOW) )

#define MSPI_CANNOT_RECEIVE                         ( (rts_mspi_data_flag == 0) && \
                                                      (mux_rx_buffer_curr_size >= MSPI_RX_BUFFER_WTRMK_HIGH) )

static struct workqueue_struct *mspi_rx_wq          = NULL;
struct mspi_rx_work
{
    struct work_struct      work;
    unsigned char          *mspi_rx_buffer;
    struct ifx_spi_data    *spi_data;
    struct tty_struct      *ifx_tty;
    int                     len;
};


/*------------------------------------------------------------------*/
/* END RX: recv from CP to AP                                       */
/*------------------------------------------------------------------*/

/*------------------------------------------------------------------*/
/* SPI SYNC & COUNT                                                 */
/*------------------------------------------------------------------*/
#define MSPI_DMA_ALIGN                              16

static int mspi_data_tx_next_len                    = 0;
static int mspi_data_rx_next_len                    = 0;

/* this variable is used to hold the DMA buffer size that will be used in next transfer
   Typically it is MAX of the TX and RX buffers that are planned for the next transfer
   However in case if both TX and RX are going to be 0 we sometimes */
static int mspi_curr_dma_data_len                   = SPI_MAX_PACKET_LEN;
static u8 cts_mspi_data_flag                        = 0;
static volatile u8 rts_mspi_data_flag               = 0;

static int is_tx_rx_required                        = 0;
/*------------------------------------------------------------------*/
/* END SPI SYNC & COUNT                                             */
/*------------------------------------------------------------------*/

/*------------------------------------------------------------------*/
/* SPI2SPI TEST MODE                                                */
/*------------------------------------------------------------------*/
#ifdef SPI2SPI_TEST
#define TIOCSPI2SPISTART                            0x5501
#define TIOCSPI2SPISTOP                             0x5502
#define TIOCGSTAT                                   0x5503

static uint64_t spi2spi_counter;
static spinlock_t spi2spi_lock                      = SPIN_LOCK_UNLOCKED;

static u8 spi2spi_test_is_running                   = 0;

static u8 *spi2spi_tx_buff                          = NULL;
static u8 *spi2spi_rx_buff                          = NULL;
static u16 spi2spi_buff_size                        = MSPI_SPI_FULL_FRAME_LENGTH;

struct spi2spi_args {
    int         testcase;
    int         xfer_size;
    int         pattern;
};

struct spi2spi_stat {
    uint64_t         counter;
    uint64_t         timestamp;
};

struct spi2spi_args s2s_test;

static int spi2spi_start_test(struct ifx_spi_data *spi_data);
static void ifx_spi2spi_handle_work(struct work_struct *work);
#endif //SPI2SPI_TEST

/*------------------------------------------------------------------*/
/* END SPI2SPI TEST MODE                                                                              */
/*------------------------------------------------------------------*/


/*------------------------------------------------------------------*/
/* IOCTL                                                                                                    */
/*------------------------------------------------------------------*/

static volatile int mspi_flush_rx               = 0;
static DEFINE_MUTEX(mspi_transfer_lock);
static DEFINE_MUTEX(mspi_rx_task_lock);

/*------------------------------------------------------------------*/
/* END IOCTL                                                                                                */
/*------------------------------------------------------------------*/
/* ################################################################################################################ */
/* Global Declarations */
/* Function Declarations */
// MSPI Header functions
static void ifx_spi_header_set_info(unsigned char *header_buffer, unsigned int curr_buf_size, unsigned int next_buf_size);
static void ifx_spi_header_get_info(unsigned char *header_buffer, unsigned int *curr_buf_size, unsigned int *next_buf_size);
static void ifx_spi_header_update_curr_size(unsigned char *header_buffer, unsigned int curr_buf_size);
static void ifx_spi_header_update_next_size(unsigned char *header_buffer, unsigned int next_buf_size);
static inline int ifx_spi_header_get_more_flag(unsigned char *rx_buffer);
static inline int ifx_spi_header_get_cts_flag(unsigned char *rx_buffer);

static void ifx_spi_ap_ready(struct spi_device *spi, int enable);
static irqreturn_t ifx_spi_handle_mrdy_irq(int irq, void *handle);
static void ifx_spi_sync_data(struct ifx_spi_data *spi_data);
static int ifx_spi_allocate_frame_memory(struct ifx_spi_data *spi_data, unsigned int memory_size);
static void ifx_spi_free_frame_memory(struct ifx_spi_data *spi_data);
static void ifx_spi_buffer_initialization(struct ifx_spi_data *spi_data);
static void ifx_spi_handle_work(struct work_struct *work);
static void ifx_spi_handle_free_irq_work(struct work_struct *work);

// QUEUE
static int mspi_queue_data_insert_item(const unsigned char *buf, int count);
static void mspi_queue_data_setup_spi_tx_buf(struct ifx_spi_data *spi_data);
static int mspi_queue_empty(void);

static void mspi_dump_compilation_info(void);

static void mspi_tx_buffer_flush(struct ifx_spi_data *spi_data);
static void mspi_rx_buffer_flush(struct ifx_spi_data *spi_data);

/* ################################################################################################################ */

static void inline dump_spi_simple(u8 *data)
{
    UNMSPI_DBG("TS spi dump %x %x %x %x %x %x %x %x \n", data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
}

static void
mspi_dump_spi_header_info(unsigned char *buffer, int is_rx)
{
    struct ifx_spi_frame_header *header = (struct ifx_spi_frame_header *)buffer;

	if(is_rx == 0)
	{
		UNMSPI_DBG("TX c%d m%d n%d cts%d\n",
			header->curr_data_size,
			header->more,
			header->next_data_size,
			header->cts_rts);
	}
	else
	{
		UNMSPI_DBG("RX c%d m%d n%d cts%d\n",
			header->curr_data_size,
			header->more,
			header->next_data_size,
			header->cts_rts);
	}
}

#ifdef MSPI_DRV_DEBUG_FS_LEVEL
/*--------------------------------------------------------------------
 * BEGINS: Proc file system related DBG functionality for SPI.
 *--------------------------------------------------------------------
 */
#define MSPI_PROC_DBG_FILE                          "driver/mspi_dbg_level"
#define MSPI_DBG_PROC_BUF_MAX_LEN                   3UL

static struct proc_dir_entry *mspi_proc_dbg_file;

static int mspi_proc_dbg_read(char *page, char **start, off_t off, int count, int *eof, void *data)
{
    int len;

    MSPI_DBG(8, "%s called, curr debug_level = %d\n", __FUNCTION__, debug_level);
    len = sprintf(page, "%d\n", debug_level);

    return len;

}

static int mspi_proc_dbg_write(struct file *file, const char __user *buffer, unsigned long count, void *data)
{
    char buff[MSPI_DBG_PROC_BUF_MAX_LEN];
    int val;
    unsigned long len = min(MSPI_DBG_PROC_BUF_MAX_LEN, count);
    MSPI_DBG(8, "%s called, curr debug_level = %u, passed count %lu\n", __FUNCTION__, debug_level, count);

    if(copy_from_user(buff, buffer, len))
                return -EFAULT;

    buff[len-1] = '\0';
    val = simple_strtoul(buff, NULL, 10);

    if(val >= 0 && val <= MSPI_DRV_DEBUG_LEVEL)
    {
        debug_level = (unsigned int)val;
        MSPI_DBG(8, "%s: updated debug_level = %u\n", __FUNCTION__, debug_level);
    }

    return len;
}

static void create_mspi_proc_dbg_file(void)
{
    mspi_proc_dbg_file = create_proc_entry(MSPI_PROC_DBG_FILE, 0664, NULL);

    if(mspi_proc_dbg_file != NULL)
    {
        mspi_proc_dbg_file->read_proc = mspi_proc_dbg_read;
        mspi_proc_dbg_file->write_proc = mspi_proc_dbg_write;
    }
    else
        MSPI_ERR(" MSPI DBG proc file create failed!\n");
}

static void remove_mspi_proc_dbg_file(void)
{
    remove_proc_entry(MSPI_PROC_DBG_FILE, NULL);
}

#endif //MSPI_DRV_DEBUG_LEVEL

static inline void mspi_dump_compilation_info(void)
{
    MSPI_DBG(0," compilled with \n\t def_pkt_size=%d \n\t max_pkt_size=%d\n\t TX_buf_size=%d\n\t RX_buf_size=%d\n",
        IFX_SPI_DEFAULT_BUF_SIZE+IFX_SPI_HEADER_SIZE, MSPI_SPI_FULL_FRAME_LENGTH,
        MSPI_SPI_FULL_FRAME_LENGTH * (MSPI_MAX_TX_FRAME_BUFS + 2), MSPI_RX_BUFFER_SIZE);
}
/* IFX SPI Operations */

/*
 * Function opens a tty device when called from user space
 */
static int
ifx_spi_open(struct tty_struct *tty, struct file *filp)
{
    struct ifx_spi_data *spi_data;

    MSPI_DBG(0," ifx_spi_open called");
    ifx_shutdown = 0;
    mspi_tty_client_wakeup = 0;

    switch (tty->index)
    {

        case 0: /* ttyspi0 */
        spi_data = spi_data_table[1];
        break;
        
        case 1: /* ttyspi1 */
        spi_data = spi_data_table[MSPI_DEFAULT_TABLE_ENTRY];
        break;
        
        default:
        spi_data = spi_data_table[MSPI_DEFAULT_TABLE_ENTRY];
        break;
    }

    if(spi_data == NULL)
    {
        MSPI_ERR(" ifx_spi_open spi_data NULL - tty->index: %d \n", tty->index );
        return -ENODEV;
    }

    ifx_spi_buffer_initialization(spi_data);
    if (spi_data->users++ == 0)
    {
        tty->driver_data = (void *)spi_data;
        spi_data->ifx_tty = tty;
    }

    tty->low_latency = 1;

    return 0;
}

/*
 * Function closes a opened a tty device when called from user space
 */
static void
ifx_spi_close(struct tty_struct *tty, struct file *filp)
{
    struct ifx_spi_data *spi_data = (struct ifx_spi_data *)tty->driver_data;

    MSPI_DBG(7, "%s called mdm\n", __FUNCTION__);

    // Prohibit the writes while mSPI is shutting down
    ifx_shutdown = 1;

    if((spi_data != NULL) && (spi_data->users != 0))
    {
        if(--spi_data->users == 0)
        {
            tty_driver_flush_buffer(tty);
            spi_data->ifx_tty = NULL;
            tty->driver_data = NULL;
        }
    }
}
/*
 * Function is called from user space to send data to MODEM, it setups the transmission, enable MRDY signal and
 * waits for SRDY signal HIGH from MDOEM. Then starts transmission and reception of data to and from MODEM.
 * Once data read from MODEM is transferred to TTY core flip buffers, then "ifx_read_write_completion" is set
 * and this function returns number of bytes sent to MODEM
 */


static int
ifx_spi_write(struct tty_struct *tty, const unsigned char *buf, int count)
{
    int ret;
    struct ifx_spi_data *spi_data = (struct ifx_spi_data *)tty->driver_data;

    if (ifx_shutdown) return -ENODEV;

    if(spi_data == NULL || buf == NULL || count <= 0) return 0;

#ifdef SPI2SPI_TEST
    spin_lock_bh(&spi2spi_lock);
    if (spi2spi_test_is_running == 1)
    {
        MSPI_ERR("ifx_spi_write: Device is unavailable during spi to spi test");
        spin_unlock_bh(&spi2spi_lock);
        return -EBUSY;
    }
    spin_unlock_bh(&spi2spi_lock);
#endif

    ret = mspi_queue_data_insert_item(buf, count);
#ifdef WAKE_LOCK_RESUME
    wake_lock_timeout(&spi_data_table[spi_data->spi->master->bus_num - 1]->wake_lock, MSPI_WAKE_LOCK_TIMEOUT);
#endif
    queue_work(spi_data->ifx_wq, &spi_data->ifx_work);
    return ret;
}

/* This function should return number of free bytes left in the write buffer in this case always return 2048 */

static int
ifx_spi_write_room(struct tty_struct *tty)
{
    if(mspi_tty_client_wakeup == 1)
    {
        return 0;
    }

    return (MSPI_TX_BUFFER_WTRMK_HIGH - mspi_tx_buffer_data_count);
}

static int
ifx_spi_chars_in_buffer(struct tty_struct *tty)
{
    return mspi_tx_buffer_data_count;
}

static void
ifx_spi_throttle(struct tty_struct *tty)
{
    MSPI_DBG(3, "throttle");
}

static void
ifx_spi_unthrottle(struct tty_struct *tty)
{
    MSPI_DBG(3, "unthrottle");

    up(&mspi_rx_completion);
    down_trylock(&mspi_rx_completion);
}


static void
ifx_spi_flush_buffer(struct tty_struct *tty)
{
  struct ifx_spi_data *spi_data = (struct ifx_spi_data *)tty->driver_data;

  if (spi_data != NULL)
  {
    MSPI_DBG(5, "flush buffer on mSPI");

    mspi_rx_buffer_flush(spi_data);
    mspi_tx_buffer_flush(spi_data);
  }
}

static void
ifx_spi_hangup(struct tty_struct *tty)
{
    MSPI_DBG(5, "hangup on mSPI");

    tty_wakeup(tty);
    tty_ldisc_flush(tty);
    //TODO close
}

static void
ifx_spi_set_ldisc(struct tty_struct *tty)
{
    MSPI_DBG(5, "ldisc changed on mSPI");
}


static int
ifx_spi_tiocmget(struct tty_struct *tty /*, struct file *file*/)
{
    int status = 0;

    if(cts_mspi_data_flag != 0)
        status |= TIOCM_CTS;

    if(rts_mspi_data_flag != 0)
        status |= TIOCM_RTS;

    return status;
}

/*
This function copies external data from buf to internal tx buffer.
It tackes care of tx cycle filling in case if end of the buffer is reached.
As well as intrenal position variables calculating.
*/
static int mspi_queue_data_insert_item(const unsigned char *buf, int count)
{
    int len = 0;
    int remain_len = 0;

    spin_lock_bh(&spi_nodes_lock);

    if((mspi_tty_client_wakeup == 1) || ((mspi_tx_buffer_data_count + count) > MSPI_TX_BUFFER_WTRMK_HIGH))
    {
            mspi_tty_client_wakeup = 1;
            MSPI_DBG(6,"TS qdi failed \n");
            spin_unlock_bh(&spi_nodes_lock);
            return 0;
    }

    if((remain_len = (mux_tx_buffer_pos + count) - mspi_tx_buffer_end) < 0)
    {
        len = count;
        remain_len = 0;
    }
    else
    {
        len = count - remain_len;
    }

    memcpy(mux_tx_buffer_pos, buf , len);
    if(remain_len == 0)
    {
        mux_tx_buffer_pos += len;
    }
    else
    {
        mux_tx_buffer_pos = mspi_tx_buffer_start + IFX_SPI_HEADER_SIZE;
        memcpy(mux_tx_buffer_pos, buf+len , remain_len);
        mux_tx_buffer_pos += remain_len;
    }
    mspi_tx_buffer_data_count += count;
    MSPI_DBG(7,"TS qdi: len=%d , remain_len=%d , count=%d , buff_used_size=%d\n", len, remain_len, count,mspi_tx_buffer_data_count);

    spin_unlock_bh(&spi_nodes_lock);

    return count;
}


static void mspi_queue_data_setup_spi_tx_buf(struct ifx_spi_data *spi_data)
{
    int n_data_to_tx_len = 0;
    int n_remaining_tx_buffer_len = 0;
    u8 *temp_p = NULL;
    spin_lock_bh(&spi_nodes_lock);
    // CTS flag from CP set to 1
    if(cts_mspi_data_flag != 0)
    {
    /* Using blank backup buffer for transfer as Modem not ready to receive data*/
        if(spi_data->ifx_tx_buffer != mspi_tx_buffer_backup)
        {
            spi_data->ifx_tx_buffer = mspi_tx_buffer_backup;
        }

        
        /*if(mspi_tx_buffer_data_count < MAX_DYN_IFX_SPI_BUFFER_SIZE)
        {
                mspi_data_tx_next_len = mspi_tx_buffer_data_count;
        }
        else
        {*/
                mspi_data_tx_next_len = MAX_DYN_IFX_SPI_BUFFER_SIZE;
        //}

    ifx_spi_header_set_info(spi_data->ifx_tx_buffer, 0, mspi_data_tx_next_len);
        
        mspi_data_tx_next_len += IFX_SPI_HEADER_SIZE;
        mspi_data_tx_next_len = ALIGN(mspi_data_tx_next_len, MSPI_DMA_ALIGN);
    }
    else // CTS flag from CP set to 0
    {
        n_data_to_tx_len = mspi_curr_dma_data_len - IFX_SPI_HEADER_SIZE;
        n_remaining_tx_buffer_len = mspi_tx_buffer_end - mspi_tx_buffer_pos - IFX_SPI_HEADER_SIZE;
        /* we are reaching the end of the TX buffer, will use TX space under _start position (-1 helper frame) 
           data from the end of the TX buffer will be placed in the -1 frame, and the rest of the date already writen in the 0 frame by
           the insert_item function */
        if((n_remaining_tx_buffer_len < n_data_to_tx_len) && (mspi_tx_buffer_data_count > n_remaining_tx_buffer_len))
        {
            temp_p = mspi_tx_buffer_start + IFX_SPI_HEADER_SIZE - n_remaining_tx_buffer_len;
            memcpy(temp_p, mspi_tx_buffer_pos + IFX_SPI_HEADER_SIZE, n_remaining_tx_buffer_len);
            mspi_tx_buffer_pos = temp_p - IFX_SPI_HEADER_SIZE;
        }

        if(mspi_tx_buffer_data_count <= n_data_to_tx_len)
        {
            n_data_to_tx_len = mspi_tx_buffer_data_count;
            mspi_data_tx_next_len = 0;
        }
        else
        {
            /*if( (mspi_tx_buffer_data_count - n_data_to_tx_len) >= MAX_DYN_IFX_SPI_BUFFER_SIZE)
            {*/
                mspi_data_tx_next_len = MAX_DYN_IFX_SPI_BUFFER_SIZE;
            /*}
            else
            {
                mspi_data_tx_next_len = mspi_tx_buffer_data_count - n_data_to_tx_len;
            }*/
        }

    /* the case when we are not reaching the end of TX buffer */
        if((mux_tx_buffer_pos >= mspi_tx_buffer_pos) && (mux_tx_buffer_pos < (mspi_tx_buffer_pos + mspi_curr_dma_data_len)))
        {
            mux_tx_buffer_pos = mspi_tx_buffer_pos + mspi_curr_dma_data_len;
            if(mux_tx_buffer_pos >= mspi_tx_buffer_end)
                mux_tx_buffer_pos = mspi_tx_buffer_start + IFX_SPI_HEADER_SIZE;
        }

        spi_data->ifx_tx_buffer = mspi_tx_buffer_pos;
        /* set MSPI Header */
        ifx_spi_header_set_info(spi_data->ifx_tx_buffer, n_data_to_tx_len, mspi_data_tx_next_len);

        mspi_tx_buffer_data_count -= n_data_to_tx_len;

        MSPI_DBG(7,"TS qdc: c_len=%d ,n_len=%d\n", n_data_to_tx_len ,mspi_data_tx_next_len);

        if(mspi_data_tx_next_len != 0)
        {
            mspi_data_tx_next_len = ALIGN((mspi_data_tx_next_len + IFX_SPI_HEADER_SIZE), MSPI_DMA_ALIGN);
        }

        if((mspi_tty_client_wakeup == 1) && (mspi_tx_buffer_data_count <= MSPI_TX_BUFFER_WTRMK_LOW))
        {
            mspi_tty_client_wakeup = 0;
            spin_unlock_bh(&spi_nodes_lock);
            tty_wakeup(spi_data->ifx_tty);
            return;
        }
    }
    spin_unlock_bh(&spi_nodes_lock);
    return;
}

/* returns
  0 in case if mspi have data to transfer down (TX)
  1 in case if nothing to send */
static int mspi_queue_empty(void)
{
    int  ret = 0;
    spin_lock_bh(&spi_nodes_lock);
    if(mspi_tx_buffer_data_count <= 0)
    {
        MSPI_DBG(7,"TS q_e\n");
        ret = 1;
    }
    spin_unlock_bh(&spi_nodes_lock);
    return ret;
}

/* ################################################################################################################ */

/* TTY - SPI driver Operations */

static int ifx_spi_probe_hw_dependible(struct ifx_spi_data *spi_data)
{
    struct spi_device *spi = spi_data->spi;
    struct ifx_spi_platform_data *spi_pd = (struct ifx_spi_platform_data *)spi->dev.platform_data;
    int status = 0;

    spi_data->mrdy_gpio = spi_pd->mrdy_gpio;
    spi_data->srdy_gpio = spi_pd->srdy_gpio;
    omap_mux_init_gpio(spi_data->srdy_gpio, OMAP_PIN_INPUT | OMAP_PIN_OFF_WAKEUPENABLE);    
    omap_mux_init_gpio(spi_data->mrdy_gpio, OMAP_PIN_OUTPUT );  
    omap_mux_init_gpio(OMAP_MODEM_WAKE, OMAP_PIN_OUTPUT );

    if (gpio_request(spi_data->srdy_gpio, "ifx srdy") < 0)
    {
        printk(KERN_ERR "Can't get SRDY GPIO\n");
    }
    gpio_direction_input(spi_data->srdy_gpio);

    if (gpio_request(spi_data->mrdy_gpio, "ifx mrdy") < 0)
    {
        printk(KERN_ERR "Can't get MRDY GPIO\n");
    }
    gpio_direction_output(spi_data->mrdy_gpio, 0);

    if (gpio_request(OMAP_MODEM_WAKE, "ap sleep check") < 0)
    {
        printk(KERN_ERR "can't get synaptics pen down GPIO\n");
        /* 20110328 ws.yang@lge.com.  to free the memory */
        ifx_spi_free_frame_memory(spi_data);
        if(spi_data)
        {
            kfree(spi_data);
        }           
        return -ENOMEM;
    }

    gpio_direction_output(OMAP_MODEM_WAKE, 1);
    gpio_set_value(OMAP_MODEM_WAKE, 1);

    spi_data->spi->irq = OMAP_GPIO_IRQ(spi_data->srdy_gpio);
    /* Enable SRDY Interrupt request - If the SRDY signal is high then ifx_spi_handle_mrdy_irq() is called */
    status = request_irq(spi->irq, ifx_spi_handle_mrdy_irq,  IRQF_TRIGGER_RISING, spi->dev.driver->name, spi_data);

    enable_irq_wake(spi->irq);

    return status;
}

static int ifx_spi_probe(struct spi_device *spi)
{
    int status;
    struct ifx_spi_data *spi_data;
    unsigned char wq_name[20];
    static int index = 0;

  if(index >= MSPI_DATA_TABLE_SIZE)
  {
        MSPI_DBG(5, "probe failed, too many devices, index = %d", index);
    return -EBUSY;
  }


    /* Allocate SPI driver data */
    spi_data = (struct ifx_spi_data*)kzalloc(sizeof(struct ifx_spi_data), GFP_KERNEL);
    if (spi_data == NULL) return -ENOMEM;


    dev_set_drvdata(&spi->dev,spi_data);
    INIT_WORK(&spi_data->ifx_work,ifx_spi_handle_work);
    INIT_WORK(&spi_data->ifx_free_irq_work, ifx_spi_handle_free_irq_work);
    sprintf(wq_name, "spi_wq_%d", index);
    spi_data->ifx_wq = create_singlethread_workqueue(wq_name);
    if(spi_data->ifx_wq == NULL)
    {
        MSPI_ERR("TS failed to allocate workqueue %s\n",wq_name);
    }

    /* Configure SPI */
    spi_data->spi = spi;
    spi->mode = SPI_MODE_1;
    spi->bits_per_word = 32;
    spi->slave_ready = ifx_spi_ap_ready;
    status = spi_setup(spi);

    if(status < 0)
    {
        MSPI_ERR("TS failed to setup SPI \n");
    }

    status = ifx_spi_allocate_frame_memory(spi_data, IFX_SPI_MAX_BUF_SIZE + IFX_SPI_HEADER_SIZE);
    if(status != 0)
    {
        MSPI_ERR("Failed to allocate memory for buffers");
        return -ENOMEM;
    }

#ifdef WAKE_LOCK_RESUME
    wake_lock_init(&spi_data->wake_lock, WAKE_LOCK_SUSPEND, "mspi_wake");
#endif

    status = ifx_spi_probe_hw_dependible(spi_data);

    if (status != 0)
    {
        MSPI_ERR("Failed to request IRQ for SRDY - IFX SPI Probe Failed \n");
        ifx_spi_free_frame_memory(spi_data);
        if(spi_data != NULL)
        {
            kfree(spi_data);
        }
        return status;
    }

    ifx_spi_buffer_initialization(spi_data);
    spi_data_table[spi->master->bus_num - 1] = spi_data;
    status = 0;
    MSPI_DBG(6,"ifx_probe [END] with status: %d \n", status);
    return status;
}

static int
ifx_spi_remove(struct spi_device *spi)
{
    struct ifx_spi_data *spi_data;
    spi_data = spi_get_drvdata(spi);
    spi_data->spi = NULL;
    spi_set_drvdata(spi, NULL);

    ifx_spi_free_frame_memory(spi_data);
    if(spi_data != NULL){
        kfree(spi_data);
    }
    spi_data_table[spi->master->bus_num - 1] = NULL;

    return 0;
}

static void ifx_spi_shutdown(struct spi_device *spi)
{

    struct ifx_spi_data *spi_data;
    struct irq_desc *desc = irq_to_desc(spi->irq);

    ifx_shutdown = 1;

    spi_data= spi_get_drvdata(spi);

    if(spi_data->spi->irq)
    {
        disable_irq(spi_data->spi->irq);
        MSPI_DBG(6,"ifx_spi_shutdown: disable_irq\n");
    }


    if(spi_data->spi->irq && (desc!=NULL && desc->action))
    {
    	if(in_interrupt())
    	{
	        queue_work(spi_data->ifx_wq, &(spi_data->ifx_free_irq_work));
	        MSPI_DBG(3,"ifx_spi_shutdown: called from irq context, free_irq delayed.\n");
	        if(spi->irq != spi_data->spi->irq)
	        {
	        	MSPI_ERR("ifx_spi_shutdown: spi->irq = %d, spi_data->spi->irq =%d\n", spi->irq, spi_data->spi->irq);
	        }
    	}
    	else
    	{
	        free_irq(spi_data->spi->irq, spi_data);
	        MSPI_DBG(6,"ifx_spi_shutdown: free_irq\n");
	    }
    }

    MSPI_DBG(6,"ifx_spi_shutdown: completed\n");
}

static int
ifx_spi_suspend(struct spi_device *spi, pm_message_t mesg)
{
    MSPI_DBG(5,"[IFX_SPI SUSPEND] suspend called\n");
/*
    unsigned long reg;

    if(bSuspend)
        return 0;
    bSuspend = 1;

    reg = readl(pmc_base + PMC_WAKE_STATUS);

    // Clear power key wakeup pad bit.
    if (reg & WAKEUP_IFX_SRDY_MASK)
    {
        MSPI_DBG(5,"[IFX_SPI SUSPEND]  wakeup pad : 0x%lx\n", reg);
        writel(WAKEUP_IFX_SRDY_MASK, pmc_base + PMC_WAKE_STATUS);
    }
*/
    return 0;
}

static int
ifx_spi_resume(struct spi_device *spi)
{
    MSPI_DBG(5,"[IFX_SPI RESUME] resume called\n");
/*
    NvU32   pinValue;
    unsigned long reg;

    if(!bSuspend)
        return 0;

    bSuspend = 0;

    reg = readl(pmc_base + PMC_WAKE_STATUS);

    if (reg & WAKEUP_IFX_SRDY_MASK)
    {
        MSPI_DBG(5,"[IFX_SPI RESUME] wakeup pad : 0x%lx\n", reg);

#ifdef  WAKE_LOCK_RESUME
        if(&spi_data_table[MSPI_DEFAULT_TABLE_ENTRY]->wake_lock)
            wake_lock_timeout(&spi_data_table[MSPI_DEFAULT_TABLE_ENTRY]->wake_lock, 5); //20101203-1, syblue.lee@lge.com, change 3 to 1 for power consumption
#endif

        queue_work(spi_data_table[MSPI_DEFAULT_TABLE_ENTRY]->ifx_wq, &spi_data_table[MSPI_DEFAULT_TABLE_ENTRY]->ifx_work);
    }
*/
    return 0;
}
/* End of TTY - SPI driver Operations */

#ifdef SPI2SPI_TEST
static int spi2spi_start_test(struct ifx_spi_data *spi_data)
{
    unsigned long flag;
    if(s2s_test.xfer_size > 0)
    {
        spi2spi_buff_size = ALIGN(s2s_test.xfer_size + IFX_SPI_HEADER_SIZE,MSPI_DMA_ALIGN);
    }
    if (s2s_test.testcase == 1 || s2s_test.testcase == 3) {
        spi2spi_tx_buff = kmalloc(spi2spi_buff_size, GFP_KERNEL | GFP_DMA);
        MSPI_DBG(9,"spi2spi_start_test allocated TX buf %d\n", spi2spi_buff_size);
        if (spi2spi_tx_buff == NULL) {
            MSPI_ERR("spi2spi_test: failed to allocate tx_buf");
            goto out;
        }
        memset(spi2spi_tx_buff,0,IFX_SPI_HEADER_SIZE);
    }
    else
    {
        spi2spi_tx_buff = NULL;
    }

    spi2spi_rx_buff = kmalloc(spi2spi_buff_size, GFP_KERNEL | GFP_DMA);
    MSPI_DBG(9,"spi2spi_start_test allocated RX buf %d\n", spi2spi_buff_size);
    if (spi2spi_rx_buff == NULL) {
        MSPI_ERR("spi2spi_test: failed to allocate rx_buf");
        goto out;
    }
    memset(spi2spi_rx_buff,0,IFX_SPI_HEADER_SIZE);

    spin_lock_irqsave(&spi2spi_lock,flag);
    spi2spi_test_is_running = 1;
    spin_unlock_irqrestore(&spi2spi_lock,flag);

    if (spi_data->ifx_wq != NULL){
        cancel_work_sync(&(spi_data->ifx_work));
        destroy_workqueue(spi_data->ifx_wq);
        spi_data->ifx_wq = NULL;
    }

    INIT_WORK(&spi_data->ifx_work,ifx_spi2spi_handle_work);
    spi_data->ifx_wq = create_singlethread_workqueue("spi2spi_test_wq");
    if(spi_data->ifx_wq == NULL)
    {
        MSPI_ERR("TS failed to allocate spi2spi test workqueue\n");
    }

    MSPI_DBG(9,"spi2spi_start_test start work \n");
    queue_work(spi_data->ifx_wq, &spi_data->ifx_work);


    return 0;
out:
    MSPI_DBG(9,"spi2spi_start_test error \n");
    if(spi2spi_tx_buff != NULL)
    {
        kfree(spi2spi_tx_buff);
        spi2spi_tx_buff = NULL;
    }
    if(spi2spi_rx_buff != NULL)
    {
        kfree(spi2spi_rx_buff);
        spi2spi_rx_buff = NULL;
    }
    return -ENOMEM;
}
#endif //SPI2SPI_TEST

static void mspi_tx_buffer_flush(struct ifx_spi_data *spi_data)
{
    mutex_lock(&mspi_transfer_lock);
    spin_lock_bh(&spi_nodes_lock);
    mspi_tx_buffer_pos              = mspi_tx_buffer_start;
    mux_tx_buffer_pos               = mspi_tx_buffer_start + (int)IFX_SPI_HEADER_SIZE;
    mspi_tx_buffer_data_count       = 0;
    mspi_tx_buffer_backup           = mspi_tx_buffer_end;
    memset(mspi_tx_buffer_backup,0,MSPI_SPI_FULL_FRAME_LENGTH);
    spin_unlock_bh(&spi_nodes_lock);
    mutex_unlock(&mspi_transfer_lock);
}

static void mspi_rx_buffer_flush(struct ifx_spi_data *spi_data)
{
    mutex_lock(&mspi_transfer_lock);
    mutex_lock(&mspi_rx_task_lock);
    if(mspi_flush_rx == 0)
    {
        mspi_flush_rx = 1;

        if(mspi_rx_wq)
        {
            flush_workqueue(mspi_rx_wq);
            spin_lock_bh(&mux_rx_buff_lock);
            spi_data->ifx_rx_buffer     = mspi_rx_buffer_start;
            mux_rx_buffer_curr_size     = 0;
            spin_unlock_bh(&mux_rx_buff_lock);
        }

        mspi_flush_rx = 0;
    }
    mutex_unlock(&mspi_rx_task_lock);
    mutex_unlock(&mspi_transfer_lock);
}

int ifx_spi_ioctl(struct tty_struct *tty,
                  /*struct file * file,*/     // only for GB
                  unsigned int cmd, unsigned long arg)
{
    int retval = 0;
    struct ifx_spi_data *spi_data;
    spi_data = tty->driver_data;
    if (spi_data == NULL)
        return -ENODEV;

    switch (cmd) {
#ifdef SPI2SPI_TEST
        case TIOCSPI2SPISTART:
            MSPI_DBG(9," TIOCSPI2SPISTART ");
            if (copy_from_user(&s2s_test, (void __user *)arg, sizeof(s2s_test)))
            {
                MSPI_DBG(8," TIOCSPI2SPISTART FAIL ");
                return -EIO;
            }
            retval = spi2spi_start_test(spi_data);
            break;
        case TIOCSPI2SPISTOP:
            // TODO stop
            MSPI_DBG(9," TIOCSPI2SPISTOP ");
            spin_lock_bh(&spi2spi_lock);
            spi2spi_test_is_running = 0;
            if(spi2spi_tx_buff != NULL)
            {
                kfree(spi2spi_tx_buff);
                spi2spi_tx_buff = NULL;
            }
            if(spi2spi_rx_buff != NULL)
            {
                kfree(spi2spi_rx_buff);
                spi2spi_rx_buff = NULL;
            }
            spin_unlock_bh(&spi2spi_lock);
            break;
        case TIOCGSTAT:
            {
            struct spi2spi_stat stat;
            struct timespec ts;
            MSPI_DBG(9," TIOCGSTAT ");

            spin_lock_bh(&spi2spi_lock);
            stat.counter = spi2spi_counter;
            spin_unlock_bh(&spi2spi_lock);
            ts = current_kernel_time();
            stat.timestamp = ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
            if (copy_to_user((void*)arg, &stat, sizeof stat))
                return -EIO;
            }
            break;
#endif //SPI2SPI_TEST
        default:
        MSPI_DBG(9,"ifx_spi_ioctl: No such ioctl %x", cmd);
        return -ENOIOCTLCMD;
        break;
    }
    return retval;
}

/* ################################################################################################################ */
static struct spi_driver ifx_spi_driver = 
{
    .driver = 
    {
        .name = "mdm6600",
                .bus = &spi_bus_type,
        .owner = THIS_MODULE,
    },
    .probe = ifx_spi_probe,
    .remove = __devexit_p(ifx_spi_remove),
    .shutdown   = ifx_spi_shutdown,
    .suspend = ifx_spi_suspend,
    .resume = ifx_spi_resume,
};

/*
 * Structure to specify tty core about tty driver operations supported in TTY SPI driver.
 */
static const struct tty_operations ifx_spi_ops = {
    .open               = ifx_spi_open,
    .close              = ifx_spi_close,
    .write              = ifx_spi_write,
    .write_room         = ifx_spi_write_room,
    .ioctl              = ifx_spi_ioctl,
    .chars_in_buffer    = ifx_spi_chars_in_buffer,
    .throttle           = ifx_spi_throttle,
    .unthrottle         = ifx_spi_unthrottle,
    .flush_buffer       = ifx_spi_flush_buffer,
    .hangup             = ifx_spi_hangup,
    .set_ldisc          = ifx_spi_set_ldisc,
    .tiocmget           = ifx_spi_tiocmget
};

/* ################################################################################################################ */

/*
 * Intialize frame sizes as "IFX_SPI_DEFAULT_BUF_SIZE"(128) bytes for first SPI frame transfer
 */
static void
ifx_spi_buffer_initialization(struct ifx_spi_data *spi_data)
{
    spi_data->ifx_receiver_buf_size = IFX_SPI_DEFAULT_BUF_SIZE;
    mspi_curr_dma_data_len = SPI_MAX_PACKET_LEN;
}

/*
 * Allocate memeory for TX_BUFFER and RX_BUFFER
 */
static int
ifx_spi_allocate_frame_memory(struct ifx_spi_data *spi_data, unsigned int memory_size)
{
    int status = 0;

    spi_data->ifx_tx_buffer = kmalloc(MSPI_SPI_FULL_FRAME_LENGTH * (MSPI_MAX_TX_FRAME_BUFS + 2) , GFP_KERNEL | GFP_DMA);
    if (spi_data->ifx_tx_buffer == NULL)
    {
        status = -ENOMEM;
    }
    else
    {
        mspi_tx_buffer_start            = spi_data->ifx_tx_buffer + (int)MSPI_SPI_FULL_FRAME_LENGTH;
        mspi_tx_buffer_end              = spi_data->ifx_tx_buffer + (int)((int)MSPI_SPI_FULL_FRAME_LENGTH * (int)(MSPI_MAX_TX_FRAME_BUFS + 1));
        mspi_tx_buffer_pos              = mspi_tx_buffer_start;
        mux_tx_buffer_pos               = mspi_tx_buffer_start + (int)IFX_SPI_HEADER_SIZE;
        mspi_tx_buffer_data_count       = 0;
        mspi_tx_buffer_backup           = mspi_tx_buffer_end;
        memset(mspi_tx_buffer_backup,0,MSPI_SPI_FULL_FRAME_LENGTH);
    }

    spi_data->ifx_rx_buffer = kmalloc( MSPI_RX_BUFFER_SIZE , GFP_KERNEL | GFP_DMA);
    if (spi_data->ifx_rx_buffer == NULL)
    {
        status = -ENOMEM;
    } else {
        mspi_rx_buffer_start            = spi_data->ifx_rx_buffer;
        mspi_rx_buffer_end              = spi_data->ifx_rx_buffer + (int)MSPI_RX_BUFFER_SIZE;
        mux_rx_buffer_curr_size         = 0;
    }

    if(status == -ENOMEM){
        MSPI_ERR("Open Failed ENOMEM\n");
        if(spi_data->ifx_tx_buffer != NULL)
        {
            kfree(spi_data->ifx_tx_buffer);
            mspi_tx_buffer_start            = NULL;
            mspi_tx_buffer_end              = NULL;
            mux_tx_buffer_pos               = NULL;
            mspi_tx_buffer_data_count       = 0;
            mspi_tx_buffer_backup           = NULL;
            mspi_tx_buffer_pos              = NULL;
        }
        if(spi_data->ifx_rx_buffer != NULL)
        {
            kfree(spi_data->ifx_rx_buffer);
            mspi_rx_buffer_start            = NULL;
            mspi_rx_buffer_end              = NULL;
            mux_rx_buffer_curr_size         = 0;
        }
    }
    return status;
}
static void
ifx_spi_free_frame_memory(struct ifx_spi_data *spi_data)
{
    if(mspi_tx_buffer_start != NULL)
    {
        kfree((mspi_tx_buffer_start - (int)MSPI_SPI_FULL_FRAME_LENGTH));
        mspi_tx_buffer_start            = NULL;
        mspi_tx_buffer_end              = NULL;
        mux_tx_buffer_pos               = NULL;
        mspi_tx_buffer_data_count       = 0;
        mspi_tx_buffer_backup           = NULL;
        mspi_tx_buffer_pos              = NULL;
    }
    if(mspi_rx_buffer_start != NULL)
    {
        kfree(mspi_rx_buffer_start);
        mspi_rx_buffer_start            = NULL;
        mspi_rx_buffer_end              = NULL;
        mux_rx_buffer_curr_size         = 0;
    }
}

/*
 * Function to set header information according to IFX SPI framing protocol specification
 */
static void
ifx_spi_header_set_info(unsigned char *header_buffer, unsigned int curr_buf_size, unsigned int next_buf_size)
{
    struct ifx_spi_frame_header *header = (struct ifx_spi_frame_header *)header_buffer;

    memset(header_buffer,0,IFX_SPI_HEADER_SIZE);

    header->curr_data_size = curr_buf_size;
  
    if(next_buf_size > 0)
    {
        header->more = 1;
        header->next_data_size = next_buf_size;
    }
  
    if(rts_mspi_data_flag != 0)
    {
        header->cts_rts = 1;
    }
}

/*
 * Function to set header information according to IFX SPI framing protocol specification
 */
static void
ifx_spi_header_update_curr_size(unsigned char *header_buffer, unsigned int curr_buf_size)
{
    struct ifx_spi_frame_header *header = (struct ifx_spi_frame_header *)header_buffer;
    header->curr_data_size = curr_buf_size;
}

/*
 * Function to set header information according to IFX SPI framing protocol specification
 */
static void
ifx_spi_header_update_next_size(unsigned char *header_buffer, unsigned int next_buf_size)
{
    struct ifx_spi_frame_header *header = (struct ifx_spi_frame_header *)header_buffer;
    if(next_buf_size > 0)
    {
        header->more = 1;
        header->next_data_size = next_buf_size;
    }
    else
    {
        header->more = 0;
        header->next_data_size = 0;
    }
}

/*
 * Function to get header information according to IFX SPI framing protocol specification
 * returns
 * number of bytes ready for the next transfer (RX)
 */
static void
ifx_spi_header_get_info(unsigned char *header_buffer, unsigned int *curr_buf_size, unsigned int *next_buf_size)
{
    struct ifx_spi_frame_header *header = (struct ifx_spi_frame_header *)header_buffer;
    *curr_buf_size = header->curr_data_size;
  *next_buf_size = header->more == 1 ? header->next_data_size : 0;
}

/*
 * Function to get header information according to IFX SPI framing protocol specification
 */
static inline int
ifx_spi_header_get_more_flag(unsigned char *rx_buffer)
{
    return ((struct ifx_spi_frame_header *)rx_buffer)->more;
}

/*
 * Function to get header information according to IFX SPI framing protocol specification
 */
static inline int
ifx_spi_header_get_cts_flag(unsigned char *rx_buffer)
{
    return ((struct ifx_spi_frame_header *)rx_buffer)->cts_rts;
}

static void mspi_rx_task(struct work_struct *pwork)
{
    struct mspi_rx_work *work = container_of(pwork, struct mspi_rx_work, work);

    /* flush is in progress, so drop all the data*/
    if(mspi_flush_rx == 1)
    {
        MSPI_DBG(10, "%s flushing rx work", __func__);
        spin_lock_bh(&mux_rx_buff_lock);
        if(MSPI_CAN_RECEIVE)
        {
            /* 0 the terminal is able to receive data */
            rts_mspi_data_flag = 0;
        }
        spin_unlock_bh(&mux_rx_buff_lock);
        kfree(work);
        return;
    }

    mutex_lock(&mspi_rx_task_lock);

    // Ensure that tty does have enough space to receive packet, otherwise wait for unthrottle
    /*while(tty_buffer_request_room(work->ifx_tty, work->len) < work->len)
    {
        MSPI_DBG(9, "not enough receive room");
        set_bit(TTY_THROTTLED, &work->ifx_tty->flags);
        while(down_interruptible(&mspi_rx_completion) != 0) {};
    }
    tty_insert_flip_string(work->ifx_tty, work->mspi_rx_buffer, work->len);
    tty_flip_buffer_push(work->ifx_tty);*/
    work->ifx_tty->ldisc->ops->receive_buf(work->ifx_tty, work->mspi_rx_buffer, NULL, work->len);

    spin_lock_bh(&mux_rx_buff_lock);
    mux_rx_buffer_curr_size -= ALIGN((work->len + IFX_SPI_HEADER_SIZE),MSPI_DMA_ALIGN);

    if(MSPI_CAN_RECEIVE)
    {
        rts_mspi_data_flag = 0;
        spin_unlock_bh(&mux_rx_buff_lock);
        mutex_unlock(&mspi_rx_task_lock);
#ifdef WAKE_LOCK_RESUME
        wake_lock_timeout(&spi_data_table[work->spi_data->spi->master->bus_num - 1]->wake_lock, MSPI_WAKE_LOCK_TIMEOUT);
#endif
        queue_work(work->spi_data->ifx_wq, &(work->spi_data->ifx_work));
    }
    else
    {
        spin_unlock_bh(&mux_rx_buff_lock);
        mutex_unlock(&mspi_rx_task_lock);
    }
    kfree(work);

}

static void
ifx_spi_tty_callback( struct ifx_spi_data *spi_data)
{

    struct mspi_rx_work *work;
    unsigned int rx_valid_buf_size;
    unsigned int rx_next_idx;
    if(spi_data != NULL)
    {
    /* Handling Received data */
        ifx_spi_header_get_info(spi_data->ifx_rx_buffer, &rx_valid_buf_size, &spi_data->ifx_receiver_buf_size);
        spi_data->ifx_receiver_more_data = ifx_spi_header_get_more_flag(spi_data->ifx_rx_buffer);
        cts_mspi_data_flag = ifx_spi_header_get_cts_flag(spi_data->ifx_rx_buffer);

        if(MSPI_ISDBG(8))
        {
            UNMSPI_DBG("TS istc: RXbuf=%p \n",spi_data->ifx_rx_buffer);
            mspi_dump_spi_header_info(spi_data->ifx_rx_buffer, 1);
        }
        
        if((spi_data->users > 0) && (rx_valid_buf_size > 0)
          && !(rx_valid_buf_size > MAX_DYN_IFX_SPI_BUFFER_SIZE)) // validity check
        {
            work = (struct mspi_rx_work*)kmalloc(sizeof(struct mspi_rx_work),GFP_ATOMIC);
            if (work)
            {
                work->ifx_tty = spi_data->ifx_tty; //yes, I know it can be changed, but we have only 1 mSPI client.
                work->len = rx_valid_buf_size;
                work->spi_data = spi_data;
                work->mspi_rx_buffer = spi_data->ifx_rx_buffer + IFX_SPI_HEADER_SIZE;
                rx_next_idx = ALIGN((rx_valid_buf_size + IFX_SPI_HEADER_SIZE),MSPI_DMA_ALIGN);
                if((spi_data->ifx_rx_buffer + (int)rx_next_idx +  (int)MSPI_SPI_FULL_FRAME_LENGTH) > mspi_rx_buffer_end)
                {
                    spi_data->ifx_rx_buffer = mspi_rx_buffer_start;
                }
                else
                {
                    spi_data->ifx_rx_buffer += rx_next_idx;
                }
                MSPI_DBG(7,"TS istc: a=%p l=%d na=%p \n",work->mspi_rx_buffer,work->len,spi_data->ifx_rx_buffer);
                spin_lock_bh(&mux_rx_buff_lock);
                mux_rx_buffer_curr_size += rx_next_idx;
                if(MSPI_CANNOT_RECEIVE)
                    rts_mspi_data_flag = 1;

                spin_unlock_bh(&mux_rx_buff_lock);
                INIT_WORK(&(work->work), mspi_rx_task);
                queue_work(mspi_rx_wq, &(work->work));
            }
        }

        MSPI_DBG(4,"TS istc: CTS=%d RTS=%d \n",cts_mspi_data_flag,rts_mspi_data_flag);
    }
}

static void
ifx_spi_sync_data(struct ifx_spi_data *spi_data)
{
    int status              = -ESHUTDOWN;

    struct spi_message  m;
    struct spi_transfer t   = {
                        .tx_buf     = spi_data->ifx_tx_buffer,
                        .rx_buf     = spi_data->ifx_rx_buffer,
                        .len        = mspi_curr_dma_data_len,
                        };

    if(ifx_shutdown != 0 || spi_data->spi == NULL) return;

    if(is_tx_rx_required == 0) t.tx_buf = NULL;

    spi_message_init(&m);
    spi_message_add_tail(&t, &m);
    status = spi_sync_killable(spi_data->spi, &m);
    if (status == 0){
        status = m.status;
        if (status == 0)
            status = m.actual_length;
    }


    if(status < 0)
    {
		MSPI_ERR("spi error encountered, halting");
        ifx_shutdown = 1;
    }

    if(status != mspi_curr_dma_data_len)
    {
        MSPI_ERR("[EBS-SPI] TX-RX SPI Failure !! dma_len=%d ret_len=%d\n",mspi_curr_dma_data_len, status);
        return;
    }
    ifx_spi_tty_callback(spi_data);
}

static void ifx_spi_ap_ready(struct spi_device *spi, int enable)
{
    struct ifx_spi_data *spi_data = spi_data_table[spi->master->bus_num - 1];

    if(spi_data == NULL)
    {
        printk("ifx_spi_ap_ready, spi_data is null\n");
        return;
    }
    gpio_set_value(spi_data->mrdy_gpio, enable);
}

/*
 * Function is a Interrupt service routine, is called when MRDY signal goes HIGH. It set up srdy pin when ready and
 * reception if it is a Master initiated data transfer. For both the cases Master intiated/Slave intiated
 * transfer it starts data transfer.
 */
static irqreturn_t ifx_spi_handle_mrdy_irq(int irq, void *handle)
{
    struct ifx_spi_data *spi_data = (struct ifx_spi_data *)handle;

    MSPI_DBG(4, "IRQ");
    if (ifx_shutdown == 1) return IRQ_HANDLED;

    if(spi_data != NULL && spi_data->ifx_tty != NULL)
    {
#ifdef WAKE_LOCK_RESUME
        wake_lock_timeout(&spi_data_table[spi_data->spi->master->bus_num - 1]->wake_lock, MSPI_WAKE_LOCK_TIMEOUT);
#endif
        queue_work(spi_data->ifx_wq, &spi_data->ifx_work);
    }
    else
    {
        MSPI_ERR("Unexpected interrupt happen!");
    }
    return IRQ_HANDLED;
}

#ifdef SPI2SPI_TEST
static void
ifx_spi2spi_handle_work(struct work_struct *work)
{
    int status = 0;
    struct ifx_spi_data *spi_data = container_of(work, struct ifx_spi_data, ifx_work);

    if(spi_data != NULL || spi_data->spi != NULL)
    {
        struct spi_message  m;
        struct spi_transfer t = {
            .tx_buf     = spi2spi_tx_buff,
            .rx_buf     = spi2spi_rx_buff,
            .len        = spi2spi_buff_size,
        };
        while(1)
        {
            spi_message_init(&m);
            spi_message_add_tail(&t, &m);
            status = spi_sync(spi_data->spi, &m);
            if (status == 0){
                status = m.status;
                if (status == 0)
                {
                    spin_lock_bh(&spi2spi_lock);
                    spi2spi_counter += m.actual_length;
                    spin_unlock_bh(&spi2spi_lock);
                }
            }
        }
    }
}
#endif //SPI2SPI_TEST

static void
ifx_spi_handle_free_irq_work(struct work_struct *work)
{
	struct ifx_spi_data *spi_data = container_of(work, struct ifx_spi_data, ifx_free_irq_work);

	if(spi_data != NULL && spi_data->spi != NULL)
	{
		struct irq_desc *desc = irq_to_desc(spi_data->spi->irq);

	    if(spi_data->spi->irq && (desc!=NULL && desc->action))
	    {
	        free_irq(spi_data->spi->irq, spi_data);
	        MSPI_DBG(6,"ifx_spi_handle_free_irq_work: free_irq\n");
	    }
	    else
	    {
	    	MSPI_ERR("ifx_spi_handle_free_irq_work: irq state has changed!\n");
	    }
	}
	else
	{
		MSPI_ERR("ifx_spi_handle_free_irq_work: spi_data or spi is NULL!\n");
	}

}

static void
ifx_spi_handle_work(struct work_struct *work)
{
    struct ifx_spi_data *spi_data = container_of(work, struct ifx_spi_data, ifx_work);

  do
  {

    if(spi_data == NULL || ifx_shutdown != 0) return;

    mutex_lock(&mspi_transfer_lock);
    
    memset(spi_data->ifx_rx_buffer,0,IFX_SPI_HEADER_SIZE);

    if(MSPI_ISDBG(7))
    {
        mspi_frame_tx_count++;
        UNMSPI_DBG("TS ishw: cTX=%d cRX=%d cDMA=%d id=%d\n", mspi_data_tx_next_len, mspi_data_rx_next_len,mspi_curr_dma_data_len,mspi_frame_tx_count);
    }

    is_tx_rx_required = 0;

    if(mspi_queue_empty() == 0)
    {
        mspi_queue_data_setup_spi_tx_buf(spi_data);
        is_tx_rx_required = 1;
        if(MSPI_ISDBG(8))
        {
            UNMSPI_DBG("TS TX_RX tbuf=%p rbuf=%p\n",spi_data->ifx_tx_buffer,spi_data->ifx_rx_buffer);
  //            dump_spi_simple(spi_data->ifx_tx_buffer);
            mspi_dump_spi_header_info(spi_data->ifx_tx_buffer, 0);
        }
    }
    else
    {
    /* we have nothing to send from our side */
        spin_lock_bh(&mux_rx_buff_lock);
        if(MSPI_CAN_RECEIVE)
        {
            rts_mspi_data_flag = 0;
        }

      /* we have data for the next transfer, and need to tell about this to CP
         or rts flag is not 0, and we must pass at least the header to the CP otherwise it receices 0 */
        if(mspi_data_tx_next_len != 0 || rts_mspi_data_flag != 0)
        {
            is_tx_rx_required = 1;
        }
        spin_unlock_bh(&mux_rx_buff_lock);
        if(spi_data->ifx_tx_buffer != mspi_tx_buffer_backup)
        {
            spi_data->ifx_tx_buffer = mspi_tx_buffer_backup;
        }
        ifx_spi_header_set_info(spi_data->ifx_tx_buffer, 0, 0);
        if(MSPI_ISDBG(8))
        {
            UNMSPI_DBG("TS RX_TX tbuf=%p rbuf=%p\n",spi_data->ifx_tx_buffer,spi_data->ifx_rx_buffer);
  //            dump_spi_simple(spi_data->ifx_tx_buffer);
            mspi_dump_spi_header_info(spi_data->ifx_tx_buffer, 0);
        }
    }

    ifx_spi_sync_data(spi_data);

    spin_lock_bh(&spi_nodes_lock);
    /* Some valuable data was sent */
    if((spi_data->ifx_tx_buffer != mspi_tx_buffer_backup) && (spi_data->ifx_tx_buffer != NULL))
    {
         mspi_tx_buffer_pos += (mspi_curr_dma_data_len - IFX_SPI_HEADER_SIZE);

         if(mspi_tx_buffer_pos >= (mspi_tx_buffer_end - IFX_SPI_HEADER_SIZE))
             mspi_tx_buffer_pos = mspi_tx_buffer_start;
    }

/* In the just transfered frame CP indicated that it would like to send some data next time */
    if(spi_data->ifx_receiver_buf_size > 0)
    {
        mspi_data_rx_next_len = ALIGN((spi_data->ifx_receiver_buf_size + IFX_SPI_HEADER_SIZE),MSPI_DMA_ALIGN);
        spin_lock_bh(&mux_rx_buff_lock);
        switch(rts_mspi_data_flag)
        {
            case 1:
                rts_mspi_data_flag = 2;
            case 0:
            /* terminal is able to receive data */
                break;
                
            case 2:
            default:
                mspi_data_rx_next_len = 0;
                break;
        }
        spin_unlock_bh(&mux_rx_buff_lock);
    }
    else
    {
        mspi_data_rx_next_len = 0;
    }

    if(cts_mspi_data_flag != 0)
    {
        mspi_data_tx_next_len = 0;
    }

    mspi_curr_dma_data_len = max(mspi_data_tx_next_len, mspi_data_rx_next_len);

    if(mspi_curr_dma_data_len == 0)
         mspi_curr_dma_data_len = SPI_MAX_PACKET_LEN;

    spin_unlock_bh(&spi_nodes_lock);
    mutex_unlock(&mspi_transfer_lock);

    MSPI_DBG(7,"TS ishw: nTX=%d nRXrecv=%d nRX=%d nDMA=%d \n", mspi_data_tx_next_len, spi_data->ifx_receiver_buf_size, mspi_data_rx_next_len,mspi_curr_dma_data_len);
  }while((mspi_queue_empty() == 0) || (spi_data->ifx_receiver_more_data == 1));
}


/* ################################################################################################################ */


/* ################################################################################################################ */

/* Initialization Functions */

/*
 * Initialization function which allocates and set different parameters for TTY SPI driver. Registers the tty driver 
 * with TTY core and register SPI driver with the Kernel. It validates the GPIO pins for MRDY and then request an IRQ
 * on SRDY GPIO pin for SRDY signal going HIGH. In case of failure of SPI driver register cases it unregister tty driver
 * from tty core.
 */
int
__init ifx_spi_init(void)
{
    int status = 0;
    MSPI_ERR("mSPI driver(%s). Version: %s\n" ,mspi_drv_driver_name ,mspi_drv_driver_version);

    /* Allocate and Register a TTY device */
    ifx_spi_tty_driver = alloc_tty_driver(IFX_N_SPI_MINORS);
    if (!ifx_spi_tty_driver)
    {
        printk("[%s] Fail to allocate TTY Driver\n", __FUNCTION__);
        return -ENOMEM;
    }

    /* initialize the tty driver */
    ifx_spi_tty_driver->owner = THIS_MODULE;
    ifx_spi_tty_driver->driver_name = (const char *)(&mspi_drv_driver_name);
    ifx_spi_tty_driver->name = "ttyspi";
    ifx_spi_tty_driver->major = IFX_SPI_MAJOR;
    ifx_spi_tty_driver->minor_start = 0;
    ifx_spi_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
    ifx_spi_tty_driver->subtype = SERIAL_TYPE_NORMAL;
    ifx_spi_tty_driver->flags = TTY_DRIVER_REAL_RAW;
    ifx_spi_tty_driver->init_termios = tty_std_termios;
    ifx_spi_tty_driver->init_termios.c_cflag = B115200 | CS8 | CREAD | HUPCL | CLOCAL;
    tty_set_operations(ifx_spi_tty_driver, &ifx_spi_ops);

    status = tty_register_driver(ifx_spi_tty_driver);
    if (status != 0){
        MSPI_ERR("Failed to register mSPI tty driver");
        put_tty_driver(ifx_spi_tty_driver);
        return status;
    }

#ifdef MSPI_DRV_DEBUG_FS_LEVEL
    create_mspi_proc_dbg_file();
#endif

    /* Register SPI Driver */
    status = spi_register_driver(&ifx_spi_driver);
    mspi_rx_wq = create_singlethread_workqueue(MSPI_DRIVER_NAME "_rx");
    if (status < 0 || mspi_rx_wq == NULL){
        MSPI_ERR("Failed to register SPI device");
        tty_unregister_driver(ifx_spi_tty_driver);
        put_tty_driver(ifx_spi_tty_driver);
        return status;
    }

#ifdef SPI2SPI_TEST
    spi2spi_counter = 0;
    memset(&s2s_test, 0, sizeof(s2s_test));
    /* .args_recvd and .state will be zeroes */
#endif

    mspi_dump_compilation_info();
    return status;
}

module_init(ifx_spi_init);


/*
 * Exit function to unregister SPI driver and tty SPI driver
 */
void
__exit ifx_spi_exit(void)
{
    spi_unregister_driver(&ifx_spi_driver);
    tty_unregister_driver(ifx_spi_tty_driver);
    put_tty_driver(ifx_spi_tty_driver);

#ifdef MSPI_DRV_DEBUG_FS_LEVEL
    remove_mspi_proc_dbg_file();
#endif

    if (mspi_rx_wq){
        flush_workqueue(mspi_rx_wq);
        destroy_workqueue(mspi_rx_wq);
        mspi_rx_wq = NULL;
    }

#ifdef SPI2SPI_TEST
    spin_lock_bh(&spi2spi_lock);
    spi2spi_test_is_running = 0;
    if(spi2spi_tx_buff != NULL)
    {
        kfree(spi2spi_tx_buff);
        spi2spi_tx_buff = NULL;
    }
    if(spi2spi_rx_buff != NULL)
    {
        kfree(spi2spi_rx_buff);
        spi2spi_rx_buff = NULL;
    }
    spin_unlock_bh(&spi2spi_lock);
#endif

}

module_exit(ifx_spi_exit);

/* End of Initialization Functions */

/* ################################################################################################################ */

MODULE_AUTHOR("Teleca / LGE");
MODULE_DESCRIPTION("MDM6600 SPI Framing Layer: Support Extended Header, Dynamic TX/RX buffers, HAS_MORE, NEXT_FRAME_SIZE");
MODULE_LICENSE("GPL");
