/**
 *
 */

#include <linux/module.h>	/* Needed by all modules */
#include <linux/kernel.h>	/* Needed for KERN_INFO */
#include <linux/init.h>		/* Needed for the macros */
#include <linux/fs.h>       /* Needed for files operations */
#include <linux/pci.h>      /* Needed for PCI */
#include <asm/uaccess.h>    /* Needed for copy_to_user & copy_from_user */
#include <linux/delay.h>    /* udelay, mdelay */
#include <linux/dma-mapping.h>

#include "xpdma_driver.h"

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("PCIe driver for Xilinx CDMA subsystem (XAPP1171), Linux");
MODULE_AUTHOR("Strezhik Iurii");

// Max CDMA buffer size
#define MAX_BTT             0x007FFFFF   // 8 MBytes maximum for DMA Transfer */
#define BUF_SIZE            (4<<20)      // 4 MBytes read/write buffer size
#define TRANSFER_SIZE       (4<<20)      // 4 MBytes transfer size for scatter gather
#define DESCRIPTOR_SIZE     64           // 64-byte aligned Transfer Descriptor

#define BRAM_OFFSET         0x00000000   // Translation BRAM offset
#define PCIE_CTL_OFFSET     0x00008000   // AXI PCIe control offset
#define CDMA_OFFSET         0x0000c000   // AXI CDMA LITE control offset

// AXI CDMA Register Offsets
#define CDMA_CONTROL_OFFSET	0x00         // Control Register
#define CDMA_STATUS_OFFSET	0x04         // Status Register
#define CDMA_CDESC_OFFSET	0x08         // Current descriptor Register
#define CDMA_TDESC_OFFSET	0x10         // Tail descriptor Register
#define CDMA_SRCADDR_OFFSET	0x18         // Source Address Register
#define CDMA_DSTADDR_OFFSET	0x20         // Dest Address Register
#define CDMA_BTT_OFFSET		0x28         // Bytes to transfer Register

#define AXI_PCIE_DM_ADDR    0x80000000   // AXI:BAR1 Address
#define AXI_PCIE_SG_ADDR    0x80800000   // AXI:BAR0 Address
#define AXI_BRAM_ADDR       0x81000000   // AXI Translation BRAM Address
#define AXI_DDR3_ADDR       0x00000000   // AXI DDR3 Address

#define SG_COMPLETE_MASK    0xF0000000   // Scatter Gather Operation Complete status flag mask
#define SG_DEC_ERR_MASK     0x40000000   // Scatter Gather Operation Decode Error flag mask
#define SG_SLAVE_ERR_MASK   0x20000000   // Scatter Gather Operation Slave Error flag mask
#define SG_INT_ERR_MASK     0x10000000   // Scatter Gather Operation Internal Error flag mask

#define BRAM_STEP           0x8          // Translation Vector Length
#define ADDR_BTT            0x00000008   // 64 bit address translation descriptor control length

#define CDMA_CR_SG_EN       0x00000008   // Scatter gather mode enable
#define CDMA_CR_IDLE_MASK   0x00000002   // CDMA Idle mask
#define CDMA_CR_RESET_MASK  0x00000004   // CDMA Reset mask
#define AXIBAR2PCIEBAR_0U   0x208        // AXI:BAR0 Upper Address Translation (bits [63:32])
#define AXIBAR2PCIEBAR_0L   0x20C        // AXI:BAR0 Lower Address Translation (bits [31:0])
#define AXIBAR2PCIEBAR_1U   0x210        // AXI:BAR1 Upper Address Translation (bits [63:32])
#define AXIBAR2PCIEBAR_1L   0x214        // AXI:BAR1 Lower Address Translation (bits [31:0])

#define CDMA_RESET_LOOP	    1000000      // Reset timeout counter limit
#define SG_TRANSFER_LOOP	1000000      // Scatter Gather Transfer timeout counter limit

// Scatter Gather Transfer descriptor
typedef struct {
    u32 nextDesc;   /* 0x00 */
    u32 na1;	    /* 0x04 */
    u32 srcAddr;    /* 0x08 */
    u32 na2;        /* 0x0C */
    u32 destAddr;   /* 0x10 */
    u32 na3;        /* 0x14 */
    u32 control;    /* 0x18 */
    u32 status;     /* 0x1C */
} __aligned(DESCRIPTOR_SIZE) sg_desc_t;

#define HAVE_KERNEL_REG     0x01    // Kernel registration
#define HAVE_MEM_REGION     0x02    // I/O Memory region

int gDrvrMajor = 241;               // Major number not dynamic
struct pci_dev *gDev = NULL;        // PCI device structure
unsigned int gStatFlags = 0x00;     // Status flags used for cleanup
unsigned long gBaseHdwr;            // Base register address (Hardware address)
unsigned long gBaseLen;             // Base register address Length
void *gBaseVirt = NULL;             // Base register address (Virtual address, for I/O)
char *gReadBuffer = NULL;           // Pointer to dword aligned DMA Read buffer
char *gWriteBuffer = NULL;          // Pointer to dword aligned DMA Write buffer

sg_desc_t *gDescChain;              // Translation Descriptors chain
size_t gDescChainLength;

dma_addr_t gReadHWAddr;
dma_addr_t gWriteHWAddr;
dma_addr_t gDescChainHWAddr;

// Prototypes
static int xpdma_reset(void);
ssize_t xpdma_write (struct file *filp, const char *buf, size_t count, loff_t *f_pos);
ssize_t xpdma_read (struct file *filp, char *buf, size_t count, loff_t *f_pos);
long xpdma_ioctl (struct file *filp, unsigned int cmd, unsigned long arg);
int xpdma_open(struct inode *inode, struct file *filp);
int xpdma_release(struct inode *inode, struct file *filp);
static inline u32 xpdma_readReg (u32 reg);
static inline void xpdma_writeReg (u32 reg, u32 val);
ssize_t xpdma_send (void *data, size_t count, u32 addr);
ssize_t xpdma_recv (void *data, size_t count, u32 addr);
void xpdma_showInfo (void);

// Aliasing write, read, ioctl, etc...
struct file_operations xpdma_intf = {
        read           : xpdma_read,
        write          : xpdma_write,
        unlocked_ioctl : xpdma_ioctl,
        //llseek         : xpdma_lseek,
        open           : xpdma_open,
        release        : xpdma_release,
};

ssize_t xpdma_write (struct file *filp, const char *buf, size_t count, loff_t *f_pos)
{
//    dma_addr_t dma_addr;

    /*if ( (count % 4) != 0 )  {
        printk("%s: xpdma_writeMem: Buffer length not dword aligned.\n",DEVICE_NAME);
        return (CRIT_ERR);
    }*/

    // Now it is safe to copy the data from user space.
    if ( copy_from_user(gWriteBuffer, buf, count) )  {
        printk("%s: xpdma_writeMem: Failed copy from user.\n", DEVICE_NAME);
        return (CRIT_ERR);
    }

    //TODO: set DMA semaphore

    printk("%s: xpdma_writeMem: WriteBuf Virt Addr = %lX Phy Addr = %lX.\n",
           DEVICE_NAME, (size_t)gWriteBuffer, (size_t)gWriteHWAddr);

    //TODO: release DMA semaphore

    printk(KERN_INFO"%s: XPCIe_Write: %lu bytes have been written...\n", DEVICE_NAME, count);

    return (SUCCESS);
}

ssize_t xpdma_read (struct file *filp, char *buf, size_t count, loff_t *f_pos)
{
    //TODO: set DMA semaphore

    printk("%s: xpdma_readMem: ReadBuf Virt Addr = %lX Phy Addr = %lX.\n",
           DEVICE_NAME, (size_t)gReadBuffer, (size_t)gReadHWAddr);

    //TODO: release DMA semaphore

    // copy the data to user space.
    if ( copy_to_user(buf, gReadBuffer, count) )  {
        printk("%s: xpdma_readMem: Failed copy to user.\n", DEVICE_NAME);
        return (CRIT_ERR);
    }

    printk(KERN_INFO"%s: XPCIe_Read: %lu bytes have been read...\n", DEVICE_NAME, count);
    return (SUCCESS);
}

long xpdma_ioctl (struct file *filp, unsigned int cmd, unsigned long arg)
{
    u32 regx = 0;

//    printk(KERN_INFO"%s: Ioctl command: %d \n", DEVICE_NAME, cmd);
    switch (cmd) {
        case IOCTL_RESET:
            xpdma_reset();
            break;
        case IOCTL_RDCDMAREG: // Read CDMA config registers
            printk(KERN_INFO"%s: Read Register 0x%X\n", DEVICE_NAME, (*(u32 *)arg));
            regx = xpdma_readReg(*((u32 *)arg));
            *((u32 *)arg) = regx;
            printk(KERN_INFO"%s: Readed value 0x%X\n", DEVICE_NAME, regx);
            break;
        case IOCTL_WRCDMAREG: // Write CDMA config registers
            printk(KERN_INFO"%s: Write Register 0x%X\n", DEVICE_NAME, (*(cdmaReg_t *)arg).reg);
            printk(KERN_INFO"%s: Write Value 0x%X\n", DEVICE_NAME, (*(cdmaReg_t *)arg).value);
            xpdma_writeReg((*(cdmaReg_t *)arg).reg, (*(cdmaReg_t *)arg).value);
            break;
        case IOCTL_RDCFGREG:
            // TODO: Read PCIe config registers
            break;
        case IOCTL_WRCFGREG:
            // TODO: Write PCIe config registers
            break;
        case IOCTL_SEND:
            // Send data from Host system to AXI CDMA
//            printk(KERN_INFO"%s: Send Data size 0x%X\n", DEVICE_NAME, (*(cdmaBuffer_t *)arg).count);
//            printk(KERN_INFO"%s: Send Data address 0x%X\n", DEVICE_NAME, (*(cdmaBuffer_t *)arg).addr);
            xpdma_send ((*(cdmaBuffer_t *)arg).data, (*(cdmaBuffer_t *)arg).count, (*(cdmaBuffer_t *)arg).addr);
//            printk(KERN_INFO"%s: Sended\n", DEVICE_NAME);
            break;
        case IOCTL_RECV:
            // Receive data from AXI CDMA to Host system
//            printk(KERN_INFO"%s: Receive Data size 0x%X\n", DEVICE_NAME, (*(cdmaBuffer_t *)arg).count);
//            printk(KERN_INFO"%s: Receive Data address 0x%X\n", DEVICE_NAME, (*(cdmaBuffer_t *)arg).addr);
            xpdma_recv ((*(cdmaBuffer_t *)arg).data, (*(cdmaBuffer_t *)arg).count, (*(cdmaBuffer_t *)arg).addr);
//            printk(KERN_INFO"%s: Received\n", DEVICE_NAME);
            break;
        case IOCTL_INFO:
            xpdma_showInfo ();
        default:
            break;
    }

    return (SUCCESS);
}

void xpdma_showInfo (void)
{
    uint32_t c = 0;

    printk(KERN_INFO"%s: INFORMATION\n", DEVICE_NAME);
    printk(KERN_INFO"%s: HOST REGIONS:\n", DEVICE_NAME);
    printk(KERN_INFO"%s: gBaseVirt: 0x%lX\n", DEVICE_NAME, (size_t) gBaseVirt);
    printk(KERN_INFO"%s: gReadBuffer address: 0x%lX\n", DEVICE_NAME, (size_t) gReadBuffer);
    printk(KERN_INFO"%s: gReadBuffer: %s\n", DEVICE_NAME, gReadBuffer);
    printk(KERN_INFO"%s: gWriteBuffer address: 0x%lX\n", DEVICE_NAME, (size_t) gWriteBuffer);
    printk(KERN_INFO"%s: gWriteBuffer: %s\n", DEVICE_NAME, gWriteBuffer);
    printk(KERN_INFO"%s: gDescChain:          0x%lX\n", DEVICE_NAME, (size_t) gDescChain);
    printk(KERN_INFO"%s: gDescChainLength:   0x%lX\n", DEVICE_NAME, (size_t) gDescChainLength);

    printk(KERN_INFO"%s: REGISTERS:\n", DEVICE_NAME);

    printk(KERN_INFO"%s: BRAM:\n", DEVICE_NAME);
    for (c = 0; c <= 8*4; c += 4)
        printk(KERN_INFO"%s: 0x%08X: 0x%08X\n", DEVICE_NAME, BRAM_OFFSET + c, xpdma_readReg(BRAM_OFFSET + c));

    printk(KERN_INFO"%s: PCIe CTL:\n", DEVICE_NAME);
    printk(KERN_INFO"%s: 0x%08X: 0x%08X\n", DEVICE_NAME, PCIE_CTL_OFFSET, xpdma_readReg(PCIE_CTL_OFFSET));
    for (c = 0x208; c <= 0x234 ; c += 4)
        printk(KERN_INFO"%s: 0x%08X: 0x%08X\n", DEVICE_NAME, PCIE_CTL_OFFSET + c, xpdma_readReg(PCIE_CTL_OFFSET + c));

    printk(KERN_INFO"%s: CDMA CTL:\n", DEVICE_NAME);
    for (c = 0; c <= 0x28; c += 4)
        printk(KERN_INFO"%s: 0x%08X: 0x%08X\n", DEVICE_NAME, CDMA_OFFSET + c, xpdma_readReg(CDMA_OFFSET + c));
}

ssize_t create_desc_chain(int direction, u32 size, u32 addr)
{
    // length of desctriptors chain
    u32 count = 0;
    u32 sgAddr = AXI_PCIE_SG_ADDR; // current descriptor address in chain
    u32 bramAddr = AXI_BRAM_ADDR ; // Translation BRAM Address
    u32 btt = 0;                   // current descriptor BTT
    u32 unmappedSize = size;       // unmapped data size
    u32 srcAddr = 0;               // source address (SG_DM of DDR3)
    u32 destAddr = 0;              // destination address (SG_DM of DDR3)

    gDescChainLength = (size + (u32)(TRANSFER_SIZE) - 1) / (u32)(TRANSFER_SIZE);
//    printk(KERN_INFO"%s: gDescChainLength = %lu\n", DEVICE_NAME, gDescChainLength);

    // TODO: future: add PCI_DMA_NONE as indicator of MEM 2 MEM transitions
    if (direction == PCI_DMA_FROMDEVICE) {
        srcAddr  = AXI_DDR3_ADDR + addr;
        destAddr = AXI_PCIE_DM_ADDR;
    } else if (direction == PCI_DMA_TODEVICE) {
        srcAddr  = AXI_PCIE_DM_ADDR;
        destAddr = AXI_DDR3_ADDR + addr;
    } else {
        printk(KERN_INFO"%s: Descriptors Chain create error: unknown direction\n", DEVICE_NAME);
        return (CRIT_ERR);
    }

    // fill descriptor chain
//    printk(KERN_INFO"%s: fill descriptor chain\n", DEVICE_NAME);
    for (count = 0; count < gDescChainLength; ++count) {
        sg_desc_t *addrDesc = gDescChain + 2 * count; // address translation descriptor
        sg_desc_t *dataDesc = addrDesc + 1;                // target data transfer descriptor
        btt = (unmappedSize > TRANSFER_SIZE) ? TRANSFER_SIZE : unmappedSize;

        // fill address translation descriptor
//        printk(KERN_INFO"%s: fill address translation descriptor\n", DEVICE_NAME);
        addrDesc->nextDesc  = sgAddr + DESCRIPTOR_SIZE;
        addrDesc->srcAddr   = bramAddr;
        addrDesc->destAddr  = AXI_BRAM_ADDR + PCIE_CTL_OFFSET + AXIBAR2PCIEBAR_1U;
        addrDesc->control   = ADDR_BTT;
        addrDesc->status    = 0x00000000;
        sgAddr += DESCRIPTOR_SIZE;

        // fill target data transfer descriptor
//        printk(KERN_INFO"%s: fill address data transfer descriptor\n", DEVICE_NAME);
        dataDesc->nextDesc  = sgAddr + DESCRIPTOR_SIZE;
        dataDesc->srcAddr   = srcAddr;
        dataDesc->destAddr  = destAddr;
        dataDesc->control   = btt;
        dataDesc->status    = 0x00000000;
        sgAddr += DESCRIPTOR_SIZE;

//        printk(KERN_INFO"%s: update counters\n", DEVICE_NAME);
        bramAddr += BRAM_STEP;
        unmappedSize -= btt;
        srcAddr += btt;
        destAddr += btt;
    }

    gDescChain[2 * gDescChainLength - 1].nextDesc = AXI_PCIE_SG_ADDR; // tail descriptor pointed to chain head

    return (SUCCESS);
}

void show_descriptors(void)
{
    int c = 0;
    sg_desc_t *descriptor = gDescChain;

    printk(KERN_INFO
    "%s: Translation vectors:\n", DEVICE_NAME);
    printk(KERN_INFO
    "%s: Operation_1 Upper: %08X\n", DEVICE_NAME, xpdma_readReg(0));
    printk(KERN_INFO
    "%s: Operation_1 Lower: %08X\n", DEVICE_NAME, xpdma_readReg(4));
    printk(KERN_INFO
    "%s: Operation_2 Upper: %08X\n", DEVICE_NAME, xpdma_readReg(8));
    printk(KERN_INFO
    "%s: Operation_2 Lower: %08X\n", DEVICE_NAME, xpdma_readReg(12));

    for (c = 0; c < 4; ++c) {
        printk(KERN_INFO
        "%s: Descriptor %d\n", DEVICE_NAME, c);
        printk(KERN_INFO
        "%s: nextDesc 0x%08X\n", DEVICE_NAME, descriptor->nextDesc);
        printk(KERN_INFO
        "%s: srcAddr 0x%08X\n", DEVICE_NAME, descriptor->srcAddr);
        printk(KERN_INFO
        "%s: destAddr 0x%08X\n", DEVICE_NAME, descriptor->destAddr);
        printk(KERN_INFO
        "%s: control 0x%08X\n", DEVICE_NAME, descriptor->control);
        printk(KERN_INFO
        "%s: status 0x%08X\n", DEVICE_NAME, descriptor->status);
        descriptor++;        // target data transfer descriptor
    }
}

int xpdma_open(struct inode *inode, struct file *filp)
{
    printk(KERN_INFO"%s: Open: module opened\n", DEVICE_NAME);
    return (SUCCESS);
}

static int xpdma_reset(void)
{
    int loop = CDMA_RESET_LOOP;
    u32 tmp;

    printk(KERN_INFO"%s: RESET CDMA\n", DEVICE_NAME);

    xpdma_writeReg((CDMA_OFFSET + CDMA_CONTROL_OFFSET),
                   xpdma_readReg(CDMA_OFFSET + CDMA_CONTROL_OFFSET) | CDMA_CR_RESET_MASK);

    tmp = xpdma_readReg(CDMA_OFFSET + CDMA_CONTROL_OFFSET) & CDMA_CR_RESET_MASK;

    /* Wait for the hardware to finish reset */
    while (loop && tmp) {
        tmp = xpdma_readReg(CDMA_OFFSET + CDMA_CONTROL_OFFSET) & CDMA_CR_RESET_MASK;
        loop--;
    }

    if (!loop) {
        printk(KERN_INFO"%s: reset timeout, CONTROL_REG: 0x%08X, STATUS_REG 0x%08X\n",
                DEVICE_NAME,
                xpdma_readReg(CDMA_OFFSET + CDMA_CONTROL_OFFSET),
                xpdma_readReg(CDMA_OFFSET + CDMA_STATUS_OFFSET));
        return (CRIT_ERR);
    }

    // For Axi CDMA, always do sg transfers if sg mode is built in
    xpdma_writeReg(CDMA_OFFSET + CDMA_CONTROL_OFFSET, tmp | CDMA_CR_SG_EN);

    printk(KERN_INFO"%s: SUCCESSFULLY RESET CDMA!\n", DEVICE_NAME);

    return (SUCCESS);
}

static int xpdma_isIdle(void)
{
    return xpdma_readReg(CDMA_OFFSET + CDMA_STATUS_OFFSET) &
           CDMA_CR_IDLE_MASK;
}

static int sg_operation(int direction, size_t count, u32 addr)
{
    u32 status = 0;
    size_t pntr = 0;
    size_t delayTime = 0;
    u32 countBuf = count;
    size_t bramOffset = 0;

    if (!xpdma_isIdle()){
        printk(KERN_INFO"%s: CDMA is not idle\n", DEVICE_NAME);
        return (SUCCESS);
    }

    // 1. Set DMA to Scatter Gather Mode
//    printk(KERN_INFO"%s: 1. Set DMA to Scatter Gather Mode\n", DEVICE_NAME);
    xpdma_writeReg (CDMA_OFFSET + CDMA_CONTROL_OFFSET, CDMA_CR_SG_EN);

    // 2. Create Descriptors chain
//    printk(KERN_INFO"%s: 2. Create Descriptors chain\n", DEVICE_NAME);
    create_desc_chain(direction, count, addr);

    // 3. Update PCIe Translation vector
    pntr =  (size_t) (gDescChainHWAddr);
//    printk(KERN_INFO"%s: 3. Update PCIe Translation vector\n", DEVICE_NAME);
//    printk(KERN_INFO"%s: gDescChain 0x%016lX\n", DEVICE_NAME, pntr);
    xpdma_writeReg ((PCIE_CTL_OFFSET + AXIBAR2PCIEBAR_0L), (pntr >> 0)  & 0xFFFFFFFF); // Lower 32 bit
    xpdma_writeReg ((PCIE_CTL_OFFSET + AXIBAR2PCIEBAR_0U), (pntr >> 32) & 0xFFFFFFFF); // Upper 32 bit

    // 4. Write appropriate Translation Vectors
//    printk(KERN_INFO"%s: 4. Write Translation Vectors to BRAM\n", DEVICE_NAME);
    if (PCI_DMA_FROMDEVICE == direction) {
        pntr = (size_t)(gReadHWAddr);
    } else if (PCI_DMA_TODEVICE == direction) {
        pntr = (size_t)(gWriteHWAddr);
    } else {
        printk(KERN_INFO"%s: Write Translation Vectors to BRAM error: unknown direction\n", DEVICE_NAME);
        return (CRIT_ERR);
    }

    countBuf = gDescChainLength;
    while (countBuf) {
//        printk(KERN_INFO"%s: pntr 0x%016lX\n", DEVICE_NAME, pntr);
//        printk(KERN_INFO"%s: bramOffset 0x%016lX\n", DEVICE_NAME, bramOffset);
//        printk(KERN_INFO"%s: countBuf 0x%08X\n", DEVICE_NAME, countBuf);
        xpdma_writeReg ((BRAM_OFFSET + bramOffset + 4), (pntr >> 0 ) & 0xFFFFFFFF); // Lower 32 bit
        xpdma_writeReg ((BRAM_OFFSET + bramOffset + 0), (pntr >> 32) & 0xFFFFFFFF); // Upper 32 bit

        pntr += TRANSFER_SIZE;
        bramOffset += BRAM_STEP;
        countBuf--;
    }

    // 5. Write a valid pointer to DMA CURDESC_PNTR
//    printk(KERN_INFO"%s: 5. Write a valid pointer to DMA CURDESC_PNTR\n", DEVICE_NAME);
    xpdma_writeReg ((CDMA_OFFSET + CDMA_CDESC_OFFSET), (AXI_PCIE_SG_ADDR));

    // 6. Write a valid pointer to DMA TAILDESC_PNTR
//    printk(KERN_INFO"%s: 6. Write a valid pointer to DMA TAILDESC_PNTR\n", DEVICE_NAME);
    xpdma_writeReg ((CDMA_OFFSET + CDMA_TDESC_OFFSET), (AXI_PCIE_SG_ADDR) + ((2 * gDescChainLength - 1) * (DESCRIPTOR_SIZE)));

    // wait for Scatter Gather operation...
//    printk(KERN_INFO"%s: Scatter Gather must be started!\n", DEVICE_NAME);

    delayTime = SG_TRANSFER_LOOP;
    while (delayTime) {
        delayTime--;
        udelay(10);// TODO: can it be less?

        status = (gDescChain + 2 * gDescChainLength - 1)->status;

//        printk(KERN_INFO
//        "%s: Scatter Gather Operation: loop counter %08X\n", DEVICE_NAME, SG_TRANSFER_LOOP - delayTime);

//        printk(KERN_INFO
//        "%s: Scatter Gather Operation: status 0x%08X\n", DEVICE_NAME, status);

        if (status & SG_DEC_ERR_MASK) {
            printk(KERN_INFO
            "%s: Scatter Gather Operation: Decode Error\n", DEVICE_NAME);
            show_descriptors();
            return (CRIT_ERR);
        }

        if (status & SG_SLAVE_ERR_MASK) {
            printk(KERN_INFO
            "%s: Scatter Gather Operation: Slave Error\n", DEVICE_NAME);
            show_descriptors();
            return (CRIT_ERR);
        }

        if (status & SG_INT_ERR_MASK) {
            printk(KERN_INFO
            "%s: Scatter Gather Operation: Internal Error\n", DEVICE_NAME);
            show_descriptors();
            return (CRIT_ERR);
        }

        if (status & SG_COMPLETE_MASK) {
//            printk(KERN_INFO
//            "%s: Scatter Gather Operation: Completed successfully\n", DEVICE_NAME);
            return (SUCCESS);
        }
    }
//    printk(KERN_INFO
//    "%s: gReadBuffer: %s\n", DEVICE_NAME, gReadBuffer);
//    printk(KERN_INFO
//    "%s: gWriteBuffer: %s\n", DEVICE_NAME, gWriteBuffer);

    printk(KERN_INFO"%s: Scatter Gather Operation error: Timeout Error\n", DEVICE_NAME);
    show_descriptors();
    return (CRIT_ERR);
}

static int sg_block(int direction, void *data, size_t count, u32 addr)
{
    size_t unsended = count;
    char *curData = data;
    u32 curAddr = addr;
    u32 btt = BUF_SIZE;

    // divide block
    while (unsended) {
        btt = (unsended < BUF_SIZE) ? unsended : BUF_SIZE;
//        printk(KERN_INFO"%s: SG Block: BTT=%u\tunsended=%lu \n", DEVICE_NAME, btt, unsended);

        // TODO: remove this multiple checks
        if (PCI_DMA_TODEVICE == direction)
            if ( copy_from_user(gWriteBuffer, curData, btt) )  {
                printk("%s: sg_block: Failed copy from user.\n", DEVICE_NAME);
                return (CRIT_ERR);
            }

        sg_operation(direction, btt, curAddr);

        // TODO: remove this multiple checks
        if (PCI_DMA_FROMDEVICE == direction)
            if ( copy_to_user(curData, gReadBuffer, btt) )  {
                printk("%s: sg_block: Failed copy to user.\n", DEVICE_NAME);
                return (CRIT_ERR);
            }

        curData += BUF_SIZE;
        curAddr += BUF_SIZE;
        unsended -= btt;
    }

    return (SUCCESS);
}

ssize_t xpdma_send (void *data, size_t count, u32 addr)
{
    sg_block(PCI_DMA_TODEVICE, (void *)data, count, addr);
    return (SUCCESS);
}

ssize_t xpdma_recv (void *data, size_t count, u32 addr)
{
    sg_block(PCI_DMA_FROMDEVICE, (void *)data, count, addr);
    return (SUCCESS);
}

int xpdma_release(struct inode *inode, struct file *filp)
{
    printk(KERN_INFO"%s: Release: module released\n", DEVICE_NAME);
    return (SUCCESS);
}

// IO access (with byte addressing)
static inline u32 xpdma_readReg (u32 reg)
{
//    printk(KERN_INFO"%s: xpdma_readReg: address:0x%08X\t\n", DEVICE_NAME, reg);
    return readl(gBaseVirt + reg);
}

static inline void xpdma_writeReg (u32 reg, u32 val)
{
//    u32 prev = xpdma_readReg(reg);
//    printk(KERN_INFO"%s: xpdma_writeReg: address:0x%08X\t data:0x%08X -> 0x%08X\n", DEVICE_NAME, reg, prev, val);
    writel(val, (gBaseVirt + reg));
}

static int xpdma_init (void)
{
    gDev = pci_get_device(VENDOR_ID, DEVICE_ID, gDev);
    if (NULL == gDev) {
        printk(KERN_WARNING"%s: Init: Hardware not found.\n", DEVICE_NAME);
        return (CRIT_ERR);
    }

    // Set Bus Master Enable (BME) bit
    pci_set_master(gDev);

    // Get Base Address of BAR0 registers
    gBaseHdwr = pci_resource_start(gDev, 0);
    if (0 > gBaseHdwr) {
        printk(KERN_WARNING"%s: Init: Base Address not set.\n", DEVICE_NAME);
        return (CRIT_ERR);
    }
    printk(KERN_INFO"%s: Init: Base hw val %X\n", DEVICE_NAME, (unsigned int) gBaseHdwr);

    // Get the Base Address Length
    gBaseLen = pci_resource_len(gDev, 0);
    printk(KERN_INFO"%s: Init: Base hw len %d\n", DEVICE_NAME, (unsigned int) gBaseLen);

    // Get Virtual HW address
    gBaseVirt = ioremap(gBaseHdwr, gBaseLen);
    if (!gBaseVirt) {
        printk(KERN_WARNING"%s: Init: Could not remap memory.\n", DEVICE_NAME);
        return (CRIT_ERR);
    }
//    printk(KERN_INFO"%s: Init: Virt HW address %lX\n", DEVICE_NAME, (size_t) gBaseVirt);

    // Check the memory region to see if it is in use
    if (0 > check_mem_region(gBaseHdwr, gBaseLen)) {
        printk(KERN_WARNING"%s: Init: Memory in use.\n", DEVICE_NAME);
        return (CRIT_ERR);
    }

    // Try to gain exclusive control of memory for demo hardware.
    request_mem_region(gBaseHdwr, gBaseLen, "Xilinx_PCIe_CDMA_Driver");
    gStatFlags = gStatFlags | HAVE_MEM_REGION;
    printk(KERN_INFO"%s: Init: Initialize Hardware Done..\n", DEVICE_NAME);

    // Bus Master Enable
    if (0 > pci_enable_device(gDev)) {
        printk(KERN_WARNING"%s: Init: Device not enabled.\n", DEVICE_NAME);
        return (CRIT_ERR);
    }

    // Set DMA Mask
    if (0 > pci_set_dma_mask(gDev, 0x7FFFFFFFFFFFFFFF)) {
        printk("%s: Init: DMA not supported\n", DEVICE_NAME);
        return (CRIT_ERR);
    }
    pci_set_consistent_dma_mask(gDev, 0x7FFFFFFFFFFFFFFF);

    gReadBuffer = dma_alloc_coherent( &gDev->dev, BUF_SIZE, &gReadHWAddr, GFP_KERNEL );
    if (NULL == gReadBuffer) {
        printk(KERN_CRIT"%s: Init: Unable to allocate gReadBuffer\n", DEVICE_NAME);
        return (CRIT_ERR);
    }
    printk(KERN_CRIT"%s: Init: Read buffer allocated: 0x%016lX, Phy:0x%016lX\n",
            DEVICE_NAME, (size_t) gReadBuffer, (size_t) gReadHWAddr);

    gWriteBuffer = dma_alloc_coherent( &gDev->dev, BUF_SIZE, &gWriteHWAddr, GFP_KERNEL );
    if (NULL == gWriteBuffer) {
        printk(KERN_CRIT"%s: Init: Unable to allocate gWriteBuffer\n", DEVICE_NAME);
        return (CRIT_ERR);
    }
    printk(KERN_CRIT"%s: Init: Write buffer allocated: 0x%016lX, Phy:0x%016lX\n",
            DEVICE_NAME, (size_t) gWriteBuffer, (size_t) gWriteHWAddr);

    gDescChain = dma_alloc_coherent( &gDev->dev, BUF_SIZE, &gDescChainHWAddr, GFP_KERNEL );
    if (NULL == gDescChain) {
        printk(KERN_CRIT"%s: Init: Unable to allocate gDescChain\n", DEVICE_NAME);
        return (CRIT_ERR);
    }
    printk(KERN_CRIT"%s: Init: Descriptor chain buffer allocated: 0x%016lX, Phy:0x%016lX\n",
            DEVICE_NAME, (size_t) (gDescChain), (size_t) gDescChainHWAddr);

    // Register driver as a character device.
    if (0 > register_chrdev(gDrvrMajor, DEVICE_NAME, &xpdma_intf)) {
        printk(KERN_WARNING"%s: Init: will not register\n", DEVICE_NAME);
        return (CRIT_ERR);
    }
    printk(KERN_INFO"%s: Init: module registered\n", DEVICE_NAME);

    gStatFlags = gStatFlags | HAVE_KERNEL_REG;
    printk(KERN_INFO"%s: driver is loaded\n", DEVICE_NAME);

    // try to reset CDMA
    if (xpdma_reset()) {
        printk(KERN_INFO"%s: RESET timeout\n", DEVICE_NAME);
        return (CRIT_ERR);
    }

    return (SUCCESS);
}

static void xpdma_exit (void)
{
    // Check if we have a memory region and free it
    if (gStatFlags & HAVE_MEM_REGION) {
        (void) release_mem_region(gBaseHdwr, gBaseLen);
    }

    printk(KERN_INFO"%s: xpdma_exit: erase gReadBuffer\n", DEVICE_NAME);
    // Free Write, Read and Descriptor buffers allocated to use
    if (NULL != gReadBuffer)
        dma_free_coherent( &gDev->dev, BUF_SIZE, gReadBuffer, gReadHWAddr);

    printk(KERN_INFO"%s: xpdma_exit: erase gWriteBuffer\n", DEVICE_NAME);
    if (NULL != gWriteBuffer)
        dma_free_coherent( &gDev->dev, BUF_SIZE, gWriteBuffer, gWriteHWAddr);

    printk(KERN_INFO"%s: xpdma_exit: erase gDescChain\n", DEVICE_NAME);
    if (NULL != gWriteBuffer)
        dma_free_coherent( &gDev->dev, BUF_SIZE, gDescChain, gDescChainHWAddr);

    gReadBuffer = NULL;
    gWriteBuffer = NULL;
    gDescChain = NULL;

    // Unmap virtual device address
    printk(KERN_INFO"%s: xpdma_exit: unmap gBaseVirt\n", DEVICE_NAME);
    if (gBaseVirt != NULL)
        iounmap(gBaseVirt);

    gBaseVirt = NULL;

    // Unregister Device Driver
    if (gStatFlags & HAVE_KERNEL_REG)
        unregister_chrdev(gDrvrMajor, DEVICE_NAME);

    gStatFlags = 0;
    printk(KERN_ALERT"%s: driver is unloaded\n", DEVICE_NAME);
}

module_init(xpdma_init);
module_exit(xpdma_exit);
