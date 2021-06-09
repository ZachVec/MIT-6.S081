#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock, e1000_lock_recv;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");
  initlock(&e1000_lock_recv, "e1000"); // use to block concurrent recv while receiving.

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

int
e1000_transmit(struct mbuf *m)
{
  //
  // Your code here.
  //
  // the mbuf contains an ethernet frame; program it into
  // the TX descriptor ring so that the e1000 sends it. Stash
  // a pointer so that it can be freed after sending.
  //
  acquire(&e1000_lock);
  uint32 index = regs[E1000_TDT];
  if(!(tx_ring[index].status & E1000_TXD_STAT_DD)){
    // last transmit request hasn't been finished yet.
    // return error
    release(&e1000_lock);
    return -1;
  }

  // otherwise, the mbuf indicated by tx_ring[index]
  // has been transmitted. Then free it.
  if(tx_mbufs[index]){
    mbuffree(tx_mbufs[index]);
  }

  tx_mbufs[index] = m;
  // modify the tx_ring[index], so that rx_desc points to right mbuf
  memset(&tx_ring[index], 0, sizeof(struct tx_desc));
  tx_ring[index].addr   = (uint64)m->head;
  tx_ring[index].length = (uint16)m->len;
  // look at section 3.3.3.1 in the E1000 Manual
  // Actually in e1000_dev.h, there are only 2 bits
  // E1000_TXD_CMD_RS and E1000_TXD_CMD_EOP
  // set report status for future use, and alse the end of packets bit.
  tx_ring[index].cmd    = E1000_TXD_CMD_RS | E1000_TXD_CMD_EOP;
  regs[E1000_TDT]       = (index + 1) % TX_RING_SIZE;
  release(&e1000_lock);
  return 0;
}

static void
e1000_recv(void)
{
  //
  // Your code here.
  //
  // Check for packets that have arrived from the e1000
  // Create and deliver an mbuf for each packet (using net_rx()).
  //
  while(1){
    uint32 index = regs[E1000_RDT];
    index = (index+1) % RX_RING_SIZE;

    // if no new packets available
    if(!(rx_ring[index].status & E1000_RXD_STAT_DD)) return;

    // otherwise, update mbuf
    rx_mbufs[index]->len = rx_ring[index].length;

    // deliver the mbuf to the network statck.
    net_rx(rx_mbufs[index]);

    // alloc a new mbuf to replace the one just given to net_rx()
    struct mbuf *m = mbufalloc(0);
    rx_mbufs[index] = m;
    rx_ring[index].addr   = (uint64) m->head;
    rx_ring[index].status = 0;

    // update reg E1000_RDT
    regs[E1000_RDT] = index;
  }
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  acquire(&e1000_lock_recv);
  regs[E1000_ICR] = 0xffffffff;
  __sync_synchronize();
  e1000_recv();
  release(&e1000_lock_recv);
}
