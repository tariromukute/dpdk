/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2018 Intel Corporation
 */

#include <rte_ethdev_pci.h>

#include "base/ice_sched.h"
#include "ice_ethdev.h"
#include "ice_rxtx.h"

#define ICE_MAX_QP_NUM "max_queue_pair_num"
#define ICE_DFLT_OUTER_TAG_TYPE ICE_AQ_VSI_OUTER_TAG_VLAN_9100

int ice_logtype_init;
int ice_logtype_driver;

static int ice_dev_configure(struct rte_eth_dev *dev);
static int ice_dev_start(struct rte_eth_dev *dev);
static void ice_dev_stop(struct rte_eth_dev *dev);
static void ice_dev_close(struct rte_eth_dev *dev);
static int ice_dev_reset(struct rte_eth_dev *dev);
static void ice_dev_info_get(struct rte_eth_dev *dev,
			     struct rte_eth_dev_info *dev_info);

static const struct rte_pci_id pci_id_ice_map[] = {
	{ RTE_PCI_DEVICE(ICE_INTEL_VENDOR_ID, ICE_DEV_ID_E810C_BACKPLANE) },
	{ RTE_PCI_DEVICE(ICE_INTEL_VENDOR_ID, ICE_DEV_ID_E810C_QSFP) },
	{ RTE_PCI_DEVICE(ICE_INTEL_VENDOR_ID, ICE_DEV_ID_E810C_SFP) },
	{ .vendor_id = 0, /* sentinel */ },
};

static const struct eth_dev_ops ice_eth_dev_ops = {
	.dev_configure                = ice_dev_configure,
	.dev_start                    = ice_dev_start,
	.dev_stop                     = ice_dev_stop,
	.dev_close                    = ice_dev_close,
	.dev_reset                    = ice_dev_reset,
	.rx_queue_start               = ice_rx_queue_start,
	.rx_queue_stop                = ice_rx_queue_stop,
	.tx_queue_start               = ice_tx_queue_start,
	.tx_queue_stop                = ice_tx_queue_stop,
	.rx_queue_setup               = ice_rx_queue_setup,
	.rx_queue_release             = ice_rx_queue_release,
	.tx_queue_setup               = ice_tx_queue_setup,
	.tx_queue_release             = ice_tx_queue_release,
	.dev_infos_get                = ice_dev_info_get,
};

static void
ice_init_controlq_parameter(struct ice_hw *hw)
{
	/* fields for adminq */
	hw->adminq.num_rq_entries = ICE_ADMINQ_LEN;
	hw->adminq.num_sq_entries = ICE_ADMINQ_LEN;
	hw->adminq.rq_buf_size = ICE_ADMINQ_BUF_SZ;
	hw->adminq.sq_buf_size = ICE_ADMINQ_BUF_SZ;

	/* fields for mailboxq, DPDK used as PF host */
	hw->mailboxq.num_rq_entries = ICE_MAILBOXQ_LEN;
	hw->mailboxq.num_sq_entries = ICE_MAILBOXQ_LEN;
	hw->mailboxq.rq_buf_size = ICE_MAILBOXQ_BUF_SZ;
	hw->mailboxq.sq_buf_size = ICE_MAILBOXQ_BUF_SZ;
}

static int
ice_check_qp_num(const char *key, const char *qp_value,
		 __rte_unused void *opaque)
{
	char *end = NULL;
	int num = 0;

	while (isblank(*qp_value))
		qp_value++;

	num = strtoul(qp_value, &end, 10);

	if (!num || (*end == '-') || errno) {
		PMD_DRV_LOG(WARNING, "invalid value:\"%s\" for key:\"%s\", "
			    "value must be > 0",
			    qp_value, key);
		return -1;
	}

	return num;
}

static int
ice_config_max_queue_pair_num(struct rte_devargs *devargs)
{
	struct rte_kvargs *kvlist;
	const char *queue_num_key = ICE_MAX_QP_NUM;
	int ret;

	if (!devargs)
		return 0;

	kvlist = rte_kvargs_parse(devargs->args, NULL);
	if (!kvlist)
		return 0;

	if (!rte_kvargs_count(kvlist, queue_num_key)) {
		rte_kvargs_free(kvlist);
		return 0;
	}

	if (rte_kvargs_process(kvlist, queue_num_key,
			       ice_check_qp_num, NULL) < 0) {
		rte_kvargs_free(kvlist);
		return 0;
	}
	ret = rte_kvargs_process(kvlist, queue_num_key,
				 ice_check_qp_num, NULL);
	rte_kvargs_free(kvlist);

	return ret;
}

static int
ice_res_pool_init(struct ice_res_pool_info *pool, uint32_t base,
		  uint32_t num)
{
	struct pool_entry *entry;

	if (!pool || !num)
		return -EINVAL;

	entry = rte_zmalloc(NULL, sizeof(*entry), 0);
	if (!entry) {
		PMD_INIT_LOG(ERR,
			     "Failed to allocate memory for resource pool");
		return -ENOMEM;
	}

	/* queue heap initialize */
	pool->num_free = num;
	pool->num_alloc = 0;
	pool->base = base;
	LIST_INIT(&pool->alloc_list);
	LIST_INIT(&pool->free_list);

	/* Initialize element  */
	entry->base = 0;
	entry->len = num;

	LIST_INSERT_HEAD(&pool->free_list, entry, next);
	return 0;
}

static int
ice_res_pool_alloc(struct ice_res_pool_info *pool,
		   uint16_t num)
{
	struct pool_entry *entry, *valid_entry;

	if (!pool || !num) {
		PMD_INIT_LOG(ERR, "Invalid parameter");
		return -EINVAL;
	}

	if (pool->num_free < num) {
		PMD_INIT_LOG(ERR, "No resource. ask:%u, available:%u",
			     num, pool->num_free);
		return -ENOMEM;
	}

	valid_entry = NULL;
	/* Lookup  in free list and find most fit one */
	LIST_FOREACH(entry, &pool->free_list, next) {
		if (entry->len >= num) {
			/* Find best one */
			if (entry->len == num) {
				valid_entry = entry;
				break;
			}
			if (!valid_entry ||
			    valid_entry->len > entry->len)
				valid_entry = entry;
		}
	}

	/* Not find one to satisfy the request, return */
	if (!valid_entry) {
		PMD_INIT_LOG(ERR, "No valid entry found");
		return -ENOMEM;
	}
	/**
	 * The entry have equal queue number as requested,
	 * remove it from alloc_list.
	 */
	if (valid_entry->len == num) {
		LIST_REMOVE(valid_entry, next);
	} else {
		/**
		 * The entry have more numbers than requested,
		 * create a new entry for alloc_list and minus its
		 * queue base and number in free_list.
		 */
		entry = rte_zmalloc(NULL, sizeof(*entry), 0);
		if (!entry) {
			PMD_INIT_LOG(ERR,
				     "Failed to allocate memory for "
				     "resource pool");
			return -ENOMEM;
		}
		entry->base = valid_entry->base;
		entry->len = num;
		valid_entry->base += num;
		valid_entry->len -= num;
		valid_entry = entry;
	}

	/* Insert it into alloc list, not sorted */
	LIST_INSERT_HEAD(&pool->alloc_list, valid_entry, next);

	pool->num_free -= valid_entry->len;
	pool->num_alloc += valid_entry->len;

	return valid_entry->base + pool->base;
}

static void
ice_res_pool_destroy(struct ice_res_pool_info *pool)
{
	struct pool_entry *entry, *next_entry;

	if (!pool)
		return;

	for (entry = LIST_FIRST(&pool->alloc_list);
	     entry && (next_entry = LIST_NEXT(entry, next), 1);
	     entry = next_entry) {
		LIST_REMOVE(entry, next);
		rte_free(entry);
	}

	for (entry = LIST_FIRST(&pool->free_list);
	     entry && (next_entry = LIST_NEXT(entry, next), 1);
	     entry = next_entry) {
		LIST_REMOVE(entry, next);
		rte_free(entry);
	}

	pool->num_free = 0;
	pool->num_alloc = 0;
	pool->base = 0;
	LIST_INIT(&pool->alloc_list);
	LIST_INIT(&pool->free_list);
}

static void
ice_vsi_config_default_rss(struct ice_aqc_vsi_props *info)
{
	/* Set VSI LUT selection */
	info->q_opt_rss = ICE_AQ_VSI_Q_OPT_RSS_LUT_VSI &
			  ICE_AQ_VSI_Q_OPT_RSS_LUT_M;
	/* Set Hash scheme */
	info->q_opt_rss |= ICE_AQ_VSI_Q_OPT_RSS_TPLZ &
			   ICE_AQ_VSI_Q_OPT_RSS_HASH_M;
	/* enable TC */
	info->q_opt_tc = ICE_AQ_VSI_Q_OPT_TC_OVR_M;
}

static enum ice_status
ice_vsi_config_tc_queue_mapping(struct ice_vsi *vsi,
				struct ice_aqc_vsi_props *info,
				uint8_t enabled_tcmap)
{
	uint16_t bsf, qp_idx;

	/* default tc 0 now. Multi-TC supporting need to be done later.
	 * Configure TC and queue mapping parameters, for enabled TC,
	 * allocate qpnum_per_tc queues to this traffic.
	 */
	if (enabled_tcmap != 0x01) {
		PMD_INIT_LOG(ERR, "only TC0 is supported");
		return -ENOTSUP;
	}

	vsi->nb_qps = RTE_MIN(vsi->nb_qps, ICE_MAX_Q_PER_TC);
	bsf = rte_bsf32(vsi->nb_qps);
	/* Adjust the queue number to actual queues that can be applied */
	vsi->nb_qps = 0x1 << bsf;

	qp_idx = 0;
	/* Set tc and queue mapping with VSI */
	info->tc_mapping[0] = rte_cpu_to_le_16((qp_idx <<
						ICE_AQ_VSI_TC_Q_OFFSET_S) |
					       (bsf << ICE_AQ_VSI_TC_Q_NUM_S));

	/* Associate queue number with VSI */
	info->mapping_flags |= rte_cpu_to_le_16(ICE_AQ_VSI_Q_MAP_CONTIG);
	info->q_mapping[0] = rte_cpu_to_le_16(vsi->base_queue);
	info->q_mapping[1] = rte_cpu_to_le_16(vsi->nb_qps);
	info->valid_sections |=
		rte_cpu_to_le_16(ICE_AQ_VSI_PROP_RXQ_MAP_VALID);
	/* Set the info.ingress_table and info.egress_table
	 * for UP translate table. Now just set it to 1:1 map by default
	 * -- 0b 111 110 101 100 011 010 001 000 == 0xFAC688
	 */
#define ICE_TC_QUEUE_TABLE_DFLT 0x00FAC688
	info->ingress_table  = rte_cpu_to_le_32(ICE_TC_QUEUE_TABLE_DFLT);
	info->egress_table   = rte_cpu_to_le_32(ICE_TC_QUEUE_TABLE_DFLT);
	info->outer_up_table = rte_cpu_to_le_32(ICE_TC_QUEUE_TABLE_DFLT);
	return 0;
}

static int
ice_init_mac_address(struct rte_eth_dev *dev)
{
	struct ice_hw *hw = ICE_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	if (!is_unicast_ether_addr
		((struct ether_addr *)hw->port_info[0].mac.lan_addr)) {
		PMD_INIT_LOG(ERR, "Invalid MAC address");
		return -EINVAL;
	}

	ether_addr_copy((struct ether_addr *)hw->port_info[0].mac.lan_addr,
			(struct ether_addr *)hw->port_info[0].mac.perm_addr);

	dev->data->mac_addrs = rte_zmalloc(NULL, sizeof(struct ether_addr), 0);
	if (!dev->data->mac_addrs) {
		PMD_INIT_LOG(ERR,
			     "Failed to allocate memory to store mac address");
		return -ENOMEM;
	}
	/* store it to dev data */
	ether_addr_copy((struct ether_addr *)hw->port_info[0].mac.perm_addr,
			&dev->data->mac_addrs[0]);
	return 0;
}

/*  Initialize SW parameters of PF */
static int
ice_pf_sw_init(struct rte_eth_dev *dev)
{
	struct ice_pf *pf = ICE_DEV_PRIVATE_TO_PF(dev->data->dev_private);
	struct ice_hw *hw = ICE_PF_TO_HW(pf);

	if (ice_config_max_queue_pair_num(dev->device->devargs) > 0)
		pf->lan_nb_qp_max =
			ice_config_max_queue_pair_num(dev->device->devargs);
	else
		pf->lan_nb_qp_max =
			(uint16_t)RTE_MIN(hw->func_caps.common_cap.num_txq,
					  hw->func_caps.common_cap.num_rxq);

	pf->lan_nb_qps = pf->lan_nb_qp_max;

	return 0;
}

static struct ice_vsi *
ice_setup_vsi(struct ice_pf *pf, enum ice_vsi_type type)
{
	struct ice_hw *hw = ICE_PF_TO_HW(pf);
	struct ice_vsi *vsi = NULL;
	struct ice_vsi_ctx vsi_ctx;
	int ret;
	uint16_t max_txqs[ICE_MAX_TRAFFIC_CLASS] = { 0 };
	uint8_t tc_bitmap = 0x1;

	/* hw->num_lports = 1 in NIC mode */
	vsi = rte_zmalloc(NULL, sizeof(struct ice_vsi), 0);
	if (!vsi)
		return NULL;

	vsi->idx = pf->next_vsi_idx;
	pf->next_vsi_idx++;
	vsi->type = type;
	vsi->adapter = ICE_PF_TO_ADAPTER(pf);
	vsi->max_macaddrs = ICE_NUM_MACADDR_MAX;
	vsi->vlan_anti_spoof_on = 0;
	vsi->vlan_filter_on = 1;
	TAILQ_INIT(&vsi->mac_list);
	TAILQ_INIT(&vsi->vlan_list);

	memset(&vsi_ctx, 0, sizeof(vsi_ctx));
	/* base_queue in used in queue mapping of VSI add/update command.
	 * Suppose vsi->base_queue is 0 now, don't consider SRIOV, VMDQ
	 * cases in the first stage. Only Main VSI.
	 */
	vsi->base_queue = 0;
	switch (type) {
	case ICE_VSI_PF:
		vsi->nb_qps = pf->lan_nb_qps;
		ice_vsi_config_default_rss(&vsi_ctx.info);
		vsi_ctx.alloc_from_pool = true;
		vsi_ctx.flags = ICE_AQ_VSI_TYPE_PF;
		/* switch_id is queried by get_switch_config aq, which is done
		 * by ice_init_hw
		 */
		vsi_ctx.info.sw_id = hw->port_info->sw_id;
		vsi_ctx.info.sw_flags2 = ICE_AQ_VSI_SW_FLAG_LAN_ENA;
		/* Allow all untagged or tagged packets */
		vsi_ctx.info.vlan_flags = ICE_AQ_VSI_VLAN_MODE_ALL;
		vsi_ctx.info.vlan_flags |= ICE_AQ_VSI_VLAN_EMOD_NOTHING;
		vsi_ctx.info.q_opt_rss = ICE_AQ_VSI_Q_OPT_RSS_LUT_PF |
					 ICE_AQ_VSI_Q_OPT_RSS_TPLZ;
		/* Enable VLAN/UP trip */
		ret = ice_vsi_config_tc_queue_mapping(vsi,
						      &vsi_ctx.info,
						      ICE_DEFAULT_TCMAP);
		if (ret) {
			PMD_INIT_LOG(ERR,
				     "tc queue mapping with vsi failed, "
				     "err = %d",
				     ret);
			goto fail_mem;
		}

		break;
	default:
		/* for other types of VSI */
		PMD_INIT_LOG(ERR, "other types of VSI not supported");
		goto fail_mem;
	}

	/* VF has MSIX interrupt in VF range, don't allocate here */
	if (type == ICE_VSI_PF) {
		ret = ice_res_pool_alloc(&pf->msix_pool,
					 RTE_MIN(vsi->nb_qps,
						 RTE_MAX_RXTX_INTR_VEC_ID));
		if (ret < 0) {
			PMD_INIT_LOG(ERR, "VSI MAIN %d get heap failed %d",
				     vsi->vsi_id, ret);
		}
		vsi->msix_intr = ret;
		vsi->nb_msix = RTE_MIN(vsi->nb_qps, RTE_MAX_RXTX_INTR_VEC_ID);
	} else {
		vsi->msix_intr = 0;
		vsi->nb_msix = 0;
	}
	ret = ice_add_vsi(hw, vsi->idx, &vsi_ctx, NULL);
	if (ret != ICE_SUCCESS) {
		PMD_INIT_LOG(ERR, "add vsi failed, err = %d", ret);
		goto fail_mem;
	}
	/* store vsi information is SW structure */
	vsi->vsi_id = vsi_ctx.vsi_num;
	vsi->info = vsi_ctx.info;
	pf->vsis_allocated = vsi_ctx.vsis_allocd;
	pf->vsis_unallocated = vsi_ctx.vsis_unallocated;

	/* At the beginning, only TC0. */
	/* What we need here is the maximam number of the TX queues.
	 * Currently vsi->nb_qps means it.
	 * Correct it if any change.
	 */
	max_txqs[0] = vsi->nb_qps;
	ret = ice_cfg_vsi_lan(hw->port_info, vsi->idx,
			      tc_bitmap, max_txqs);
	if (ret != ICE_SUCCESS)
		PMD_INIT_LOG(ERR, "Failed to config vsi sched");

	return vsi;
fail_mem:
	rte_free(vsi);
	pf->next_vsi_idx--;
	return NULL;
}

static int
ice_pf_setup(struct ice_pf *pf)
{
	struct ice_vsi *vsi;

	/* Clear all stats counters */
	pf->offset_loaded = FALSE;
	memset(&pf->stats, 0, sizeof(struct ice_hw_port_stats));
	memset(&pf->stats_offset, 0, sizeof(struct ice_hw_port_stats));
	memset(&pf->internal_stats, 0, sizeof(struct ice_eth_stats));
	memset(&pf->internal_stats_offset, 0, sizeof(struct ice_eth_stats));

	vsi = ice_setup_vsi(pf, ICE_VSI_PF);
	if (!vsi) {
		PMD_INIT_LOG(ERR, "Failed to add vsi for PF");
		return -EINVAL;
	}

	pf->main_vsi = vsi;

	return 0;
}

static int
ice_dev_init(struct rte_eth_dev *dev)
{
	struct rte_pci_device *pci_dev;
	struct ice_hw *hw = ICE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ice_pf *pf = ICE_DEV_PRIVATE_TO_PF(dev->data->dev_private);
	int ret;

	dev->dev_ops = &ice_eth_dev_ops;

	pci_dev = RTE_DEV_TO_PCI(dev->device);

	pf->adapter = ICE_DEV_PRIVATE_TO_ADAPTER(dev->data->dev_private);
	pf->adapter->eth_dev = dev;
	pf->dev_data = dev->data;
	hw->back = pf->adapter;
	hw->hw_addr = (uint8_t *)pci_dev->mem_resource[0].addr;
	hw->vendor_id = pci_dev->id.vendor_id;
	hw->device_id = pci_dev->id.device_id;
	hw->subsystem_vendor_id = pci_dev->id.subsystem_vendor_id;
	hw->subsystem_device_id = pci_dev->id.subsystem_device_id;
	hw->bus.device = pci_dev->addr.devid;
	hw->bus.func = pci_dev->addr.function;

	ice_init_controlq_parameter(hw);

	ret = ice_init_hw(hw);
	if (ret) {
		PMD_INIT_LOG(ERR, "Failed to initialize HW");
		return -EINVAL;
	}

	PMD_INIT_LOG(INFO, "FW %d.%d.%05d API %d.%d",
		     hw->fw_maj_ver, hw->fw_min_ver, hw->fw_build,
		     hw->api_maj_ver, hw->api_min_ver);

	ice_pf_sw_init(dev);
	ret = ice_init_mac_address(dev);
	if (ret) {
		PMD_INIT_LOG(ERR, "Failed to initialize mac address");
		goto err_init_mac;
	}

	ret = ice_res_pool_init(&pf->msix_pool, 1,
				hw->func_caps.common_cap.num_msix_vectors - 1);
	if (ret) {
		PMD_INIT_LOG(ERR, "Failed to init MSIX pool");
		goto err_msix_pool_init;
	}

	ret = ice_pf_setup(pf);
	if (ret) {
		PMD_INIT_LOG(ERR, "Failed to setup PF");
		goto err_pf_setup;
	}

	return 0;

err_pf_setup:
	ice_res_pool_destroy(&pf->msix_pool);
err_msix_pool_init:
	rte_free(dev->data->mac_addrs);
err_init_mac:
	ice_sched_cleanup_all(hw);
	rte_free(hw->port_info);
	ice_shutdown_all_ctrlq(hw);

	return ret;
}

static int
ice_release_vsi(struct ice_vsi *vsi)
{
	struct ice_hw *hw;
	struct ice_vsi_ctx vsi_ctx;
	enum ice_status ret;

	if (!vsi)
		return 0;

	hw = ICE_VSI_TO_HW(vsi);

	memset(&vsi_ctx, 0, sizeof(vsi_ctx));

	vsi_ctx.vsi_num = vsi->vsi_id;
	vsi_ctx.info = vsi->info;
	ret = ice_free_vsi(hw, vsi->idx, &vsi_ctx, false, NULL);
	if (ret != ICE_SUCCESS) {
		PMD_INIT_LOG(ERR, "Failed to free vsi by aq, %u", vsi->vsi_id);
		rte_free(vsi);
		return -1;
	}

	rte_free(vsi);
	return 0;
}

static void
ice_dev_stop(struct rte_eth_dev *dev)
{
	struct rte_eth_dev_data *data = dev->data;
	struct ice_pf *pf = ICE_DEV_PRIVATE_TO_PF(dev->data->dev_private);
	uint16_t i;

	/* avoid stopping again */
	if (pf->adapter_stopped)
		return;

	/* stop and clear all Rx queues */
	for (i = 0; i < data->nb_rx_queues; i++)
		ice_rx_queue_stop(dev, i);

	/* stop and clear all Tx queues */
	for (i = 0; i < data->nb_tx_queues; i++)
		ice_tx_queue_stop(dev, i);

	/* Clear all queues and release mbufs */
	ice_clear_queues(dev);

	pf->adapter_stopped = true;
}

static void
ice_dev_close(struct rte_eth_dev *dev)
{
	struct ice_pf *pf = ICE_DEV_PRIVATE_TO_PF(dev->data->dev_private);
	struct ice_hw *hw = ICE_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	ice_dev_stop(dev);

	/* release all queue resource */
	ice_free_queues(dev);

	ice_res_pool_destroy(&pf->msix_pool);
	ice_release_vsi(pf->main_vsi);

	ice_shutdown_all_ctrlq(hw);
}

static int
ice_dev_uninit(struct rte_eth_dev *dev)
{
	struct ice_hw *hw = ICE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ice_pf *pf = ICE_DEV_PRIVATE_TO_PF(dev->data->dev_private);

	ice_dev_close(dev);

	dev->dev_ops = NULL;
	dev->rx_pkt_burst = NULL;
	dev->tx_pkt_burst = NULL;

	rte_free(dev->data->mac_addrs);
	dev->data->mac_addrs = NULL;

	ice_release_vsi(pf->main_vsi);
	ice_sched_cleanup_all(hw);
	rte_free(hw->port_info);
	ice_shutdown_all_ctrlq(hw);

	return 0;
}

static int
ice_dev_configure(__rte_unused struct rte_eth_dev *dev)
{
	struct ice_adapter *ad =
		ICE_DEV_PRIVATE_TO_ADAPTER(dev->data->dev_private);

	/* Initialize to TRUE. If any of Rx queues doesn't meet the
	 * bulk allocation or vector Rx preconditions we will reset it.
	 */
	ad->rx_bulk_alloc_allowed = true;
	ad->tx_simple_allowed = true;

	return 0;
}

static int ice_init_rss(struct ice_pf *pf)
{
	struct ice_hw *hw = ICE_PF_TO_HW(pf);
	struct ice_vsi *vsi = pf->main_vsi;
	struct rte_eth_dev *dev = pf->adapter->eth_dev;
	struct rte_eth_rss_conf *rss_conf;
	struct ice_aqc_get_set_rss_keys key;
	uint16_t i, nb_q;
	int ret = 0;

	rss_conf = &dev->data->dev_conf.rx_adv_conf.rss_conf;
	nb_q = dev->data->nb_rx_queues;
	vsi->rss_key_size = ICE_AQC_GET_SET_RSS_KEY_DATA_RSS_KEY_SIZE;
	vsi->rss_lut_size = hw->func_caps.common_cap.rss_table_size;

	if (!vsi->rss_key)
		vsi->rss_key = rte_zmalloc(NULL,
					   vsi->rss_key_size, 0);
	if (!vsi->rss_lut)
		vsi->rss_lut = rte_zmalloc(NULL,
					   vsi->rss_lut_size, 0);

	/* configure RSS key */
	if (!rss_conf->rss_key) {
		/* Calculate the default hash key */
		for (i = 0; i <= vsi->rss_key_size; i++)
			vsi->rss_key[i] = (uint8_t)rte_rand();
	} else {
		rte_memcpy(vsi->rss_key, rss_conf->rss_key,
			   RTE_MIN(rss_conf->rss_key_len,
				   vsi->rss_key_size));
	}
	rte_memcpy(key.standard_rss_key, vsi->rss_key, vsi->rss_key_size);
	ret = ice_aq_set_rss_key(hw, vsi->idx, &key);
	if (ret)
		return -EINVAL;

	/* init RSS LUT table */
	for (i = 0; i < vsi->rss_lut_size; i++)
		vsi->rss_lut[i] = i % nb_q;

	ret = ice_aq_set_rss_lut(hw, vsi->idx,
				 ICE_AQC_GSET_RSS_LUT_TABLE_TYPE_PF,
				 vsi->rss_lut, vsi->rss_lut_size);
	if (ret)
		return -EINVAL;

	return 0;
}

static int
ice_dev_start(struct rte_eth_dev *dev)
{
	struct rte_eth_dev_data *data = dev->data;
	struct ice_hw *hw = ICE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ice_pf *pf = ICE_DEV_PRIVATE_TO_PF(dev->data->dev_private);
	uint16_t nb_rxq = 0;
	uint16_t nb_txq, i;
	int ret;

	/* program Tx queues' context in hardware */
	for (nb_txq = 0; nb_txq < data->nb_tx_queues; nb_txq++) {
		ret = ice_tx_queue_start(dev, nb_txq);
		if (ret) {
			PMD_DRV_LOG(ERR, "fail to start Tx queue %u", nb_txq);
			goto tx_err;
		}
	}

	/* program Rx queues' context in hardware*/
	for (nb_rxq = 0; nb_rxq < data->nb_rx_queues; nb_rxq++) {
		ret = ice_rx_queue_start(dev, nb_rxq);
		if (ret) {
			PMD_DRV_LOG(ERR, "fail to start Rx queue %u", nb_rxq);
			goto rx_err;
		}
	}

	ret = ice_init_rss(pf);
	if (ret) {
		PMD_DRV_LOG(ERR, "Failed to enable rss for PF");
		goto rx_err;
	}

	ret = ice_aq_set_event_mask(hw, hw->port_info->lport,
				    ((u16)(ICE_AQ_LINK_EVENT_LINK_FAULT |
				     ICE_AQ_LINK_EVENT_PHY_TEMP_ALARM |
				     ICE_AQ_LINK_EVENT_EXCESSIVE_ERRORS |
				     ICE_AQ_LINK_EVENT_SIGNAL_DETECT |
				     ICE_AQ_LINK_EVENT_AN_COMPLETED |
				     ICE_AQ_LINK_EVENT_PORT_TX_SUSPENDED)),
				     NULL);
	if (ret != ICE_SUCCESS)
		PMD_DRV_LOG(WARNING, "Fail to set phy mask");

	pf->adapter_stopped = false;

	return 0;

	/* stop the started queues if failed to start all queues */
rx_err:
	for (i = 0; i < nb_rxq; i++)
		ice_rx_queue_stop(dev, i);
tx_err:
	for (i = 0; i < nb_txq; i++)
		ice_tx_queue_stop(dev, i);

	return -EIO;
}

static int
ice_dev_reset(struct rte_eth_dev *dev)
{
	int ret;

	if (dev->data->sriov.active)
		return -ENOTSUP;

	ret = ice_dev_uninit(dev);
	if (ret) {
		PMD_INIT_LOG(ERR, "failed to uninit device, status = %d", ret);
		return -ENXIO;
	}

	ret = ice_dev_init(dev);
	if (ret) {
		PMD_INIT_LOG(ERR, "failed to init device, status = %d", ret);
		return -ENXIO;
	}

	return 0;
}

static void
ice_dev_info_get(struct rte_eth_dev *dev, struct rte_eth_dev_info *dev_info)
{
	struct ice_pf *pf = ICE_DEV_PRIVATE_TO_PF(dev->data->dev_private);
	struct ice_hw *hw = ICE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ice_vsi *vsi = pf->main_vsi;
	struct rte_pci_device *pci_dev = RTE_DEV_TO_PCI(dev->device);

	dev_info->min_rx_bufsize = ICE_BUF_SIZE_MIN;
	dev_info->max_rx_pktlen = ICE_FRAME_SIZE_MAX;
	dev_info->max_rx_queues = vsi->nb_qps;
	dev_info->max_tx_queues = vsi->nb_qps;
	dev_info->max_mac_addrs = vsi->max_macaddrs;
	dev_info->max_vfs = pci_dev->max_vfs;

	dev_info->rx_offload_capa =
		DEV_RX_OFFLOAD_VLAN_STRIP |
		DEV_RX_OFFLOAD_IPV4_CKSUM |
		DEV_RX_OFFLOAD_UDP_CKSUM |
		DEV_RX_OFFLOAD_TCP_CKSUM |
		DEV_RX_OFFLOAD_QINQ_STRIP |
		DEV_RX_OFFLOAD_OUTER_IPV4_CKSUM |
		DEV_RX_OFFLOAD_VLAN_EXTEND |
		DEV_RX_OFFLOAD_JUMBO_FRAME |
		DEV_RX_OFFLOAD_KEEP_CRC |
		DEV_RX_OFFLOAD_SCATTER |
		DEV_RX_OFFLOAD_VLAN_FILTER;
	dev_info->tx_offload_capa =
		DEV_TX_OFFLOAD_VLAN_INSERT |
		DEV_TX_OFFLOAD_QINQ_INSERT |
		DEV_TX_OFFLOAD_IPV4_CKSUM |
		DEV_TX_OFFLOAD_UDP_CKSUM |
		DEV_TX_OFFLOAD_TCP_CKSUM |
		DEV_TX_OFFLOAD_SCTP_CKSUM |
		DEV_TX_OFFLOAD_OUTER_IPV4_CKSUM |
		DEV_TX_OFFLOAD_TCP_TSO |
		DEV_TX_OFFLOAD_MULTI_SEGS |
		DEV_TX_OFFLOAD_MBUF_FAST_FREE;
	dev_info->rx_queue_offload_capa = 0;
	dev_info->tx_queue_offload_capa = 0;

	dev_info->reta_size = hw->func_caps.common_cap.rss_table_size;
	dev_info->hash_key_size = (VSIQF_HKEY_MAX_INDEX + 1) * sizeof(uint32_t);
	dev_info->flow_type_rss_offloads = ICE_RSS_OFFLOAD_ALL;

	dev_info->default_rxconf = (struct rte_eth_rxconf) {
		.rx_thresh = {
			.pthresh = ICE_DEFAULT_RX_PTHRESH,
			.hthresh = ICE_DEFAULT_RX_HTHRESH,
			.wthresh = ICE_DEFAULT_RX_WTHRESH,
		},
		.rx_free_thresh = ICE_DEFAULT_RX_FREE_THRESH,
		.rx_drop_en = 0,
		.offloads = 0,
	};

	dev_info->default_txconf = (struct rte_eth_txconf) {
		.tx_thresh = {
			.pthresh = ICE_DEFAULT_TX_PTHRESH,
			.hthresh = ICE_DEFAULT_TX_HTHRESH,
			.wthresh = ICE_DEFAULT_TX_WTHRESH,
		},
		.tx_free_thresh = ICE_DEFAULT_TX_FREE_THRESH,
		.tx_rs_thresh = ICE_DEFAULT_TX_RSBIT_THRESH,
		.offloads = 0,
	};

	dev_info->rx_desc_lim = (struct rte_eth_desc_lim) {
		.nb_max = ICE_MAX_RING_DESC,
		.nb_min = ICE_MIN_RING_DESC,
		.nb_align = ICE_ALIGN_RING_DESC,
	};

	dev_info->tx_desc_lim = (struct rte_eth_desc_lim) {
		.nb_max = ICE_MAX_RING_DESC,
		.nb_min = ICE_MIN_RING_DESC,
		.nb_align = ICE_ALIGN_RING_DESC,
	};

	dev_info->speed_capa = ETH_LINK_SPEED_10M |
			       ETH_LINK_SPEED_100M |
			       ETH_LINK_SPEED_1G |
			       ETH_LINK_SPEED_2_5G |
			       ETH_LINK_SPEED_5G |
			       ETH_LINK_SPEED_10G |
			       ETH_LINK_SPEED_20G |
			       ETH_LINK_SPEED_25G |
			       ETH_LINK_SPEED_40G;

	dev_info->nb_rx_queues = dev->data->nb_rx_queues;
	dev_info->nb_tx_queues = dev->data->nb_tx_queues;

	dev_info->default_rxportconf.burst_size = ICE_RX_MAX_BURST;
	dev_info->default_txportconf.burst_size = ICE_TX_MAX_BURST;
	dev_info->default_rxportconf.nb_queues = 1;
	dev_info->default_txportconf.nb_queues = 1;
	dev_info->default_rxportconf.ring_size = ICE_BUF_SIZE_MIN;
	dev_info->default_txportconf.ring_size = ICE_BUF_SIZE_MIN;
}

static int
ice_pci_probe(struct rte_pci_driver *pci_drv __rte_unused,
	      struct rte_pci_device *pci_dev)
{
	return rte_eth_dev_pci_generic_probe(pci_dev,
					     sizeof(struct ice_adapter),
					     ice_dev_init);
}

static int
ice_pci_remove(struct rte_pci_device *pci_dev)
{
	return rte_eth_dev_pci_generic_remove(pci_dev, ice_dev_uninit);
}

static struct rte_pci_driver rte_ice_pmd = {
	.id_table = pci_id_ice_map,
	.drv_flags = RTE_PCI_DRV_NEED_MAPPING | RTE_PCI_DRV_INTR_LSC |
		     RTE_PCI_DRV_IOVA_AS_VA,
	.probe = ice_pci_probe,
	.remove = ice_pci_remove,
};

/**
 * Driver initialization routine.
 * Invoked once at EAL init time.
 * Register itself as the [Poll Mode] Driver of PCI devices.
 */
RTE_PMD_REGISTER_PCI(net_ice, rte_ice_pmd);
RTE_PMD_REGISTER_PCI_TABLE(net_ice, pci_id_ice_map);
RTE_PMD_REGISTER_KMOD_DEP(net_ice, "* igb_uio | uio_pci_generic | vfio-pci");
RTE_PMD_REGISTER_PARAM_STRING(net_ice,
			      ICE_MAX_QP_NUM "=<int>");

RTE_INIT(ice_init_log)
{
	ice_logtype_init = rte_log_register("pmd.net.ice.init");
	if (ice_logtype_init >= 0)
		rte_log_set_level(ice_logtype_init, RTE_LOG_NOTICE);
	ice_logtype_driver = rte_log_register("pmd.net.ice.driver");
	if (ice_logtype_driver >= 0)
		rte_log_set_level(ice_logtype_driver, RTE_LOG_NOTICE);
}