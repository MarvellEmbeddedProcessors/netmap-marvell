/*******************************************************************************
Copyright (C) Marvell International Ltd. and its affiliates

This software file (the "File") is owned and distributed by Marvell
International Ltd. and/or its affiliates ("Marvell") under the following
alternative licensing terms.  Once you have made an election to distribute the
File under one of the following license alternatives, please (i) delete this
introductory statement regarding license alternatives, (ii) delete the two
license alternatives that you have not elected to use and (iii) preserve the
Marvell copyright notice above.

********************************************************************************
Marvell GPL License Option

If you received this File from Marvell, you may opt to use, redistribute and/or
modify this File in accordance with the terms and conditions of the General
Public License Version 2, June 1991 (the "GPL License"), a copy of which is
available along with the File in the license.txt file or by writing to the Free
Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 or
on the worldwide web at http://www.gnu.org/licenses/gpl.txt.

THE FILE IS DISTRIBUTED AS-IS, WITHOUT WARRANTY OF ANY KIND, AND THE IMPLIED
WARRANTIES OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE ARE EXPRESSLY
DISCLAIMED.  The GPL License provides additional details about this warranty
disclaimer.
*******************************************************************************/
/* mv_neta_netmap.h */

#ifndef __MV_NETA_NETMAP_H__
#define __MV_NETA_NETMAP_H__

#include <bsd_glue.h>
#include <net/netmap.h>
#include <dev/netmap/netmap_kern.h>

#define SOFTC_T	eth_port

/*
 * Register/unregister
 * adapter is pointer to eth_port
 */
static int mv_neta_netmap_reg(struct ifnet *ifp, int onoff)
{
	struct eth_port *adapter = MV_ETH_PRIV(ifp);
	struct netmap_adapter *na = NA(ifp);
	int error = 0;

	if (na == NULL)
		return -EINVAL;

	if (!(ifp->flags & IFF_UP)) {
		/* mv_eth_open has not been called yet, so resources
		 * are not allocated */
		printk(KERN_ERR "Interface is down!");
		return -EINVAL;
	}

	/* stop current interface */
	if (mv_eth_stop(ifp)) {
		printk(KERN_ERR "%s: stop interface failed\n", ifp->name);
		return -EINVAL;
	}

	if (onoff) { /* enable netmap mode */
		mv_eth_rx_reset(adapter->port);
		mv_eth_txp_reset(adapter->port, 0);
		ifp->if_capenable |= IFCAP_NETMAP;
		na->if_transmit = (void *)ifp->netdev_ops;
		ifp->netdev_ops = &na->nm_ndo;
		set_bit(MV_ETH_F_IFCAP_NETMAP_BIT, &(adapter->flags));
#ifdef CONFIG_MV_ETH_BM_CPU
		mv_eth_pool_free(adapter->pool_long->pool, adapter->pool_long_num);
#endif
	} else {
#ifdef CONFIG_MV_ETH_BM_CPU
		unsigned long flags = 0;
		u_int pa, i;
#endif
		ifp->if_capenable &= ~IFCAP_NETMAP;
		ifp->netdev_ops = (void *)na->if_transmit;
		mvNetaRxReset(adapter->port);
		mvNetaTxpReset(adapter->port, 0);
		clear_bit(MV_ETH_F_IFCAP_NETMAP_BIT, &(adapter->flags));
#ifdef CONFIG_MV_ETH_BM_CPU
		i = 0;
		MV_ETH_LOCK(&adapter->pool_long->lock, flags);
		/* enable complete emptying of the BPPI */
		mvBmConfigSet(MV_BM_EMPTY_LIMIT_MASK);
		do {
			pa = mvBmPoolGet(adapter->pool_long->pool);
			i++;
		} while (pa != 0);

		/* disable complete emptying of the BPPI */
		mvBmConfigClear(MV_BM_EMPTY_LIMIT_MASK);
		MV_ETH_UNLOCK(&adapter->pool_long->lock, flags);
		printk(KERN_ERR "free %d buffers from pool %d ", i, adapter->pool_long->pool);
		mv_eth_pool_add(adapter->pool_long->pool, adapter->pool_long_num);
#endif
	}

	/*rtnl_unlock();  XXX do we need it ? */
	if (mv_eth_start(ifp)) {
		printk(KERN_ERR "%s: start interface failed\n", ifp->name);
		return -EINVAL;
	}
	return error;
}

/*
 * Reconcile kernel and user view of the transmit ring.
 */
static int
mv_neta_netmap_txsync(struct ifnet *ifp, u_int ring_nr, int do_lock)
{
	struct SOFTC_T *adapter = MV_ETH_PRIV(ifp);
	struct tx_queue   *txr = &(adapter->txq_ctrl[ring_nr]);

	struct netmap_adapter *na = NA(ifp);
	struct netmap_kring *kring = &na->tx_rings[ring_nr];
	struct netmap_ring *ring = kring->ring;
	u_int j, k, l, n = 0, lim = kring->nkr_num_slots - 1;
	u_int sent_n;

	/* generate an interrupt approximately every half ring */
	/*int report_frequency = kring->nkr_num_slots >> 1;*/

	/* take a copy of ring->cur now, and never read it again */
	k = ring->cur;
	if (k > lim)
		return netmap_ring_reinit(kring);

	if (do_lock)
		mtx_lock(&kring->q_lock);

	rmb();
	/*
	 * Process new packets to send. j is the current index in the
	 * netmap ring, l is the corresponding index in the NIC ring.
	 */
	j = kring->nr_hwcur;
	if (j != k) {	/* we have new packets to send */
		l = netmap_idx_k2n(kring, j);
		for (n = 0; j != k; n++) {
			/* slot is the current slot in the netmap ring */
			struct netmap_slot *slot = &ring->slot[j];
			/* curr is the current slot in the nic ring */
			struct neta_tx_desc *curr =
				(struct neta_tx_desc *)MV_NETA_QUEUE_DESC_PTR(&txr->q->queueCtrl, l);

			uint64_t paddr;
			void *addr = PNMB(slot, &paddr);
			u_int len = slot->len;

			if (addr == netmap_buffer_base || len > NETMAP_BUF_SIZE) {
				if (do_lock)
					mtx_unlock(&kring->q_lock);
				return netmap_ring_reinit(kring);
			}

			slot->flags &= ~NS_REPORT;

			if (slot->flags & NS_BUF_CHANGED) {
				/*netmap_reload_map(pdev, DMA_TO_DEVICE, old_paddr, addr)*/
				curr->bufPhysAddr = (uint32_t)(paddr);
				curr->dataSize = len;
				curr->command = NETA_TX_L4_CSUM_NOT | NETA_TX_FLZ_DESC_MASK |
					NETA_TX_PKT_OFFSET_MASK(slot->data_offs);
				slot->flags &= ~NS_BUF_CHANGED;
			}
			j = (j == lim) ? 0 : j + 1;
			l = (l == lim) ? 0 : l + 1;
		}
		kring->nr_hwcur = k; /* the saved ring->cur */
		kring->nr_hwavail -= n;

		wmb(); /* synchronize writes to the NIC ring */

		txr->q->queueCtrl.nextToProc = l;
		/* Enable transmit */
		sent_n = n;
		while (sent_n > 0xFF) {
			mvNetaTxqPendDescAdd(adapter->port, 0, ring_nr, 0xFF);
			sent_n -= 0xFF;
		}
		mvNetaTxqPendDescAdd(adapter->port, 0, ring_nr, sent_n);
		/*mmiowb();  XXX where do we need this ?*/
	}

	if (n == 0 || kring->nr_hwavail < 1) {
		int delta;

		delta = mvNetaTxqSentDescProc(adapter->port, 0, ring_nr);
		if (delta)
			kring->nr_hwavail += delta;
	}
	/* update avail to what the kernel knows */
	ring->avail = kring->nr_hwavail;

	if (do_lock)
		mtx_unlock(&kring->q_lock);

	return 0;
}


/*
 * Reconcile kernel and user view of the receive ring.
 */
static int
mv_neta_netmap_rxsync(struct ifnet *ifp, u_int ring_nr, int do_lock)
{
	struct SOFTC_T *adapter = MV_ETH_PRIV(ifp);
	struct netmap_adapter *na = NA(ifp);

	MV_NETA_RXQ_CTRL *rxr = adapter->rxq_ctrl[ring_nr].q;

	struct netmap_kring *kring = &na->rx_rings[ring_nr];
	struct netmap_ring *ring = kring->ring;
	u_int j, l, n;

	int force_update = do_lock || kring->nr_kflags & NKR_PENDINTR;

	uint16_t strip_crc = (1) ? 4 : 0; /* TBD :: remove CRC or not */

	u_int lim   = kring->nkr_num_slots - 1;
	u_int k     = ring->cur;
	u_int resvd = ring->reserved;
	u_int rx_done;

	if (k > lim)
		return netmap_ring_reinit(kring);

	if (do_lock)
		mtx_lock(&kring->q_lock);
	/* hardware memory barrier that prevents any memory read access from being moved */
	/* and executed on the other side of the barrier */
	rmb();
	/*
	 * Import newly received packets into the netmap ring.
	 * j is an index in the netmap ring, l in the NIC ring.
	*/
	l = rxr->queueCtrl.nextToProc;
	j = netmap_idx_n2k(kring, l); /* map NIC ring index to netmap ring index */

	if (netmap_no_pendintr || force_update) { /* netmap_no_pendintr = 1, see netmap.c */
		/* Get number of received packets */
		rx_done = mvNetaRxqBusyDescNumGet(adapter->port, ring_nr);
		rx_done = (rx_done >= lim) ? lim - 1 : rx_done;
		for (n = 0; n < rx_done; n++) {
			struct neta_rx_desc *curr =
				(struct neta_rx_desc *)MV_NETA_QUEUE_DESC_PTR(&rxr->queueCtrl, l);

			/* OPEN :: uint32_t staterr = le32toh(curr->status); */
			/* TBD : check for ERRORs */
			/*if ((staterr & E1000_RXD_STAT_DD) == 0)
			break;*/

			ring->slot[j].len = (curr->dataSize) - strip_crc - MV_ETH_MH_SIZE;
			ring->slot[j].data_offs = NET_SKB_PAD + MV_ETH_MH_SIZE;
#ifdef CONFIG_MV_ETH_BM_CPU
			ring->slot[j].buf_idx = curr->bufCookie;
			ring->slot[j].flags |= NS_BUF_CHANGED;
#endif
			j = (j == lim) ? 0 : j + 1;
			l = (l == lim) ? 0 : l + 1;
		}
		if (n) { /* update the state variables */
			unsigned long flags;
			rxr->queueCtrl.nextToProc = l;
			kring->nr_hwavail += n;
			mvNetaRxqOccupDescDec(adapter->port, ring_nr, rx_done);

			/* enable interrupts */
			local_irq_save(flags);
			MV_REG_WRITE(NETA_INTR_NEW_MASK_REG(adapter->port),
			     (MV_ETH_MISC_SUM_INTR_MASK | MV_ETH_TXDONE_INTR_MASK | MV_ETH_RX_INTR_MASK));
			local_irq_restore(flags);
		}
		kring->nr_kflags &= ~NKR_PENDINTR;
	}

	/* skip past packets that userspace has released */
	j = kring->nr_hwcur; /* netmap ring index */
	if (resvd > 0) {
		if (resvd + ring->avail >= lim + 1) {
			printk(KERN_ERR "XXX invalid reserve/avail %d %d", resvd, ring->avail);
			ring->reserved = resvd = 0;
		}
		k = (k >= resvd) ? k - resvd : k + lim + 1 - resvd;
	}

	if (j != k) { /* userspace has released some packets. */
		l = netmap_idx_k2n(kring, j); /* NIC ring index */
		for (n = 0; j != k; n++) {
			struct netmap_slot *slot = &ring->slot[j];
			struct neta_rx_desc *curr = (struct neta_rx_desc *)MV_NETA_QUEUE_DESC_PTR(&rxr->queueCtrl, l);
			uint64_t paddr;
			uint32_t neta_paddr;
			uint32_t *addr = PNMB(slot, &paddr);

			neta_paddr = (uint32_t)paddr;
			slot->data_offs = NET_SKB_PAD + MV_ETH_MH_SIZE;
			if (addr == netmap_buffer_base) { /* bad buf */
				if (do_lock)
					mtx_unlock(&kring->q_lock);
				return netmap_ring_reinit(kring);
			}
			if (slot->flags & NS_BUF_CHANGED) {
				/*
				netmap_reload_map(pdev, DMA_TO_DEVICE, old_paddr, addr)
				curr->buffer_addr = htole32(paddr);
				*/
#ifdef CONFIG_MV_ETH_BM_CPU
				*addr = slot->buf_idx;
				mvBmPoolPut(adapter->pool_long->pool, neta_paddr);
#else
				mvNetaRxDescFill(curr, paddr, 0);
#endif
				slot->flags &= ~NS_BUF_CHANGED;
			}
			curr->status = 0;
			j = (j == lim) ? 0 : j + 1;
			l = (l == lim) ? 0 : l + 1;
		}
		kring->nr_hwavail -= n;
		kring->nr_hwcur = k;
		/* hardware memory barrier that prevents any memory write access from being moved and */
		/* executed on the other side of the barrier.*/
		wmb();
		/*rxr->queueCtrl.nextToProc = l; // XXX not really used*/
		/*
		 * IMPORTANT: we must leave one free slot in the ring,
		 * so move l back by one unit
		 */
		l = (l == 0) ? lim : l - 1;
		mvNetaRxqNonOccupDescAdd(adapter->port, ring_nr, n);
	}
	/* tell userspace that there are new packets */
	ring->avail = kring->nr_hwavail - resvd;

	if (do_lock)
		mtx_unlock(&kring->q_lock);

	return 0;
}


/* diagnostic routine to catch errors */
static void mv_neta_no_rx_alloc(struct SOFTC_T *a, int n)
{
	printk("mv_neta_skb_alloc should not be called");
}

/*
 * Make the rx ring point to the netmap buffers.
 */
static int neta_netmap_rxq_init_buffers(struct SOFTC_T *adapter, int rxq)
{
	struct ifnet *ifp = adapter->dev; /* struct net_devive */
	struct netmap_adapter *na = NA(ifp);
	struct netmap_slot *slot;
#ifndef CONFIG_MV_ETH_BM_CPU
	struct neta_rx_desc *rx_desc;
#endif
	struct rx_queue *rxr;

	int i, si;
	uint64_t paddr;
	uint32_t neta_paddr, *vaddr;

	if (!(adapter->flags & MV_ETH_F_IFCAP_NETMAP))
		return 0;

	/* initialize the rx ring */
	slot = netmap_reset(na, NR_RX, rxq, 0);
	if (!slot) {
		printk(KERN_ERR "%s: TX slot is null\n", __func__);
		return 1;
	}
	rxr = &(adapter->rxq_ctrl[rxq]);

	for (i = 0; i < rxr->rxq_size; i++) {
		si = netmap_idx_n2k(&na->rx_rings[rxq], i);
		vaddr = PNMB(slot + si, &paddr);
		neta_paddr = (uint32_t)paddr;
#ifdef CONFIG_MV_ETH_BM_CPU
		*vaddr = (slot+si)->buf_idx;
		mvBmPoolPut(adapter->pool_long->pool, neta_paddr);
#else
		rx_desc = (struct neta_rx_desc *)MV_NETA_QUEUE_DESC_PTR(&rxr->q->queueCtrl, i);
		mvNetaRxDescFill(rx_desc, neta_paddr, i);
#endif
	}
	rxr->q->queueCtrl.nextToProc = 0;
	/* Force memory writes to complete */
	wmb();
	return 0;
}

/*
 * Make the tx ring point to the netmap buffers.
*/
static int neta_netmap_txq_init_buffers(struct SOFTC_T *adapter, int txp, int txq)
{
	struct ifnet *ifp = adapter->dev;
	struct netmap_adapter *na = NA(ifp);
	struct netmap_slot *slot;
	struct tx_queue   *txr;
	struct neta_tx_desc *tx_desc;
	int i, si;
	uint64_t paddr;
	int q;

	if (!(adapter->flags & MV_ETH_F_IFCAP_NETMAP))
		return 0;

	q = txp * CONFIG_MV_ETH_TXQ + txq;

	/* initialize the tx ring */
	slot = netmap_reset(na, NR_TX, q, 0);

	if (!slot) {
		printk(KERN_ERR "%s: TX slot is null\n", __func__);
		return 1;
	}

	txr = &adapter->txq_ctrl[q];

	for (i = 0; i < na->num_tx_desc; i++) {
		si = netmap_idx_n2k(&na->tx_rings[q], i);
		PNMB(slot + si, &paddr);
		tx_desc = (struct neta_tx_desc *)MV_NETA_QUEUE_DESC_PTR(&txr->q->queueCtrl, i);
		tx_desc->bufPhysAddr = (uint32_t)(paddr);
	}
	return 0;
}


static void
mv_neta_netmap_attach(struct SOFTC_T *adapter)
{
	struct netmap_adapter na;

	bzero(&na, sizeof(na));

	na.ifp = adapter->dev; /* struct net_device */
	na.separate_locks = 0;
	na.num_tx_desc = adapter->txq_ctrl->txq_size;
	na.num_rx_desc = adapter->rxq_ctrl->rxq_size;
	na.nm_register = mv_neta_netmap_reg;
	na.nm_txsync = mv_neta_netmap_txsync;
	na.nm_rxsync = mv_neta_netmap_rxsync;
	na.num_tx_rings = CONFIG_MV_ETH_TXQ;
	netmap_attach(&na, CONFIG_MV_ETH_RXQ);
}
/* end of file */

#endif  /* __MV_NETA_NETMAP_H__ */
