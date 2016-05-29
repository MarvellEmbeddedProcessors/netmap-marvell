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
#define MVPP2_BM_NETMAP_PKT_SIZE 2048
#define MVPP2_NETMAP_MAX_QUEUES_NUM 	(MVPP2_MAX_CELLS * MVPP2_MAX_PORTS * \
					MVPP2_MAX_RXQ)

struct mvpp2_ntmp_buf_idx {
	int rx;
	int tx;
};

static struct mvpp2_ntmp_buf_idx buf_idx[MVPP2_NETMAP_MAX_QUEUES_NUM];
static int bm_pool_num[MVPP2_MAX_CELLS];

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
	u_int rxq, queue, si, cell_id;

	if (na == NULL)
		return -EINVAL;

	/* stop current interface */
	if (!netif_running(adapter->dev)) {
		return -EINVAL;
	}

	mv_pp2x_stop(adapter->dev);

	DBG_MSG("%s: stopping interface\n", ifp->name);

	cell_id = adapter->priv->pp2_cfg.cell_index;

	/* enable or disable flags and callbacks in na and ifp */
	if (onoff) {
		u_int pool;

		DBG_MSG("Netmap started\n");

		nm_set_native_flags(na);
		adapter->flags |= MVPP2_F_IFCAP_NETMAP;

		if (bm_pool_num[cell_id] == 0) {
			if (mv_pp2x_bm_pool_add(ifp->dev.parent,
				adapter->priv, &pool,
				MVPP2_BM_NETMAP_PKT_SIZE) != 0) {
				DBG_MSG("Unable to allocate a new pool\n");
				return -EINVAL;
			}
			bm_pool_num[cell_id] = pool;
		}
	} else {
		u_int i,idx;

		DBG_MSG("Netmap stop\n");

		nm_clear_native_flags(na);

		if (bm_pool_num[cell_id] != 0) {

			idx = cell_id * MVPP2_MAX_PORTS * MVPP2_MAX_RXQ
			      + adapter->id * MVPP2_MAX_RXQ;
			/* restore buf_idx to original place in ring
			 * Netmap releases buffers according to buf_idx, so
			 * they need to be in place.
			 */
			for (queue = 0; queue < adapter->num_rx_queues;
			     queue++) {
				struct netmap_kring *kring =
					&na->rx_rings[queue];
				struct netmap_ring *ring = kring->ring;
				struct netmap_slot *slot = &ring->slot[0];

				for (i = 0; i < na->num_rx_desc; i++) {
					si = netmap_idx_n2k(
						&na->rx_rings[queue], i);
					(slot+si)->buf_idx =
						buf_idx[idx + queue].rx	+ i;
				}
			}

			for (queue = 0; queue < adapter->num_tx_queues;
			     queue++) {
				struct netmap_kring *kring =
					&na->tx_rings[queue];
				struct netmap_ring *ring = kring->ring;
				struct netmap_slot *slot = &ring->slot[0];

				for (i = 0; i < na->num_rx_desc; i++) {
					si = netmap_idx_n2k(
						&na->tx_rings[queue], i);
					(slot+si)->buf_idx =
						buf_idx[idx + queue].tx + i;
				}
			}

			for (rxq = 0; rxq < adapter->num_rx_queues; rxq++) {
				mv_pp2x_swf_bm_pool_assign(adapter, rxq,
						adapter->pool_long->id,
						adapter->pool_short->id);
			}

			mv_pp2x_bm_pool_destroy(ifp->dev.parent, adapter->priv,
				&adapter->priv->bm_pools[bm_pool_num[cell_id]],
				false);
			bm_pool_num[cell_id] = 0;
		}

		adapter->flags &= ~MVPP2_F_IFCAP_NETMAP;
	}

	if (netif_running(adapter->dev)) {
		mv_pp2x_open(adapter->dev);
		DBG_MSG("%s: starting interface\n", ifp->name);
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
	u_int nm_i;	 /* index into the netmap ring */
	u_int nic_i = 0; /* Number of sent packets from NIC  */
	u_int n;
	u_int const lim = kring->nkr_num_slots - 1;
	u_int count = 0, sent = 0;
	u_int const head = kring->rhead;
	struct mv_pp2x_tx_desc *tx_desc;
	struct mv_pp2x_aggr_tx_queue *aggr_txq = NULL;
	struct mv_pp2x_txq_pcpu *txq_pcpu;
	struct mv_pp2x_tx_queue *txq;
	u_int tx_sent;
	u8 first_addr_space;

	/* generate an interrupt approximately every half ring */
	/*u_int report_frequency = kring->nkr_num_slots >> 1;*/

	/* device-specific */
	/* take a copy of ring->cur now, and never read it again */

	struct SOFTC_T *adapter = netdev_priv(ifp);

	txq = adapter->txqs[ring_nr];
	txq_pcpu = this_cpu_ptr(txq->pcpu);
	aggr_txq = &adapter->priv->aggr_txqs[smp_processor_id()];
	first_addr_space = adapter->priv->pp2_cfg.first_sw_thread;

	/*
	 * Process new packets to send. j is the current index in the
	 * netmap ring, l is the corresponding index in the NIC ring.
	 */

	if (!netif_carrier_ok(ifp)) {
		DBG_MSG("dev_state=%ld\n", ifp->state);
		goto out;
	}

	nm_i = kring->nr_hwcur;
	if (nm_i != head) {	/* we have new packets to send */
		for (n = 0; nm_i != head; n++) {
			/* slot is the current slot in the netmap ring */
			struct netmap_slot *slot = &ring->slot[nm_i];
			u_int len = slot->len;
			uint64_t paddr;
			void *addr = PNMB(na, slot, &paddr);

			/* device-specific */
			NM_CHECK_ADDR_LEN(na, addr, len);
			slot->flags &= ~NS_REPORT;

			dma_sync_single_for_cpu(ifp->dev.parent, paddr,
				len, DMA_BIDIRECTIONAL);

			/* check aggregated TXQ resource */
			if (mv_pp2x_aggr_desc_num_check(adapter->priv,
			    aggr_txq, 1) ||
			    mv_pp2x_txq_reserved_desc_num_proc(
			    adapter->priv,txq, txq_pcpu, 1)) {
				/*return netmap_ring_reinit(kring);*/
				continue;
			}

			tx_desc = mv_pp2x_txq_next_desc_get(aggr_txq);
			tx_desc->phys_txq = txq->id; /* Destination Queue ID */
			mv_pp2x_txdesc_phys_addr_set(adapter->priv->pp2_version,
				(uint32_t)(paddr) & ~MVPP2_TX_DESC_ALIGN,
				tx_desc);
			tx_desc->data_size = len;
			tx_desc->packet_offset = slot->data_offs;

			tx_desc->command = MVPP2_TXD_L4_CSUM_NOT |
				MVPP2_TXD_IP_CSUM_DISABLE | MVPP2_TXD_F_DESC |
				 MVPP2_TXD_L_DESC;

			mv_pp2x_txq_inc_put(adapter->priv->pp2_version,
				txq_pcpu, addr, tx_desc);

			txq_pcpu->count += 1;
			txq_pcpu->reserved_num -= 1;

			if (++count >= (aggr_txq->size >> 2)) {
				wmb(); /* synchronize writes to the NIC ring */
				mv_pp2x_aggr_txq_pend_desc_add(adapter, count);
				sent += count;
				aggr_txq->count += count;
				count = 0;
			}

			if (slot->flags & NS_BUF_CHANGED)
				slot->flags &= ~NS_BUF_CHANGED;

			nm_i = nm_next(nm_i, lim);
		}

		/* Enable transmit */
		if (count > 0) {
			wmb(); /* synchronize writes to the NIC ring */
			mv_pp2x_aggr_txq_pend_desc_add(adapter, count);
			sent += count;
			aggr_txq->count += count;
		}
		kring->nr_hwcur = head; /* the saved ring->cur */
	}

	/*
	 * Second part: reclaim buffers for completed transmissions.
	 */
	nic_i = netmap_idx_k2n(kring, kring->nr_hwtail);
	tx_sent = mv_pp2x_txq_sent_desc_proc(adapter,
			(first_addr_space + txq_pcpu->cpu),txq->id);
	txq_pcpu->count -= tx_sent;

	if (tx_sent >= kring->nkr_num_slots) {
		DBG_MSG("tx_sent: %d, nkr_num_slots: %d\n", tx_sent,
			kring->nkr_num_slots);
		tx_sent = tx_sent % kring->nkr_num_slots;
	}

	nic_i = ((tx_sent + nic_i) % kring->nkr_num_slots);
	kring->nr_hwtail = netmap_idx_n2k(kring, nic_i);

	/*DBG_MSG("hwcur %d, hwtail %d, head %d, tx_sent %d, nic_i: %d, ring_nr: %d\n",
		kring->nr_hwcur, kring->nr_hwtail, head,
		tx_sent, nic_i, ring_nr);*/
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
	u_int cell_id;

	/* device-specific */
	struct SOFTC_T *adapter = netdev_priv(ifp);
	struct queue_vector *q_vec;
	struct mv_pp2x_rx_queue *rxq;

	if (!netif_carrier_ok(ifp)) {
		return 0;
	}
	if (adapter->priv->pp2_cfg.queue_mode == MVPP2_QDIST_SINGLE_MODE)
		q_vec = &adapter->q_vector[num_active_cpus()];
	else
		q_vec = &adapter->q_vector[smp_processor_id()];


	if ((ring_nr < q_vec->first_rx_queue) ||
	    (ring_nr >= (q_vec->first_rx_queue + q_vec->num_rx_queues))) {
		return 0;
	}

	rxq = adapter->rxqs[ring_nr];

	if (head > lim)
		return netmap_ring_reinit(kring);

	rmb();

	/* hardware memory barrier that prevents any memory read access from
	 *  being moved and executed on the other side of the barrier rmb();
	 *
	 * First part: import newly received packets into the netmap ring.
	*/
	/* netmap_no_pendintr = 1, see netmap.c */
	if (netmap_no_pendintr || force_update) {
		/* Get number of received packets */
		uint16_t slot_flags = kring->nkr_slot_flags;
		/* TBD :: remove CRC or not */
		uint16_t strip_crc = (0) ? 4 : 0;

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

#if defined(__BIG_ENDIAN)
			if (adapter->priv->pp2_version == PPV21)
				mv_pp21_rx_desc_swap(curr);
			else
				mv_pp22_rx_desc_swap(curr);
#endif /* __BIG_ENDIAN */

			/* TBD : check for ERRORs */
			slot->len = (curr->data_size) -
						strip_crc - MVPP2_MH_SIZE;
			slot->data_offs = NET_SKB_PAD +
						MVPP2_MH_SIZE;
			slot->buf_idx = (uintptr_t)
				mv_pp22_rxdesc_cookie_get(curr);
			slot->flags = slot_flags;
			nm_i = nm_next(nm_i, lim);
			nic_i = nm_next(nic_i, lim);

		}
		if (n) { /* update the state variables */
			rxq->next_desc_to_proc = nic_i;
			kring->nr_hwtail = nm_i;
			wmb(); /* synchronize writes to the NIC ring */
			mv_pp2x_rxq_status_update(adapter, rxq->id, n, 0);
		}
		kring->nr_kflags &= ~NKR_PENDINTR;

	}

	/*
	 * Second part: skip past packets that userspace has released.
	 */
	nm_i  = kring->nr_hwcur; /* netmap ring index */

	if (nm_i != head) { /* userspace has released some packets. */
		nic_i = netmap_idx_k2n(kring, nm_i); /* NIC ring index */
		cell_id = adapter->priv->pp2_cfg.cell_index;

		for (m = 0; nm_i != head; m++) {
			struct netmap_slot *slot = &ring->slot[nm_i];
			struct mv_pp2x_rx_desc *curr =
				MVPP2_QUEUE_DESC_PTR(rxq, nic_i);
			uint64_t paddr;

			void *addr = PNMB(na, slot, &paddr);

			/*
			In big endian mode:
			we do not need to swap descriptor here, allready swapped before
			*/

			if (addr == NETMAP_BUF_BASE(na)) {   /* bad buf */
				DBG_MSG("addr %p\n", addr);
				goto ring_reset;
			}

			/*DBG_MSG("3 na %s slot %p, index %d, new_index %d, paddr %p\n",
				na->name, slot, slot->buf_idx,
				mv_pp22_rxdesc_cookie_get(curr),
				paddr);*/

			dma_sync_single_for_cpu(ifp->dev.parent,
				paddr,
				MVPP2_RX_BUF_SIZE(curr->data_size),
				DMA_FROM_DEVICE);

			mv_pp2x_pool_refill(adapter->priv,
				bm_pool_num[cell_id],
				paddr, mv_pp22_rxdesc_cookie_get(curr));

			if (slot->flags & NS_BUF_CHANGED)
				slot->flags &= ~NS_BUF_CHANGED;

			curr->status = 0;
			nm_i = nm_next(nm_i, lim);
			nic_i = nm_next(nic_i, lim);
		}
		kring->nr_hwcur = head;
		wmb();
		mv_pp2x_rxq_status_update(adapter, rxq->id, 0, m);
		/* enable interrupts */
		mv_pp2x_qvector_interrupt_enable(q_vec);
	}
	return 0;

ring_reset:
	return netmap_ring_reinit(kring);
}

/*
 * Make the rx ring point to the netmap buffers.
 */
static int mv_pp2x_netmap_rxq_init_buffers(struct SOFTC_T *adapter)
{
	struct ifnet *ifp = adapter->dev; /* struct net_devive */
	struct netmap_adapter *na = NA(ifp);
	struct netmap_slot *slot;
	dma_addr_t paddr;
	uint32_t *addr;
	struct mv_pp2x_rx_queue *rxq;
	u_int queue, idx, cell_id;
	u_int added_buffers = 0;
	u_int i, si;

	cell_id = adapter->priv->pp2_cfg.cell_index;
	idx = cell_id * MVPP2_MAX_PORTS * MVPP2_MAX_RXQ
	      + adapter->id * MVPP2_MAX_RXQ;

	for (queue = 0; queue < adapter->num_rx_queues; queue++) {
		rxq = adapter->rxqs[queue];

		/* initialize the rx ring */
		slot = netmap_reset(na, NR_RX, queue, 0);
		if (!slot)
			return 0;	/* not in netmap native mode*/

		buf_idx[idx + queue].rx = slot->buf_idx;

		mv_pp2x_swf_bm_pool_assign(adapter, queue,
			bm_pool_num[cell_id],
			bm_pool_num[cell_id]);

		for (i = 0; i < na->num_rx_desc; i++) {
			si = netmap_idx_n2k(&na->rx_rings[queue], i);
			addr = PNMB(na, slot+si, &paddr);
			mv_pp2x_pool_refill(adapter->priv,
				bm_pool_num[cell_id],
				paddr, (u8 *)(uint64_t)(slot+si)->buf_idx);
		}
		added_buffers += i;
		rxq->next_desc_to_proc = 0;
		/* Force memory writes to complete */
		/*wmb(); */ /* synchronize writes to the NIC ring */
		/*mv_pp2x_rxq_status_update(adapter, rxq->id, 0, rxq->size);*/
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
	u_int queue, idx, cell_id;

	cell_id = adapter->priv->pp2_cfg.cell_index;
	idx = cell_id * MVPP2_MAX_PORTS * MVPP2_MAX_RXQ
	      + adapter->id * MVPP2_MAX_RXQ;

	/* initialize the tx ring */
	for (queue = 0; queue < adapter->num_tx_queues; queue++) {
		txq = adapter->txqs[queue];
		slot = netmap_reset(na, NR_TX, queue, 0);
		if (!slot)
			return 0;


		buf_idx[idx + queue].tx = slot->buf_idx;

	}
	return 1;
}

/* Update the mvpp2x-net device configurations. Number of queues can
 * change dinamically */

static int
mv_pp2x_netmap_config(struct netmap_adapter *na, u_int *txr, u_int *txd,
						u_int *rxr, u_int *rxd)
{
	struct ifnet *ifp = na->ifp;
	struct SOFTC_T *adapter = netdev_priv(ifp);

	*txr = adapter->num_tx_queues;
	*rxr = adapter->num_rx_queues;
	*rxd = adapter->rx_ring_size;
	*txd = adapter->tx_ring_size;

        /*DBG_MSG("mvpp2x config txq=%d, txd=%d rxq=%d, rxd=%d",
		*txr, *txd, *rxr, *rxd);*/

	return 0;
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
	na.nm_config = mv_pp2x_netmap_config;
	na.nm_txsync = mv_pp2x_netmap_txsync;
	na.nm_rxsync = mv_pp2x_netmap_rxsync;
	na.num_tx_rings = adapter->num_tx_queues;
	na.num_rx_rings = adapter->num_rx_queues;
	netmap_attach(&na);
}
/* end of file */

#endif  /* __MV_PP2X_NETMAP_H__ */
