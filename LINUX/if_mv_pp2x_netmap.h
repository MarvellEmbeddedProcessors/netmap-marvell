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
/* mv_pp2x_netmap.h */

#ifndef __MV_PP2X_NETMAP_H__
#define __MV_PP2X_NETMAP_H__

#include <bsd_glue.h>
#include <net/netmap.h>
#include <dev/netmap/netmap_kern.h>

#define SOFTC_T	mv_pp2x_port

static int pool_buf_num[MVPP2_BM_POOLS_NUM];
static struct mv_pp2x_bm_pool *pool_short[MVPP2_MAX_PORTS];


/*
 * Register/unregister
 *	adapter is pointer to eth_port
 */
/*
 * Register/unregister. We are already under netmap lock.
 */
static int
mv_pp2x_netmap_reg(struct netmap_adapter *na, int onoff)
{
	struct ifnet *ifp = na->ifp;
	struct SOFTC_T *adapter = netdev_priv(ifp);
	int rxq;

	if (na == NULL)
		return -EINVAL;


	/* stop current interface */
	if (netif_running(adapter->dev)) {
		mv_pp2x_stop_dev(adapter);
		DBG_MSG("%s: stopping interface\n", ifp->name);
	}


	/* enable or disable flags and callbacks in na and ifp */
	if (onoff) {

		nm_set_native_flags(na);

		/* Keep old number of long pool buffers */
		pool_buf_num[adapter->pool_long->id] =
			adapter->pool_long->buf_num;
		mv_pp2x_bm_bufs_free(adapter->priv, adapter->pool_long,
			adapter->pool_long->buf_num);

		/* set same pool number for short and long packets */
		for (rxq = 0; rxq < adapter->num_rx_queues; rxq++)
			adapter->priv->pp2xdata->mv_pp2x_rxq_short_pool_set(
				&(adapter->priv->hw), adapter->rxqs[rxq]->id,
				 adapter->pool_long->id);

		/* update short pool in software */
		pool_short[adapter->id] = adapter->pool_short;
		adapter->pool_short = adapter->pool_long;

		adapter->flags |= MVPP2_F_IFCAP_NETMAP;
	} else {
		struct sk_buff *vaddr;
		u_int i = 0;

		nm_clear_native_flags(na);

		/* Allocate BM buffers */
		do {
			vaddr = mv_pp2x_bm_virt_addr_get(&(adapter->priv->hw),
				adapter->pool_long->id);
			i++;
		} while (vaddr);

		mv_pp2x_bm_bufs_add(adapter, adapter->pool_long,
			pool_buf_num[adapter->pool_long->id]);

		/* set port's short pool for Linux driver */
		for (rxq = 0; rxq < adapter->num_rx_queues; rxq++)
			adapter->priv->pp2xdata->mv_pp2x_rxq_short_pool_set(
				&(adapter->priv->hw), adapter->rxqs[rxq]->id,
				adapter->pool_short->id);

		/* update short pool in software */
		adapter->pool_short = pool_short[adapter->id];

		adapter->flags &= ~MVPP2_F_IFCAP_NETMAP;
	}

	if (netif_running(adapter->dev)) {
		mv_pp2x_start_dev(adapter);
		mv_pp2x_egress_enable(adapter);
		mv_pp2x_ingress_enable(adapter);
	}

	return 0;
}

/*
 * Reconcile kernel and user view of the transmit ring.
 */
static int
mv_pp2x_netmap_txsync(struct netmap_kring *kring, int flags)
{
	struct netmap_adapter *na = kring->na;
	struct ifnet *ifp = na->ifp;
	struct netmap_ring *ring = kring->ring;
	u_int ring_nr = kring->ring_id;
	u_int nm_i;	/* index into the netmap ring */
	u_int nic_i;	/* index into the NIC ring */
	u_int n;
	u_int const lim = kring->nkr_num_slots - 1;
	u_int sent = 0;
	u_int count = 0;
	u_int const head = kring->rhead;
	struct mv_pp2x_tx_desc *tx_desc;
	struct mv_pp2x_aggr_tx_queue *aggr_txq = NULL;
	struct mv_pp2x_txq_pcpu *txq_pcpu;
	struct mv_pp2x_tx_queue *txq;

	/* generate an interrupt approximately every half ring */
	/*u_int report_frequency = kring->nkr_num_slots >> 1;*/

	/* device-specific */
	/* take a copy of ring->cur now, and never read it again */

	struct SOFTC_T *adapter = netdev_priv(ifp);

	txq = adapter->txqs[ring_nr];
	txq_pcpu = this_cpu_ptr(txq->pcpu);
	aggr_txq = &adapter->priv->aggr_txqs[smp_processor_id()];

	rmb(); /* synchronize */
	/*
	 * Process new packets to send. j is the current index in the
	 * netmap ring, l is the corresponding index in the NIC ring.
	 */

	if (!netif_carrier_ok(ifp)) {
		DBG_MSG("%s: dev_state=%ld\n", __func__, ifp->state);
		goto out;
	}

	nm_i = kring->nr_hwcur;

	if (nm_i != head) {	/* we have new packets to send */
		/*nic_i = netmap_idx_k2n(kring, nm_i);*/
		for (n = 0; nm_i != head; n++) {
			/* slot is the current slot in the netmap ring */
			struct netmap_slot *slot = &ring->slot[nm_i];
			u_int len = slot->len;
			uint64_t paddr;
			void *addr = PNMB(na, slot, &paddr);

			/* device-specific */
			NM_CHECK_ADDR_LEN(na, addr, len);
			slot->flags &= ~NS_REPORT;

			/* check aggregated TXQ resource */
			if (mv_pp2x_aggr_desc_num_check(adapter->priv,
				aggr_txq, 1)) {
				DBG_MSG("%s(%d) reinit ring\n",
					__func__, __LINE__);
				return netmap_ring_reinit(kring);
			}

			tx_desc = mv_pp2x_txq_next_desc_get(aggr_txq);
			tx_desc->phys_txq = txq->id; /* Destination Queue ID */
			mv_pp2x_txdesc_phys_addr_set(adapter->priv->pp2_version,
				(uint32_t)(paddr) & ~MVPP2_TX_DESC_ALIGN,
				tx_desc);
			tx_desc->data_size = len;
			tx_desc->packet_offset = slot->data_offs &
				MVPP2_TX_DESC_ALIGN;
			tx_desc->command = MVPP2_TXD_L4_CSUM_NOT |
				MVPP2_TXD_IP_CSUM_DISABLE | MVPP2_TXD_F_DESC |
				 MVPP2_TXD_L_DESC;

			if (count >= (aggr_txq->size - 1)) {
				wmb(); /* synchronize writes to the NIC ring */
				/* check aggregated TXQ resource */
				if (mv_pp2x_aggr_desc_num_check(adapter->priv,
					aggr_txq, count)) {
					DBG_MSG(
						"%s(%d) not enough free desc in aggr queue\n",
						__func__, __LINE__);
					goto out;
				}
				sent += count;
				aggr_txq->count += count;
				mv_pp2x_aggr_txq_pend_desc_add(adapter, count);
				count = 0;
			}
			count++;

			if (slot->flags & NS_BUF_CHANGED)
				slot->flags &= ~NS_BUF_CHANGED;

			nm_i = nm_next(nm_i, lim);
			nic_i = nm_next(nic_i, lim);
		}
		kring->nr_hwcur = head; /* the saved ring->cur */

		/*DBG_MSG("%s: count: %d\n", __func__, count);*/
		wmb(); /* synchronize writes to the NIC ring */
		/* Enable transmit */

		if (count > 0) {
			/* check aggregated TXQ resource */
			if (mv_pp2x_aggr_desc_num_check(adapter->priv,
				aggr_txq, count)) {
				DBG_MSG(
					"%s(%d) not enough free desc in agg queue\n",
					__func__, __LINE__);
				goto out;
			}
			sent += count;
			aggr_txq->count += count;
			mv_pp2x_aggr_txq_pend_desc_add(adapter, count);
			count = 0;
		}
	}

	/*
	 * Second part: reclaim buffers for completed transmissions.
	 */
	/*DBG_MSG(KERN_ERR "%s: 2, rcur: %d, hwtail: %d, flags: %x\n",
	 *  __func__, kring->rcur, kring->nr_hwtail, flags );*/
	if (flags & NAF_FORCE_RECLAIM || nm_kr_txempty(kring)) {
		/* record completed transmissions using TDH */
		nic_i = mv_pp2x_txq_sent_desc_proc(adapter, txq);
		/*DBG_MSG("%s: nic_i: %d, kring->nkr_num_slots: %d\n",
		 *  __func__, nic_i, kring->nkr_num_slots);*/
		if (nic_i >= kring->nkr_num_slots) {
			DBG_MSG("%s: TDH wrap %d\n", __func__, nic_i);
			nic_i -= kring->nkr_num_slots;
		}
		kring->nr_hwtail = nm_prev(netmap_idx_n2k(kring, nic_i), lim);
	}
out:

	return 0;
}


/*
 * Reconcile kernel and user view of the receive ring.
 */
static int
mv_pp2x_netmap_rxsync(struct netmap_kring *kring, int flags)
{
	struct netmap_adapter *na = kring->na;
	struct ifnet *ifp = na->ifp;
	struct netmap_ring *ring = kring->ring;
	u_int ring_nr = kring->ring_id;
	u_int nm_i;	/* index into the netmap ring */
	u_int nic_i;	/* index into the NIC ring */
	u_int n = 0, m = 0;
	u_int const lim = kring->nkr_num_slots - 1;
	u_int const head = kring->rhead;
	int force_update = (flags & NAF_FORCE_READ) || kring->nr_kflags &
						NKR_PENDINTR;
	u_int rx_done = 0;
	u32 rx_status;
	int rx_bytes;


	/* device-specific */
	struct SOFTC_T *adapter = netdev_priv(ifp);
	struct mv_pp2x_rx_queue *rxq = adapter->rxqs[ring_nr];

	if (!netif_carrier_ok(ifp))
		return 0;

	if (head > lim)
		return netmap_ring_reinit(kring);

	/* hardware memory barrier that prevents any memory read access from
	 *  being moved and executed on the other side of the barrier rmb();
	 *
	 * First part: import newly received packets into the netmap ring.
	*/
	/* netmap_no_pendintr = 1, see netmap.c */
	if (netmap_no_pendintr || force_update) {
		/* Get number of received packets */
		/* TBD :: remove CRC or not */
		uint16_t strip_crc = (1) ? 4 : 0;

		rx_done = mv_pp2x_rxq_received(adapter, rxq->id);
		rx_done = (rx_done >= lim) ? lim - 1 : rx_done;
		nic_i = rxq->next_desc_to_proc;
		nm_i = netmap_idx_n2k(kring, nic_i);

		for (n = 0; n < rx_done; n++) {
			struct mv_pp2x_rx_desc *curr =
				MVPP2_QUEUE_DESC_PTR(rxq, nic_i);
			struct netmap_slot *slot = &ring->slot[nm_i];
			uint64_t paddr;

			PNMB(na, slot, &paddr);
			rx_status = curr->status;
			rx_bytes = curr->data_size - MVPP2_MH_SIZE;

			/*DBG_MSG(KERN_ERR "%s: status = 0x%x, rx_bytes: %d\n",
			 *  __func__, rx_status, rx_bytes);*/

			/* TBD : check for ERRORs */
			ring->slot[nm_i].len = (curr->data_size) -
						strip_crc - MVPP2_MH_SIZE;
			ring->slot[nm_i].data_offs = NET_SKB_PAD +
						MVPP2_MH_SIZE;
			ring->slot[nm_i].buf_idx = (uintptr_t)(struct sk_buff *)
				mv_pp22_rxdesc_cookie_get(curr);
			ring->slot[nm_i].flags = slot->flags;
			ring->slot[nm_i].flags |= NS_BUF_CHANGED;

			nm_i = nm_next(nm_i, lim);
			nic_i = nm_next(nic_i, lim);

		}
		if (n) { /* update the state variables */
			rxq->next_desc_to_proc = nic_i;
			kring->nr_hwtail = nm_i;
			wmb(); /* synchronize writes to the NIC ring */
			/*DBG_MSG(
			 * "mv_pp2x_rxq_status_update: rxq->id: %d, used: %d\n",
			 *  rxq->id, rx_done);*/
			mv_pp2x_rxq_status_update(adapter, rxq->id, rx_done, 0);
			/*DBG_MSG("mv_pp2x_rxq_status_update: finished\n");*/
			/* enable interrupts */
			mv_pp2x_port_interrupts_enable(adapter);
		}
		kring->nr_kflags &= ~NKR_PENDINTR;

	}

	/*
	 * Second part: skip past packets that userspace has released.
	 */
	nm_i  = kring->nr_hwcur; /* netmap ring index */

	if (nm_i != head) { /* userspace has released some packets. */
		nic_i = netmap_idx_k2n(kring, nm_i); /* NIC ring index */

		for (m = 0; nm_i != head; m++) {
			struct netmap_slot *slot = &ring->slot[nm_i];
			uint64_t paddr;

			PNMB(na, slot, &paddr);
#if 0
			if (addr == NETMAP_BUF_BASE(na)) {   /* bad buf */
				goto ring_reset;
			}
#endif
			slot->data_offs = NET_SKB_PAD + MVPP2_MH_SIZE;
			if (slot->flags & NS_BUF_CHANGED) {
				slot->flags &= ~NS_BUF_CHANGED;
				mv_pp2x_pool_refill(adapter->priv,
					adapter->pool_long->id,
					paddr, (struct sk_buff *)
					(uint64_t)slot->buf_idx);
			}
			nm_i = nm_next(nm_i, lim);
			nic_i = nm_next(nic_i, lim);
		}
		kring->nr_hwcur = head;
		/*hardware memory barrier that prevents any memory write access
		 * from being moved and executed on the other side of
		 * the barrier.*/
		wmb(); /* synchronize writes to the NIC ring */
		/*rxr->next_desc_to_proc = nic_i; // XXX not really used */
		/*
		 * IMPORTANT: we must leave one free slot in the ring,
		 * so move nic_i back by one unit
		 */
		nic_i = nm_prev(nic_i, lim);
		/*DBG_MSG("mv_pp2x_rxq_status_update: rxq->id: %d, to free: %d\n",
		 * rxq->id, m);*/
		mv_pp2x_rxq_status_update(adapter, rxq->id, 0, m);
		/*DBG_MSG("mv_pp2x_rxq_status_update: finished\n");*/
	}
	return 0;

#if 0
ring_reset:
	return netmap_ring_reinit(kring);
#endif
}


/* diagnostic routine to catch errors */
static void mv_pp2x_no_rx_alloc(struct SOFTC_T *a, int n)
{
	DBG_MSG("mv_pp2_skb_alloc should not be called");
}

/*
 * Make the rx ring point to the netmap buffers.
 */
static int mv_pp2x_netmap_rxq_init_buffers(struct SOFTC_T *adapter)
{
	struct ifnet *ifp = adapter->dev; /* struct net_devive */
	struct netmap_adapter *na = NA(ifp);
	struct netmap_slot *slot;
	int i, si;
	dma_addr_t paddr;
	uint32_t *addr;
	struct mv_pp2x_rx_queue *rxq;
	int queue;

	for (queue = 0; queue < adapter->num_rx_queues; queue++) {
		rxq = adapter->rxqs[queue];

		/* initialize the rx ring */
		slot = netmap_reset(na, NR_RX, queue, 0);
		if (!slot)
			return 0;	/* not in netmap native mode*/

		DBG_MSG(KERN_ERR "%s: queue: %d, rxq->id: %d, rxq->size: %d\n",
			__func__, queue, rxq->id, rxq->size);

		for (i = 0; i < rxq->size; i++) {
			si = netmap_idx_n2k(&na->rx_rings[queue], i);
			addr = PNMB(na, slot+si, &paddr);

			mv_pp2x_pool_refill(adapter->priv, adapter->pool_long->id,
				paddr, (struct sk_buff *)
				(uint64_t)(slot+si)->buf_idx);
		}

		adapter->pool_long->buf_num += i;
		adapter->pool_long->in_use_thresh =
			adapter->pool_long->buf_num / 4;
		rxq->next_desc_to_proc = 0;
		/* Force memory writes to complete */
		wmb(); /* synchronize writes to the NIC ring */
		mv_pp2x_rxq_status_update(adapter, rxq->id, 0, rxq->size);

	}
	return 1;
}


/*
 * Make the tx ring point to the netmap buffers.
*/
static int mv_pp2x_netmap_txq_init_buffers(struct SOFTC_T *adapter)
{
	struct ifnet *ifp = adapter->dev;
	struct netmap_adapter *na = NA(ifp);
	struct netmap_slot *slot;
	struct mv_pp2x_tx_queue *txq;
	int queue;

	/* initialize the tx ring */
	for (queue = 0; queue < adapter->num_tx_queues; queue++) {
		txq = adapter->txqs[queue];
		DBG_MSG(KERN_ERR "%s: queue: %d, txq->id: %d, txq->size: %d\n",
			__func__, queue, txq->id, txq->size);
		slot = netmap_reset(na, NR_TX, queue, 0);
		if (!slot)
			return 0;
	}

	return 1;
}


static void
mv_pp2x_netmap_attach(struct SOFTC_T *adapter)
{
	struct netmap_adapter na;

	bzero(&na, sizeof(na));

	na.ifp = adapter->dev; /* struct net_device */
	na.num_tx_desc = adapter->tx_ring_size;
	na.num_rx_desc = adapter->rx_ring_size;
	na.nm_register = mv_pp2x_netmap_reg;
	na.nm_txsync = mv_pp2x_netmap_txsync;
	na.nm_rxsync = mv_pp2x_netmap_rxsync;
	na.num_tx_rings = adapter->num_tx_queues;
	na.num_rx_rings = adapter->num_rx_queues;
	netmap_attach(&na);
}
/* end of file */

#endif  /* __MV_PP2X_NETMAP_H__ */
