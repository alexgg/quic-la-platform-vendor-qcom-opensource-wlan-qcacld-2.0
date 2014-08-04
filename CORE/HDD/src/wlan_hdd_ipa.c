/*
 * Copyright (c) 2013-2014 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file was originally distributed by Qualcomm Atheros, Inc.
 * under proprietary terms before Copyright ownership was assigned
 * to the Linux Foundation.
 */
/*========================================================================

\file  wlan_hdd_ipa.c

\brief   WLAN HDD and ipa interface implementation

========================================================================*/

/*--------------------------------------------------------------------------
Include Files
------------------------------------------------------------------------*/
#ifdef IPA_OFFLOAD
#include <wlan_hdd_includes.h>
#include <wlan_hdd_ipa.h>

#include <linux/etherdevice.h>
#include <linux/atomic.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/list.h>
#include <linux/debugfs.h>
#include <linux/inetdevice.h>
#include <linux/ip.h>
#include <wlan_hdd_softap_tx_rx.h>

#include "vos_sched.h"
#include "tl_shim.h"
#include "wlan_qct_tl.h"

#define HDD_IPA_DESC_BUFFER_RATIO 4
#define HDD_IPA_IPV4_NAME_EXT "_ipv4"
#define HDD_IPA_IPV6_NAME_EXT "_ipv6"

#define HDD_IPA_RX_INACTIVITY_MSEC_DELAY 1000

struct llc_snap_hdr {
	uint8_t dsap;
	uint8_t ssap;
	uint8_t resv[4];
	__be16 eth_type;
} __packed;

struct hdd_ipa_tx_hdr {
	struct ethhdr eth;
	struct llc_snap_hdr llc_snap;
} __packed;

/* For Tx pipes, use 802.3 Header format */
static struct hdd_ipa_tx_hdr ipa_tx_hdr = {
	{
		{0xDE, 0xAD, 0xBE, 0xEF, 0xFF, 0xFF},
		{0xDE, 0xAD, 0xBE, 0xEF, 0xFF, 0xFF},
		0x00 /* length can be zero */
	},
	{
		/* LLC SNAP header 8 bytes */
		0xaa, 0xaa,
		{0x03, 0x00, 0x00, 0x00},
		0x0008 /* type value(2 bytes) ,filled by wlan  */
			/* 0x0800 - IPV4, 0x86dd - IPV6 */
	}
};

/*
   +----------+----------+--------------+--------+
   | Reserved | QCMAP ID | interface id | STA ID |
   +----------+----------+--------------+--------+
 */
struct hdd_ipa_cld_hdr {
	uint8_t reserved[2];
	uint8_t iface_id;
	uint8_t sta_id;
} __packed;

struct hdd_ipa_rx_hdr {
	struct hdd_ipa_cld_hdr cld_hdr;
	struct ethhdr eth;
} __packed;

#define HDD_IPA_WLAN_CLD_HDR_LEN	sizeof(struct hdd_ipa_cld_hdr)
#define HDD_IPA_WLAN_TX_HDR_LEN		sizeof(ipa_tx_hdr)
#define HDD_IPA_WLAN_RX_HDR_LEN		sizeof(struct hdd_ipa_rx_hdr)
#define HDD_IPA_WLAN_HDR_DES_MAC_OFFSET 0

#define HDD_IPA_GET_IFACE_ID(_data) \
	(((struct hdd_ipa_cld_hdr *) (_data))->iface_id)


#define HDD_IPA_LOG(LVL, fmt, args...)	VOS_TRACE(VOS_MODULE_ID_HDD, LVL, \
				"%s:%d: "fmt, __func__, __LINE__, ## args)

#define HDD_IPA_DBG_DUMP(_lvl, _prefix, _buf, _len) \
	do {\
		VOS_TRACE(VOS_MODULE_ID_HDD, _lvl, "%s:", _prefix); \
		VOS_TRACE_HEX_DUMP(VOS_MODULE_ID_HDD, _lvl, _buf, _len); \
	} while(0)

enum hdd_ipa_rm_state {
	HDD_IPA_RM_RELEASED,
	HDD_IPA_RM_GRANT_PENDING,
	HDD_IPA_RM_GRANTED,
};

#define HDD_IPA_MAX_IFACE 3
#define HDD_IPA_MAX_SYSBAM_PIPE 4
#define HDD_IPA_RX_PIPE  HDD_IPA_MAX_IFACE

static struct hdd_ipa_adapter_2_client {
	enum ipa_client_type cons_client;
	enum ipa_client_type prod_client;
} hdd_ipa_adapter_2_client[HDD_IPA_MAX_IFACE] = {
	{IPA_CLIENT_WLAN1_CONS, IPA_CLIENT_WLAN1_PROD},
	{IPA_CLIENT_WLAN2_CONS, IPA_CLIENT_WLAN1_PROD},
	{IPA_CLIENT_WLAN3_CONS, IPA_CLIENT_WLAN1_PROD},
};

struct hdd_ipa_sys_pipe {
	uint32_t conn_hdl;
	uint8_t conn_hdl_valid;
	struct ipa_sys_connect_params ipa_sys_params;
};

struct hdd_ipa_iface_stats {
	uint64_t num_tx;
	uint64_t num_tx_drop;
	uint64_t num_tx_err;
	uint64_t num_tx_cac_drop;
	uint64_t num_rx_prefilter;
	uint64_t num_rx_ipa_excep;
	uint64_t num_rx_recv;
	uint64_t num_rx_recv_mul;
	uint64_t num_rx_send_desc_err;
	uint64_t max_rx_mul;
};

struct hdd_ipa_priv;

struct hdd_ipa_iface_context {
	struct hdd_ipa_priv *hdd_ipa;
	hdd_adapter_t  *adapter;
	void *tl_context;

	enum ipa_client_type cons_client;
	enum ipa_client_type prod_client;

	uint8_t iface_id; /* This iface ID */
	uint8_t sta_id; /* This iface station ID */
	adf_os_spinlock_t interface_lock;
	uint32_t ifa_address;
	struct hdd_ipa_iface_stats stats;
};


struct hdd_ipa_stats {
	uint32_t event[IPA_WLAN_EVENT_MAX];
	uint64_t num_send_msg;
	uint64_t num_free_msg;

	uint64_t num_rm_grant;
	uint64_t num_rm_release;
	uint64_t num_rm_grant_imm;
	uint64_t num_cons_perf_req;
	uint64_t num_prod_perf_req;

	uint64_t num_rx_drop;
	uint64_t num_rx_ipa_tx_dp;
	uint64_t num_rx_ipa_splice;
	uint64_t num_rx_ipa_loop;
	uint64_t num_rx_ipa_tx_dp_err;
	uint64_t num_rx_ipa_write_done;
	uint64_t num_max_ipa_tx_mul;
	uint64_t num_rx_ipa_hw_maxed_out;
	uint64_t max_pend_q_cnt;

	uint64_t num_tx_comp_cnt;

	uint64_t num_freeq_empty;
	uint64_t num_pri_freeq_empty;
};

struct hdd_ipa_priv {
	struct hdd_ipa_sys_pipe sys_pipe[HDD_IPA_MAX_SYSBAM_PIPE];
	struct hdd_ipa_iface_context iface_context[HDD_IPA_MAX_IFACE];
	enum hdd_ipa_rm_state rm_state;
	/*
	 * IPA driver can send RM notifications with IRQ disabled so using adf
	 * APIs as it is taken care gracefully. Without this, kernel would throw
	 * an warning if spin_lock_bh is used while IRQ is disabled
	 */
	adf_os_spinlock_t rm_lock;
	struct work_struct rm_work;
	vos_wake_lock_t wake_lock;
	enum ipa_client_type prod_client;

	atomic_t tx_ref_cnt;
	uint32_t pending_hw_desc_cnt;
	uint32_t hw_desc_cnt;
	spinlock_t q_lock;
	uint32_t freeq_cnt;
	struct list_head free_desc_head;

	uint32_t pend_q_cnt;
	struct list_head pend_desc_head;

	hdd_context_t *hdd_ctx;

	struct dentry *debugfs_dir;
	struct hdd_ipa_stats stats;

	struct notifier_block ipv4_notifier;
	uint32_t curr_prod_bw;
	uint32_t curr_cons_bw;
};

static struct hdd_ipa_priv *ghdd_ipa;

#define HDD_IPA_ENABLE_MASK			BIT(0)
#define HDD_IPA_PRE_FILTER_ENABLE_MASK		BIT(1)
#define HDD_IPA_IPV6_ENABLE_MASK		BIT(2)
#define HDD_IPA_RM_ENABLE_MASK			BIT(3)
#define HDD_IPA_CLK_SCALING_ENABLE_MASK		BIT(4)

#define HDD_IPA_IS_CONFIG_ENABLED(_hdd_ctx, _mask)\
	(((_hdd_ctx)->cfg_ini->IpaConfig & (_mask)) == (_mask))

/* Local Function Prototypes */
static void hdd_ipa_send_pkt_to_ipa(struct hdd_ipa_priv *hdd_ipa);

bool hdd_ipa_is_enabled(hdd_context_t *hdd_ctx)
{
	return HDD_IPA_IS_CONFIG_ENABLED(hdd_ctx, HDD_IPA_ENABLE_MASK);
}

static inline bool hdd_ipa_is_pre_filter_enabled(struct hdd_ipa_priv *hdd_ipa)
{
	hdd_context_t *hdd_ctx = hdd_ipa->hdd_ctx;
	return HDD_IPA_IS_CONFIG_ENABLED(hdd_ctx, HDD_IPA_PRE_FILTER_ENABLE_MASK);
}

static inline bool hdd_ipa_is_ipv6_enabled(struct hdd_ipa_priv *hdd_ipa)
{
	hdd_context_t *hdd_ctx = hdd_ipa->hdd_ctx;
	return HDD_IPA_IS_CONFIG_ENABLED(hdd_ctx, HDD_IPA_IPV6_ENABLE_MASK);
}

static inline bool hdd_ipa_is_rm_enabled(struct hdd_ipa_priv *hdd_ipa)
{
	hdd_context_t *hdd_ctx = hdd_ipa->hdd_ctx;
	return HDD_IPA_IS_CONFIG_ENABLED(hdd_ctx, HDD_IPA_RM_ENABLE_MASK);
}

static inline bool hdd_ipa_is_clk_scaling_enabled(struct hdd_ipa_priv *hdd_ipa)
{
	hdd_context_t *hdd_ctx = hdd_ipa->hdd_ctx;
	return HDD_IPA_IS_CONFIG_ENABLED(hdd_ctx,
			HDD_IPA_CLK_SCALING_ENABLE_MASK |
			HDD_IPA_RM_ENABLE_MASK);
}

static struct ipa_tx_data_desc *hdd_ipa_alloc_data_desc(
		struct hdd_ipa_priv *hdd_ipa, int priority)
{
	struct ipa_tx_data_desc *desc = NULL;

	spin_lock_bh(&hdd_ipa->q_lock);

	/* Keep the descriptors for priority alloc which can be used for
	 * anchor nodes
	 */
	if (hdd_ipa->freeq_cnt < (HDD_IPA_DESC_BUFFER_RATIO * 2) && !priority) {
		hdd_ipa->stats.num_freeq_empty++;
		goto end;
	}

	if (!list_empty(&hdd_ipa->free_desc_head)) {
		desc = list_first_entry(&hdd_ipa->free_desc_head,
				struct ipa_tx_data_desc, link);
		list_del(&desc->link);
		hdd_ipa->freeq_cnt--;
	} else {
		hdd_ipa->stats.num_pri_freeq_empty++;
	}

end:
	spin_unlock_bh(&hdd_ipa->q_lock);

	return desc;
}

static void hdd_ipa_free_data_desc(struct hdd_ipa_priv *hdd_ipa,
		struct ipa_tx_data_desc *desc)
{
	desc->priv = NULL;
	desc->pyld_buffer = NULL;
	desc->pyld_len = 0;
	spin_lock_bh(&hdd_ipa->q_lock);
	list_add_tail(&desc->link, &hdd_ipa->free_desc_head);
	hdd_ipa->freeq_cnt++;
	spin_unlock_bh(&hdd_ipa->q_lock);
}

static struct iphdr * hdd_ipa_get_ip_pkt(void *data, uint16_t *eth_type)
{
	struct ethhdr *eth = (struct ethhdr *)data;
	struct llc_snap_hdr *ls_hdr;
	struct iphdr *ip_hdr;

	ip_hdr = NULL;
	*eth_type = be16_to_cpu(eth->h_proto);
	if (*eth_type < 0x600) {
		/* Non Ethernet II framing format */
		ls_hdr = (struct llc_snap_hdr *)((uint8_t *)data +
						sizeof(struct ethhdr));

		if (((ls_hdr->dsap == 0xAA) && (ls_hdr->ssap == 0xAA)) ||
			((ls_hdr->dsap == 0xAB) && (ls_hdr->ssap == 0xAB)))
			*eth_type = be16_to_cpu(ls_hdr->eth_type);
		ip_hdr = (struct iphdr *)((uint8_t *)data +
				sizeof(struct ethhdr) + sizeof(struct llc_snap_hdr));
	} else if (*eth_type == ETH_P_IP) {
		ip_hdr = (struct iphdr *)((uint8_t *)data +
						sizeof(struct ethhdr));
	}

	return ip_hdr;
}

static bool hdd_ipa_can_send_to_ipa(hdd_adapter_t *adapter, struct hdd_ipa_priv *hdd_ipa, void *data)
{
	uint16_t eth_type;
	struct iphdr *ip_hdr = NULL;

	if (!hdd_ipa_is_pre_filter_enabled(hdd_ipa))
		return true;
	ip_hdr = hdd_ipa_get_ip_pkt(data, &eth_type);

	if (eth_type == ETH_P_IP) {
		//Check if the dest IP address is itself, and in that case, bypass IPA
		if (ip_hdr->daddr != ((struct hdd_ipa_iface_context *)(adapter->ipa_context))->ifa_address)
			return true;
		else
			return false;
	}

	if (hdd_ipa_is_ipv6_enabled(hdd_ipa) && eth_type == ETH_P_IPV6)
		return true;

	return false;
}

static int hdd_ipa_rm_request(struct hdd_ipa_priv *hdd_ipa)
{
	int ret = 0;

	if (!hdd_ipa_is_rm_enabled(hdd_ipa))
		return 0;

	adf_os_spin_lock_bh(&hdd_ipa->rm_lock);

	switch(hdd_ipa->rm_state) {
	case HDD_IPA_RM_GRANTED:
		adf_os_spin_unlock_bh(&hdd_ipa->rm_lock);
		return 0;
	case HDD_IPA_RM_GRANT_PENDING:
		adf_os_spin_unlock_bh(&hdd_ipa->rm_lock);
		return -EINPROGRESS;
	case HDD_IPA_RM_RELEASED:
		hdd_ipa->rm_state = HDD_IPA_RM_GRANT_PENDING;
		break;
	}

	adf_os_spin_unlock_bh(&hdd_ipa->rm_lock);

	ret = ipa_rm_inactivity_timer_request_resource(
			IPA_RM_RESOURCE_WLAN_PROD);

	adf_os_spin_lock_bh(&hdd_ipa->rm_lock);
	if (ret == 0) {
		hdd_ipa->rm_state = HDD_IPA_RM_GRANTED;
		hdd_ipa->stats.num_rm_grant_imm++;
	}
	adf_os_spin_unlock_bh(&hdd_ipa->rm_lock);

	vos_wake_lock_acquire(&hdd_ipa->wake_lock);

	return ret;
}

static int hdd_ipa_rm_try_release(struct hdd_ipa_priv *hdd_ipa)
{
	int ret = 0;

	if (!hdd_ipa_is_rm_enabled(hdd_ipa))
		return 0;

	if (atomic_read(&hdd_ipa->tx_ref_cnt))
		return -EAGAIN;

	spin_lock_bh(&hdd_ipa->q_lock);
	if (hdd_ipa->pending_hw_desc_cnt || hdd_ipa->pend_q_cnt) {
		spin_unlock_bh(&hdd_ipa->q_lock);
		return -EAGAIN;
	}
	spin_unlock_bh(&hdd_ipa->q_lock);

	adf_os_spin_lock_bh(&hdd_ipa->rm_lock);
	switch(hdd_ipa->rm_state) {
	case HDD_IPA_RM_GRANTED:
		break;
	case HDD_IPA_RM_GRANT_PENDING:
		adf_os_spin_unlock_bh(&hdd_ipa->rm_lock);
		return -EINPROGRESS;
	case HDD_IPA_RM_RELEASED:
		adf_os_spin_unlock_bh(&hdd_ipa->rm_lock);
		return 0;
	}

	/* IPA driver returns immediately so set the state here to avoid any
	 * race condition.
	 */
	hdd_ipa->rm_state = HDD_IPA_RM_RELEASED;
	hdd_ipa->stats.num_rm_release++;
	adf_os_spin_unlock_bh(&hdd_ipa->rm_lock);

	ret = ipa_rm_inactivity_timer_release_resource(
			IPA_RM_RESOURCE_WLAN_PROD);

	if (unlikely(ret != 0)) {
		adf_os_spin_lock_bh(&hdd_ipa->rm_lock);
		hdd_ipa->rm_state = HDD_IPA_RM_GRANTED;
		adf_os_spin_unlock_bh(&hdd_ipa->rm_lock);
		WARN_ON(1);
	}

	vos_wake_lock_release(&hdd_ipa->wake_lock);

	return ret;
}

static void hdd_ipa_rm_send_pkt_to_ipa(struct work_struct *work)
{
	struct hdd_ipa_priv *hdd_ipa = container_of(work,
			struct hdd_ipa_priv, rm_work);

	return hdd_ipa_send_pkt_to_ipa(hdd_ipa);
}

static void hdd_ipa_rm_notify(void *user_data, enum ipa_rm_event event,
		unsigned long data)
{
	struct hdd_ipa_priv *hdd_ipa = user_data;

	if (unlikely(!hdd_ipa))
		return;

	if (!hdd_ipa_is_rm_enabled(hdd_ipa))
		return;

	HDD_IPA_LOG(VOS_TRACE_LEVEL_INFO, "Evt: %d", event);

	switch(event) {
	case IPA_RM_RESOURCE_GRANTED:
		adf_os_spin_lock_bh(&hdd_ipa->rm_lock);
		hdd_ipa->rm_state = HDD_IPA_RM_GRANTED;
		adf_os_spin_unlock_bh(&hdd_ipa->rm_lock);
		hdd_ipa->stats.num_rm_grant++;

		schedule_work(&hdd_ipa->rm_work);
		break;
	case IPA_RM_RESOURCE_RELEASED:
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR, "RM Release not expected!");
		break;
	default:
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR, "Unknow RM Evt: %d", event);
		break;
	}
}

static int hdd_ipa_rm_cons_release(void)
{
	return 0;
}

static int hdd_ipa_rm_cons_request(void)
{
	return 0;
}

int hdd_ipa_set_perf_level(hdd_context_t *hdd_ctx, uint64_t tx_packets,
		uint64_t rx_packets)
{
	uint32_t  next_cons_bw, next_prod_bw;
	struct hdd_ipa_priv *hdd_ipa = hdd_ctx->hdd_ipa;
	struct ipa_rm_perf_profile profile;
	int ret;

	if (!hdd_ipa_is_enabled(hdd_ctx) ||
			!hdd_ipa_is_clk_scaling_enabled(hdd_ipa))
		return 0;

	memset(&profile, 0, sizeof(profile));

	if (tx_packets > (hdd_ctx->cfg_ini->busBandwidthHighThreshold / 2))
		next_cons_bw = hdd_ctx->cfg_ini->IpaHighBandwidthMbps;
	else if (tx_packets >
			(hdd_ctx->cfg_ini->busBandwidthMediumThreshold / 2))
		next_cons_bw = hdd_ctx->cfg_ini->IpaMediumBandwidthMbps;
	else
		next_cons_bw = hdd_ctx->cfg_ini->IpaLowBandwidthMbps;

	if (rx_packets > (hdd_ctx->cfg_ini->busBandwidthHighThreshold / 2))
		next_prod_bw = hdd_ctx->cfg_ini->IpaHighBandwidthMbps;
	else if (rx_packets >
			(hdd_ctx->cfg_ini->busBandwidthMediumThreshold / 2))
		next_prod_bw = hdd_ctx->cfg_ini->IpaMediumBandwidthMbps;
	else
		next_prod_bw = hdd_ctx->cfg_ini->IpaLowBandwidthMbps;

	if (hdd_ipa->curr_cons_bw != next_cons_bw) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_INFO,
				"Requesting CONS perf curr: %d, next: %d",
				hdd_ipa->curr_cons_bw, next_cons_bw);
		profile.max_supported_bandwidth_mbps = next_cons_bw;
		ret = ipa_rm_set_perf_profile(IPA_RM_RESOURCE_WLAN_CONS,
				&profile);
		if (ret) {
			HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR,
					"RM CONS set perf profile failed: %d",
					ret);

			return ret;
		}
		hdd_ipa->curr_cons_bw = next_cons_bw;
		hdd_ipa->stats.num_cons_perf_req++;
	}

	if (hdd_ipa->curr_prod_bw != next_prod_bw) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_INFO,
				"Requesting PROD perf curr: %d, next: %d",
				hdd_ipa->curr_prod_bw, next_prod_bw);
		profile.max_supported_bandwidth_mbps = next_prod_bw;
		ret = ipa_rm_set_perf_profile(IPA_RM_RESOURCE_WLAN_PROD,
				&profile);
		if (ret) {
			HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR,
					"RM PROD set perf profile failed: %d",
					ret);
			return ret;
		}
		hdd_ipa->curr_prod_bw = next_prod_bw;
		hdd_ipa->stats.num_prod_perf_req++;
	}

	return 0;
}

static int hdd_ipa_setup_rm(struct hdd_ipa_priv *hdd_ipa)
{
	struct ipa_rm_create_params create_params = {0};
	int ret;

	if (!hdd_ipa_is_rm_enabled(hdd_ipa))
		return 0;

	INIT_WORK(&hdd_ipa->rm_work, hdd_ipa_rm_send_pkt_to_ipa);

	memset(&create_params, 0, sizeof(create_params));
	create_params.name = IPA_RM_RESOURCE_WLAN_PROD;
	create_params.reg_params.user_data = hdd_ipa;
	create_params.reg_params.notify_cb = hdd_ipa_rm_notify;
	create_params.floor_voltage = IPA_VOLTAGE_SVS;

	ret = ipa_rm_create_resource(&create_params);
	if (ret) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR, "Create RM resource failed: %d",
				ret);
		goto setup_rm_fail;
	}

	memset(&create_params, 0, sizeof(create_params));
	create_params.name = IPA_RM_RESOURCE_WLAN_CONS;
	create_params.request_resource= hdd_ipa_rm_cons_request;
	create_params.release_resource= hdd_ipa_rm_cons_release;
	create_params.floor_voltage = IPA_VOLTAGE_SVS;

	ret = ipa_rm_create_resource(&create_params);
	if (ret) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR,
				"Create RM CONS resource failed: %d", ret);
		goto delete_prod;
	}

	ipa_rm_add_dependency(IPA_RM_RESOURCE_WLAN_PROD,
			IPA_RM_RESOURCE_APPS_CONS);

	ret = ipa_rm_inactivity_timer_init(IPA_RM_RESOURCE_WLAN_PROD,
			HDD_IPA_RX_INACTIVITY_MSEC_DELAY);
	if (ret) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR, "Timer init failed: %d",
				ret);
		goto timer_init_failed;
	}

	/* Set the lowest bandwidth to start with */
	ret = hdd_ipa_set_perf_level(hdd_ipa->hdd_ctx, 0, 0);

	if (ret) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR,
				"Set perf level failed: %d", ret);
		goto set_perf_failed;
	}

	vos_wake_lock_init(&hdd_ipa->wake_lock, "wlan_ipa");
	adf_os_spinlock_init(&hdd_ipa->rm_lock);
	hdd_ipa->rm_state = HDD_IPA_RM_RELEASED;
	atomic_set(&hdd_ipa->tx_ref_cnt, 0);

	return ret;

set_perf_failed:
	ipa_rm_inactivity_timer_destroy(IPA_RM_RESOURCE_WLAN_PROD);

timer_init_failed:
	ipa_rm_delete_resource(IPA_RM_RESOURCE_WLAN_CONS);

delete_prod:
	ipa_rm_delete_resource(IPA_RM_RESOURCE_WLAN_PROD);

setup_rm_fail:
	return ret;
}

static void hdd_ipa_destory_rm_resource(struct hdd_ipa_priv *hdd_ipa)
{
	int ret;

	if (!hdd_ipa_is_rm_enabled(hdd_ipa))
		return;

	vos_wake_lock_destroy(&hdd_ipa->wake_lock);

	ipa_rm_inactivity_timer_destroy(IPA_RM_RESOURCE_WLAN_PROD);

	ret = ipa_rm_delete_resource(IPA_RM_RESOURCE_WLAN_PROD);
	if (ret)
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR,
				"RM PROD resource delete failed %d", ret);

	ret = ipa_rm_delete_resource(IPA_RM_RESOURCE_WLAN_CONS);
	if (ret)
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR,
				"RM CONS resource delete failed %d", ret);
}

static void hdd_ipa_send_skb_to_network(adf_nbuf_t skb, hdd_adapter_t *adapter)
{
	if (!adapter || adapter->magic != WLAN_HDD_ADAPTER_MAGIC) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR, "Invalid adapter: 0x%p",
				adapter);

		adf_nbuf_free(skb);
		return;
	}
	skb->dev = adapter->dev;
	skb->protocol = eth_type_trans(skb, skb->dev);
	skb->ip_summed = CHECKSUM_NONE;
	++adapter->hdd_stats.hddTxRxStats.rxPackets;
	if (netif_rx_ni(skb) == NET_RX_SUCCESS)
		++adapter->hdd_stats.hddTxRxStats.rxDelivered;
	else
		++adapter->hdd_stats.hddTxRxStats.rxRefused;
	adapter->dev->last_rx = jiffies;
}

static void hdd_ipa_send_pkt_to_ipa(struct hdd_ipa_priv *hdd_ipa)
{
	struct ipa_tx_data_desc *send_desc, *desc, *tmp;
	uint32_t cur_send_cnt = 0, pend_q_cnt;
	adf_nbuf_t buf;
	struct ipa_tx_data_desc *send_desc_head = NULL;


	/* Make it priority queue request as send descriptor */
	send_desc_head = hdd_ipa_alloc_data_desc(hdd_ipa, 1);

	/* Try again later  when descriptors are available */
	if (!send_desc_head)
		return;

	INIT_LIST_HEAD(&send_desc_head->link);

	spin_lock_bh(&hdd_ipa->q_lock);

	if (hdd_ipa->pending_hw_desc_cnt >= hdd_ipa->hw_desc_cnt) {
		hdd_ipa->stats.num_rx_ipa_hw_maxed_out++;
		spin_unlock_bh(&hdd_ipa->q_lock);
		hdd_ipa_free_data_desc(hdd_ipa, send_desc_head);
		return;
	}

	pend_q_cnt = hdd_ipa->pend_q_cnt;

	if (pend_q_cnt == 0) {
		spin_unlock_bh(&hdd_ipa->q_lock);
		hdd_ipa_free_data_desc(hdd_ipa, send_desc_head);
		return;
	}

	/* If hardware has more room than what is pending in the queue update
	 * the send_desc_head right away without going through the loop
	 */
	if ((hdd_ipa->pending_hw_desc_cnt + pend_q_cnt) <
			hdd_ipa->hw_desc_cnt) {
		list_splice_tail_init(&hdd_ipa->pend_desc_head,
				&send_desc_head->link);
		cur_send_cnt = pend_q_cnt;
		hdd_ipa->pend_q_cnt = 0;
		hdd_ipa->stats.num_rx_ipa_splice++;
	} else {
		while (((hdd_ipa->pending_hw_desc_cnt + cur_send_cnt) <
					hdd_ipa->hw_desc_cnt) && pend_q_cnt > 0)
		{
			send_desc = list_first_entry(&hdd_ipa->pend_desc_head,
					struct ipa_tx_data_desc, link);
			list_del(&send_desc->link);
			list_add_tail(&send_desc->link, &send_desc_head->link);
			cur_send_cnt++;
			pend_q_cnt--;
		}
		hdd_ipa->stats.num_rx_ipa_loop++;

		hdd_ipa->pend_q_cnt -= cur_send_cnt;

		VOS_ASSERT(hdd_ipa->pend_q_cnt == pend_q_cnt);
	}

	hdd_ipa->pending_hw_desc_cnt += cur_send_cnt;
	spin_unlock_bh(&hdd_ipa->q_lock);

	if (ipa_tx_dp_mul(hdd_ipa->prod_client, send_desc_head) != 0) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR,
				"ipa_tx_dp_mul failed: %u, q_cnt: %u!",
				hdd_ipa->pending_hw_desc_cnt,
				hdd_ipa->pend_q_cnt);
		goto ipa_tx_failed;
	}

	hdd_ipa->stats.num_rx_ipa_tx_dp += cur_send_cnt;
	if (cur_send_cnt > hdd_ipa->stats.num_max_ipa_tx_mul)
		hdd_ipa->stats.num_max_ipa_tx_mul = cur_send_cnt;

	return;

ipa_tx_failed:

	spin_lock_bh(&hdd_ipa->q_lock);
	hdd_ipa->pending_hw_desc_cnt -= cur_send_cnt;
	spin_unlock_bh(&hdd_ipa->q_lock);

	list_for_each_entry_safe(desc, tmp, &send_desc_head->link, link) {
		list_del(&desc->link);
		buf = desc->priv;
		adf_nbuf_free(buf);
		hdd_ipa_free_data_desc(hdd_ipa, desc);
		hdd_ipa->stats.num_rx_ipa_tx_dp_err++;
	}

	/* Return anchor node */
	hdd_ipa_free_data_desc(hdd_ipa, send_desc_head);
}

VOS_STATUS hdd_ipa_process_rxt(v_VOID_t *vosContext, adf_nbuf_t rx_buf_list,
		v_U8_t sta_id)
{
	struct hdd_ipa_priv *hdd_ipa = ghdd_ipa;
	hdd_adapter_t *adapter = NULL;
	struct hdd_ipa_iface_context *iface_context = NULL;
	adf_nbuf_t buf, next_buf;
	uint8_t cur_cnt = 0;
	struct hdd_ipa_cld_hdr *cld_hdr;
	struct ipa_tx_data_desc *send_desc = NULL;

	if (!hdd_ipa_is_enabled(hdd_ipa->hdd_ctx))
		return VOS_STATUS_E_INVAL;

	adapter = hdd_ipa->hdd_ctx->sta_to_adapter[sta_id];
	if (!adapter || !adapter->ipa_context ||
			adapter->magic != WLAN_HDD_ADAPTER_MAGIC) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR, "Invalid sta_id: %d",
				sta_id);
		goto drop_pkts;
	}

	iface_context = (struct hdd_ipa_iface_context *) adapter->ipa_context;

	buf = rx_buf_list;
	while (buf) {
		HDD_IPA_DBG_DUMP(VOS_TRACE_LEVEL_DEBUG, "RX data",
				buf->data, 24);

		next_buf = adf_nbuf_queue_next(buf);
		adf_nbuf_set_next(buf, NULL);

		adapter->stats.rx_packets++;
		adapter->stats.rx_bytes += buf->len;
		/*
		 * we want to send Rx packets to IPA only when it is
		 * IPV4 or IPV6 (if IPV6 is enabled). All other packets
		 * will be sent to network stack directly.
		 */
		if (!hdd_ipa_can_send_to_ipa(adapter, hdd_ipa, buf->data)) {
			iface_context->stats.num_rx_prefilter++;
			hdd_ipa_send_skb_to_network(buf, adapter);
			buf = next_buf;
			continue;
		}

		cld_hdr = (struct hdd_ipa_cld_hdr *) skb_push(buf,
				HDD_IPA_WLAN_CLD_HDR_LEN);
		cld_hdr->sta_id = sta_id;
		cld_hdr->iface_id = iface_context->iface_id;

		send_desc = hdd_ipa_alloc_data_desc(hdd_ipa, 0);
		if (!send_desc) {
			adf_nbuf_free(buf); /*No desc available; drop*/
			buf = next_buf;
			iface_context->stats.num_rx_send_desc_err++;
			continue;
		}

		send_desc->priv = buf;
		send_desc->pyld_buffer = buf->data;
		send_desc->pyld_len = buf->len;
		spin_lock_bh(&hdd_ipa->q_lock);
		list_add_tail(&send_desc->link, &hdd_ipa->pend_desc_head);
		hdd_ipa->pend_q_cnt++;
		spin_unlock_bh(&hdd_ipa->q_lock);
		cur_cnt++;
		buf = next_buf;
	}

	iface_context->stats.num_rx_recv += cur_cnt;
	if (cur_cnt > 1)
		iface_context->stats.num_rx_recv_mul++;

	if (cur_cnt > iface_context->stats.max_rx_mul)
		iface_context->stats.max_rx_mul = cur_cnt;

	if (hdd_ipa->pend_q_cnt > hdd_ipa->stats.max_pend_q_cnt)
		hdd_ipa->stats.max_pend_q_cnt = hdd_ipa->pend_q_cnt;

	if (cur_cnt && hdd_ipa_rm_request(hdd_ipa) == 0) {
		hdd_ipa_send_pkt_to_ipa(hdd_ipa);
	}

	return VOS_STATUS_SUCCESS;

drop_pkts:
	buf = rx_buf_list;
	while (buf) {
		next_buf = adf_nbuf_queue_next(buf);
		adf_nbuf_free(buf);
		buf = next_buf;
		hdd_ipa->stats.num_rx_drop++;
		if (adapter)
			adapter->stats.rx_dropped++;
	}

	return VOS_STATUS_E_FAILURE;
}

static void hdd_ipa_set_adapter_ip_filter(hdd_adapter_t *adapter)
{
	struct in_ifaddr **ifap = NULL;
	struct in_ifaddr *ifa = NULL;
	struct in_device *in_dev;
	struct net_device *dev;
	struct hdd_ipa_iface_context *iface_context;

	iface_context = (struct hdd_ipa_iface_context *)adapter->ipa_context;
	dev = adapter->dev;
	if (!dev || !iface_context) {
		return;
	}
	/* This optimization not needed for Station mode one of
	 * the reason being sta-usb tethered mode
	 */
	if (adapter->device_mode == WLAN_HDD_INFRA_STATION) {
		iface_context->ifa_address = 0;
		return;
	}


	/* Get IP address */
	if (dev->priv_flags & IFF_BRIDGE_PORT) {
#ifdef WLAN_OPEN_SOURCE
		rcu_read_lock();
#endif
		dev = netdev_master_upper_dev_get_rcu(adapter->dev);
#ifdef WLAN_OPEN_SOURCE
		rcu_read_unlock();
#endif
		if (!dev)
			return;
	}
	if ((in_dev = __in_dev_get_rtnl(dev)) != NULL) {
	   for (ifap = &in_dev->ifa_list; (ifa = *ifap) != NULL;
		   ifap = &ifa->ifa_next) {
			if (dev->name && !strcmp(dev->name, ifa->ifa_label))
				break; /* found */
		}
	}
	if(ifa && ifa->ifa_address) {
		iface_context->ifa_address = ifa->ifa_address;

		HDD_IPA_LOG(VOS_TRACE_LEVEL_INFO,"%s: %d.%d.%d.%d", dev->name,
		iface_context->ifa_address & 0x000000ff,
		iface_context->ifa_address >> 8 & 0x000000ff,
		iface_context->ifa_address >> 16 & 0x000000ff,
		iface_context->ifa_address >> 24 & 0x000000ff);
	}
}

static int hdd_ipa_ipv4_changed(struct notifier_block *nb,
		unsigned long data, void *arg)
{
	struct hdd_ipa_priv *hdd_ipa = ghdd_ipa;
	hdd_adapter_list_node_t *padapter_node = NULL, *pnext = NULL;
	hdd_adapter_t *padapter;
	VOS_STATUS status;
	HDD_IPA_LOG(VOS_TRACE_LEVEL_INFO,
		"IPv4 Change detected. Updating wlan IPv4 local filters");

	status = hdd_get_front_adapter(hdd_ipa->hdd_ctx, &padapter_node);
	while (padapter_node && VOS_STATUS_SUCCESS == status) {
		padapter = padapter_node->pAdapter;
		if (padapter)
			hdd_ipa_set_adapter_ip_filter(padapter);

		status = hdd_get_next_adapter(hdd_ipa->hdd_ctx, padapter_node, &pnext);
		padapter_node = pnext;
	}
	return 0;
}

static void hdd_ipa_w2i_cb(void *priv, enum ipa_dp_evt_type evt,
		unsigned long data)
{
	struct hdd_ipa_priv *hdd_ipa = NULL;
	hdd_adapter_t *adapter = NULL;
	struct ipa_tx_data_desc *done_desc_head, *done_desc, *tmp;
	adf_nbuf_t skb;
	uint8_t iface_id;
	adf_nbuf_t buf;

	hdd_ipa = (struct hdd_ipa_priv *)priv;

	switch (evt) {
	case IPA_RECEIVE:
		skb = (adf_nbuf_t) data;

		iface_id = HDD_IPA_GET_IFACE_ID(skb->data);
		if (iface_id >= HDD_IPA_MAX_IFACE) {
			HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR,
					"IPA_RECEIVE: Invalid iface_id: %u",
					iface_id);
			adf_nbuf_free(skb);
			return;
		}

		adapter = hdd_ipa->iface_context[iface_id].adapter;

		HDD_IPA_DBG_DUMP(VOS_TRACE_LEVEL_DEBUG, "w2i -- skb", skb->data,
				8);

		skb_pull(skb, HDD_IPA_WLAN_CLD_HDR_LEN);

		hdd_ipa->iface_context[iface_id].stats.num_rx_ipa_excep++;
		hdd_ipa_send_skb_to_network(skb, adapter);
		break;
	case IPA_WRITE_DONE:
		done_desc_head = (struct ipa_tx_data_desc *)data;
		list_for_each_entry_safe(done_desc, tmp,
				&done_desc_head->link, link) {
			list_del(&done_desc->link);
			buf = done_desc->priv;
			adf_nbuf_free(buf);
			hdd_ipa_free_data_desc(hdd_ipa, done_desc);
			spin_lock_bh(&hdd_ipa->q_lock);
			hdd_ipa->pending_hw_desc_cnt--;
			spin_unlock_bh(&hdd_ipa->q_lock);
			hdd_ipa->stats.num_rx_ipa_write_done++;
		}
		/* add anchor node also back to free list */
		hdd_ipa_free_data_desc(hdd_ipa, done_desc_head);

		hdd_ipa_send_pkt_to_ipa(hdd_ipa);

		hdd_ipa_rm_try_release(hdd_ipa);
		break;
	default:
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR,
				"w2i cb wrong event: 0x%x", evt);
		return;
	}
}

static void hdd_ipa_nbuf_cb(adf_nbuf_t skb)
{
	struct hdd_ipa_priv *hdd_ipa = ghdd_ipa;

	HDD_IPA_LOG(VOS_TRACE_LEVEL_DEBUG, "%lx", NBUF_OWNER_PRIV_DATA(skb));
	ipa_free_skb((struct ipa_rx_data *) NBUF_OWNER_PRIV_DATA(skb));

	hdd_ipa->stats.num_tx_comp_cnt++;

	atomic_dec(&hdd_ipa->tx_ref_cnt);

	hdd_ipa_rm_try_release(hdd_ipa);
}

static void hdd_ipa_i2w_cb(void *priv, enum ipa_dp_evt_type evt,
		unsigned long data)
{
	struct hdd_ipa_priv *hdd_ipa = NULL;
	struct ipa_rx_data *ipa_tx_desc;
	struct hdd_ipa_iface_context *iface_context;
	adf_nbuf_t skb;
	v_U8_t interface_id;
	hdd_adapter_t  *adapter = NULL;

	if (evt != IPA_RECEIVE) {
		skb = (adf_nbuf_t) data;
		dev_kfree_skb_any(skb);
		iface_context->stats.num_tx_drop++;
		return;
	}

	iface_context = (struct hdd_ipa_iface_context *) priv;
	ipa_tx_desc = (struct ipa_rx_data *)data;
	skb = ipa_tx_desc->skb;

	hdd_ipa = iface_context->hdd_ipa;

	/*
	 * When SSR is going on, just drop the packets. There is no use in
	 * queueing the packets as STA has to connect back any way
	 */
	if (hdd_ipa->hdd_ctx->isLogpInProgress) {
		ipa_free_skb(ipa_tx_desc);
		iface_context->stats.num_tx_drop++;
		return;
	}

	adf_os_mem_set(skb->cb, 0, sizeof(skb->cb));
	NBUF_OWNER_ID(skb) = IPA_NBUF_OWNER_ID;
	NBUF_CALLBACK_FN(skb) = hdd_ipa_nbuf_cb;
	NBUF_MAPPED_PADDR_LO(skb) = ipa_tx_desc->dma_addr;

	NBUF_OWNER_PRIV_DATA(skb) = data;

	HDD_IPA_DBG_DUMP(VOS_TRACE_LEVEL_DEBUG, "i2w", skb->data, 8);

	adf_os_spin_lock_bh(&iface_context->interface_lock);
	adapter = iface_context->adapter;
	if (!adapter) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_WARN, "Interface Down");
		ipa_free_skb(ipa_tx_desc);
		iface_context->stats.num_tx_drop++;
		adf_os_spin_unlock_bh(&iface_context->interface_lock);
		return;
	}

	/*
	 * During CAC period, data packets shouldn't be sent over the air so
	 * drop all the packets here
	 */
	if (WLAN_HDD_GET_AP_CTX_PTR(adapter)->dfs_cac_block_tx) {
		ipa_free_skb(ipa_tx_desc);
		adf_os_spin_unlock_bh(&iface_context->interface_lock);
		iface_context->stats.num_tx_cac_drop++;
		return;
	}

	interface_id = adapter->sessionId;
	++adapter->stats.tx_packets;
	adapter->stats.tx_bytes += ipa_tx_desc->skb->len;

	adf_os_spin_unlock_bh(&iface_context->interface_lock);

	skb = WLANTL_SendIPA_DataFrame(hdd_ipa->hdd_ctx->pvosContext,
			iface_context->tl_context, ipa_tx_desc->skb,
			interface_id);
	if (skb) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_DEBUG, "TLSHIM tx fail");
		ipa_free_skb(ipa_tx_desc);
		iface_context->stats.num_tx_err++;
		return;
	}

	atomic_inc(&hdd_ipa->tx_ref_cnt);

	/*
	 * If PROD resource is not requested here then there may be cases where
	 * IPA hardware may be clocked down because of not having proper
	 * dependency graph between WLAN CONS and modem PROD pipes. Adding the
	 * workaround to request PROD resource while data is going over CONS
	 * pipe to prevent the IPA hardware clockdown.
	 */
	hdd_ipa_rm_request(hdd_ipa);

	iface_context->stats.num_tx++;
}

static int hdd_ipa_setup_sys_pipe(struct hdd_ipa_priv *hdd_ipa)
{
	int i, ret = 0;
	struct ipa_sys_connect_params *ipa;
	uint32_t desc_fifo_sz;

	/* The maximum number of descriptors that can be provided to a BAM at
	 * once is one less than the total number of descriptors that the buffer
	 * can contain.
	 * If max_num_of_descriptors = (BAM_PIPE_DESCRIPTOR_FIFO_SIZE / sizeof
	 * (SPS_DESCRIPTOR)), then (max_num_of_descriptors - 1) descriptors can
	 * be provided at once.
	 * Because of above requirement, one extra descriptor will be added to
	 * make sure hardware always has one descriptor.
	 */
	desc_fifo_sz = hdd_ipa->hdd_ctx->cfg_ini->IpaDescSize
		+ sizeof(struct sps_iovec);

	/*setup TX pipes */
	for (i = 0; i < HDD_IPA_MAX_IFACE; i++) {
		ipa = &hdd_ipa->sys_pipe[i].ipa_sys_params;

		ipa->client = hdd_ipa_adapter_2_client[i].cons_client;
		ipa->desc_fifo_sz = desc_fifo_sz;
		ipa->priv = &hdd_ipa->iface_context[i];
		ipa->notify = hdd_ipa_i2w_cb;

		ipa->ipa_ep_cfg.hdr.hdr_len = HDD_IPA_WLAN_TX_HDR_LEN;
		ipa->ipa_ep_cfg.mode.mode = IPA_BASIC;

		if (!hdd_ipa_is_rm_enabled(hdd_ipa))
			ipa->keep_ipa_awake = 1;

		ret = ipa_setup_sys_pipe(ipa, &(hdd_ipa->sys_pipe[i].conn_hdl));
		if (ret) {
			HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR, "Failed for pipe %d"
					" ret: %d", i, ret);
			goto setup_sys_pipe_fail;
		}
		hdd_ipa->sys_pipe[i].conn_hdl_valid = 1;
	}

	/*
	 * Hard code it here, this can be extended if in case PROD pipe is also
	 * per interface. Right now there is no advantage of doing this.
	 */
	hdd_ipa->prod_client = IPA_CLIENT_WLAN1_PROD;

	ipa = &hdd_ipa->sys_pipe[HDD_IPA_RX_PIPE].ipa_sys_params;

	ipa->client = hdd_ipa->prod_client;

	ipa->desc_fifo_sz = desc_fifo_sz;
	ipa->priv = hdd_ipa;
	ipa->notify = hdd_ipa_w2i_cb;

	ipa->ipa_ep_cfg.nat.nat_en = IPA_BYPASS_NAT;
	ipa->ipa_ep_cfg.hdr.hdr_len = HDD_IPA_WLAN_RX_HDR_LEN;
	ipa->ipa_ep_cfg.hdr.hdr_ofst_metadata_valid = 1;
	ipa->ipa_ep_cfg.mode.mode = IPA_BASIC;

	if (!hdd_ipa_is_rm_enabled(hdd_ipa))
		ipa->keep_ipa_awake = 1;

	ret = ipa_setup_sys_pipe(ipa, &(hdd_ipa->sys_pipe[i].conn_hdl));
	if (ret) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR, "Failed for RX pipe: %d",
				ret);
		goto setup_sys_pipe_fail;
	}
	hdd_ipa->sys_pipe[HDD_IPA_RX_PIPE].conn_hdl_valid = 1;

	return ret;

setup_sys_pipe_fail:

	while (--i >= 0) {
		ipa_teardown_sys_pipe(hdd_ipa->sys_pipe[i].conn_hdl);
		adf_os_mem_zero(&hdd_ipa->sys_pipe[i],
				sizeof(struct hdd_ipa_sys_pipe ));
	}

	return ret;
}

/* Disconnect all the Sys pipes */
static void hdd_ipa_teardown_sys_pipe(struct hdd_ipa_priv *hdd_ipa)
{
	int ret = 0, i;
	for (i = 0; i < HDD_IPA_MAX_SYSBAM_PIPE; i++) {
		if (hdd_ipa->sys_pipe[i].conn_hdl_valid) {
			ret = ipa_teardown_sys_pipe(
						hdd_ipa->sys_pipe[i].conn_hdl);
			if (ret)
				HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR, "Failed: %d",
						ret);

			hdd_ipa->sys_pipe[i].conn_hdl_valid = 0;
		}
	}
}

static int hdd_ipa_register_interface(struct hdd_ipa_priv *hdd_ipa,
		struct hdd_ipa_iface_context *iface_context)
{
	struct ipa_tx_intf tx_intf;
	struct ipa_rx_intf rx_intf;
	struct ipa_ioc_tx_intf_prop *tx_prop = NULL;
	struct ipa_ioc_rx_intf_prop *rx_prop = NULL;
	char *ifname = iface_context->adapter->dev->name;

	char ipv4_hdr_name[IPA_RESOURCE_NAME_MAX];
	char ipv6_hdr_name[IPA_RESOURCE_NAME_MAX];

	int num_prop = 1;
	int ret = 0;

	if (hdd_ipa_is_ipv6_enabled(hdd_ipa))
		num_prop++;

	/* Allocate TX properties for TOS categories, 1 each for IPv4 & IPv6 */
	tx_prop = adf_os_mem_alloc(NULL,
			sizeof(struct ipa_ioc_tx_intf_prop) * num_prop);
	if (!tx_prop) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR, "tx_prop allocation failed");
		goto register_interface_fail;
	}

	/* Allocate RX properties, 1 each for IPv4 & IPv6 */
	rx_prop = adf_os_mem_alloc(NULL,
			sizeof(struct ipa_ioc_rx_intf_prop) * num_prop);
	if (!rx_prop) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR, "rx_prop allocation failed");
		goto register_interface_fail;
	}

	adf_os_mem_zero(&tx_intf, sizeof(tx_intf));
	adf_os_mem_zero(&rx_intf, sizeof(rx_intf));

	snprintf(ipv4_hdr_name, IPA_RESOURCE_NAME_MAX, "%s%s",
		ifname, HDD_IPA_IPV4_NAME_EXT);
	snprintf(ipv6_hdr_name, IPA_RESOURCE_NAME_MAX, "%s%s",
		ifname, HDD_IPA_IPV6_NAME_EXT);

	rx_prop[IPA_IP_v4].ip = IPA_IP_v4;
	rx_prop[IPA_IP_v4].src_pipe = iface_context->prod_client;

	rx_prop[IPA_IP_v4].attrib.attrib_mask = IPA_FLT_META_DATA;

	/*
	 * Interface ID is 3rd byte in the CLD header. Add the meta data and
	 * mask to identify the interface in IPA hardware
	 */
	rx_prop[IPA_IP_v4].attrib.meta_data =
		htonl(iface_context->iface_id << 16);
	rx_prop[IPA_IP_v4].attrib.meta_data_mask = htonl(0x00FF0000);

	rx_intf.num_props++;
	if (hdd_ipa_is_ipv6_enabled(hdd_ipa)) {
		rx_prop[IPA_IP_v6].ip = IPA_IP_v6;
		rx_prop[IPA_IP_v6].src_pipe = iface_context->prod_client;

		rx_prop[IPA_IP_v4].attrib.attrib_mask = IPA_FLT_META_DATA;
		rx_prop[IPA_IP_v4].attrib.meta_data =
			htonl(iface_context->iface_id << 16);
		rx_prop[IPA_IP_v4].attrib.meta_data_mask = htonl(0x00FF0000);

		rx_intf.num_props++;
	}

	tx_prop[IPA_IP_v4].ip = IPA_IP_v4;
	tx_prop[IPA_IP_v4].dst_pipe = iface_context->cons_client;
	strlcpy(tx_prop[IPA_IP_v4].hdr_name, ipv4_hdr_name,
			IPA_RESOURCE_NAME_MAX);
	tx_intf.num_props++;

	if (hdd_ipa_is_ipv6_enabled(hdd_ipa)) {
		tx_prop[IPA_IP_v6].ip = IPA_IP_v6;
		tx_prop[IPA_IP_v6].dst_pipe = iface_context->cons_client;
		strlcpy(tx_prop[IPA_IP_v6].hdr_name, ipv6_hdr_name,
				IPA_RESOURCE_NAME_MAX);
		tx_intf.num_props++;
	}

	tx_intf.prop = tx_prop;
	rx_intf.prop = rx_prop;

	/* Call the ipa api to register interface */
	ret = ipa_register_intf(ifname, &tx_intf, &rx_intf);

register_interface_fail:
	adf_os_mem_free(tx_prop);
	adf_os_mem_free(rx_prop);
	return ret;
}

static void hdd_remove_ipa_header(char *name)
{
	struct ipa_ioc_get_hdr hdrlookup;
	int ret = 0, len;
	struct ipa_ioc_del_hdr *ipa_hdr;

	adf_os_mem_zero(&hdrlookup, sizeof(hdrlookup));
	strlcpy(hdrlookup.name, name, sizeof(hdrlookup.name));
	ret = ipa_get_hdr(&hdrlookup);
	if (ret) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_INFO, "Hdr deleted already %s, %d",
				name, ret);
		return;
	}


	HDD_IPA_LOG(VOS_TRACE_LEVEL_INFO, "hdl: 0x%x", hdrlookup.hdl);
	len = sizeof(struct ipa_ioc_del_hdr) + sizeof(struct ipa_hdr_del)*1;
	ipa_hdr = (struct ipa_ioc_del_hdr *) adf_os_mem_alloc(NULL, len);
	if (ipa_hdr == NULL) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR, "ipa_hdr allocation failed");
		return;
	}
	ipa_hdr->num_hdls = 1;
	ipa_hdr->commit = 0;
	ipa_hdr->hdl[0].hdl = hdrlookup.hdl;
	ipa_hdr->hdl[0].status = -1;
	ret = ipa_del_hdr(ipa_hdr);
	if (ret != 0)
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR, "Delete header failed: %d",
				ret);

	adf_os_mem_free(ipa_hdr);
}


static int hdd_ipa_add_header_info(struct hdd_ipa_priv *hdd_ipa,
		struct hdd_ipa_iface_context *iface_context, uint8_t *mac_addr)
{
	hdd_adapter_t *adapter = iface_context->adapter;
	char *ifname;
	struct ipa_ioc_add_hdr *ipa_hdr = NULL;
	int ret = -EINVAL;
	struct hdd_ipa_tx_hdr *tx_hdr = NULL;

	ifname = adapter->dev->name;


	HDD_IPA_LOG(VOS_TRACE_LEVEL_INFO, "Add Partial hdr: %s, %pM",
			ifname, mac_addr);

	/* dynamically allocate the memory to add the hdrs */
	ipa_hdr = adf_os_mem_alloc(NULL, sizeof(struct ipa_ioc_add_hdr)
			+ sizeof(struct ipa_hdr_add));
	if (!ipa_hdr) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR,
				"%s: ipa_hdr allocation failed", ifname);
		ret = -ENOMEM;
		goto end;
	}

	ipa_hdr->commit = 0;
	ipa_hdr->num_hdrs = 1;

	tx_hdr = (struct hdd_ipa_tx_hdr *)ipa_hdr->hdr[0].hdr;

	/* Set the Source MAC */
	memcpy(tx_hdr, &ipa_tx_hdr, HDD_IPA_WLAN_TX_HDR_LEN);
	memcpy(tx_hdr->eth.h_source, mac_addr, ETH_ALEN);

	snprintf(ipa_hdr->hdr[0].name, IPA_RESOURCE_NAME_MAX, "%s%s",
			ifname, HDD_IPA_IPV4_NAME_EXT);
	ipa_hdr->hdr[0].hdr_len = HDD_IPA_WLAN_TX_HDR_LEN;
	ipa_hdr->hdr[0].is_partial = 1;
	ipa_hdr->hdr[0].hdr_hdl = 0;

	/* Set the type to IPV4 in the header*/
	tx_hdr->llc_snap.eth_type = cpu_to_be16(ETH_P_IP);

	ret = ipa_add_hdr(ipa_hdr);

	if (ret) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR, "%s IPv4 add hdr failed: %d",
				ifname, ret);
		goto end;
	}

	HDD_IPA_LOG(VOS_TRACE_LEVEL_INFO, "%s: IPv4 hdr_hdl: 0x%x",
			ipa_hdr->hdr[0].name, ipa_hdr->hdr[0].hdr_hdl);

	if (hdd_ipa_is_ipv6_enabled(hdd_ipa)) {
		snprintf(ipa_hdr->hdr[0].name, IPA_RESOURCE_NAME_MAX, "%s%s",
				ifname, HDD_IPA_IPV6_NAME_EXT);

		/* Set the type to IPV6 in the header*/
		tx_hdr->llc_snap.eth_type = cpu_to_be16(ETH_P_IPV6);

		ret = ipa_add_hdr(ipa_hdr);

		if (ret) {
			HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR,
					"%s: IPv6 add hdr failed: %d",
					ifname, ret);
			goto clean_ipv4_hdr;
		}

		HDD_IPA_LOG(VOS_TRACE_LEVEL_INFO, "%s: IPv6 hdr_hdl: 0x%x",
				ipa_hdr->hdr[0].name, ipa_hdr->hdr[0].hdr_hdl);
	}

	adf_os_mem_free(ipa_hdr);

	return ret;

clean_ipv4_hdr:
	snprintf(ipa_hdr->hdr[0].name, IPA_RESOURCE_NAME_MAX, "%s%s",
			ifname, HDD_IPA_IPV4_NAME_EXT);
	hdd_remove_ipa_header(ipa_hdr->hdr[0].name);
end:
	if(ipa_hdr)
		adf_os_mem_free(ipa_hdr);

	return ret;
}

static void hdd_ipa_clean_hdr(hdd_adapter_t *adapter)
{
	struct hdd_ipa_priv *hdd_ipa = ghdd_ipa;
	int ret;
	char name_ipa[IPA_RESOURCE_NAME_MAX];

	/* Remove the headers */
	snprintf(name_ipa, IPA_RESOURCE_NAME_MAX, "%s%s",
		adapter->dev->name, HDD_IPA_IPV4_NAME_EXT);
	hdd_remove_ipa_header(name_ipa);

	if (hdd_ipa_is_ipv6_enabled(hdd_ipa)) {
		snprintf(name_ipa, IPA_RESOURCE_NAME_MAX, "%s%s",
			adapter->dev->name, HDD_IPA_IPV6_NAME_EXT);
		hdd_remove_ipa_header(name_ipa);
	}
	/* unregister the interface with IPA */
	ret = ipa_deregister_intf(adapter->dev->name);
	if (ret)
		HDD_IPA_LOG(VOS_TRACE_LEVEL_INFO,
				"%s: ipa_deregister_intf fail: %d",
				adapter->dev->name, ret);
}

static void hdd_ipa_cleanup_iface(struct hdd_ipa_iface_context *iface_context)
{
	if (iface_context == NULL)
		return;

	hdd_ipa_clean_hdr(iface_context->adapter);

	adf_os_spin_lock_bh(&iface_context->interface_lock);
	iface_context->adapter->ipa_context = NULL;
	iface_context->adapter = NULL;
	iface_context->tl_context = NULL;
	adf_os_spin_unlock_bh(&iface_context->interface_lock);
	iface_context->ifa_address = 0;
}


static int hdd_ipa_setup_iface(struct hdd_ipa_priv *hdd_ipa,
		hdd_adapter_t *adapter, uint8_t sta_id)
{
	struct hdd_ipa_iface_context *iface_context = NULL;
	void *tl_context = NULL;
	int i, ret = 0;

	/* Lower layer may send multiple START_BSS_EVENT in DFS mode or during
	 * channel change indication. Since these indications are sent by lower
	 * layer as SAP updates and IPA doesn't have to do anything for these
	 * updates so ignoring!
	 */
	if (WLAN_HDD_SOFTAP == adapter->device_mode && adapter->ipa_context)
		return 0;

	for (i = 0; i < HDD_IPA_MAX_IFACE; i++) {
		if (hdd_ipa->iface_context[i].adapter == NULL) {
			iface_context = &(hdd_ipa->iface_context[i]);
			break;
		}
	}

	if (iface_context == NULL) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR,
				"All the IPA interfaces are in use");
		ret = -ENOMEM;
		goto end;
	}


	adapter->ipa_context = iface_context;
	iface_context->adapter = adapter;
	iface_context->sta_id = sta_id;
	tl_context = tl_shim_get_vdev_by_sta_id(hdd_ipa->hdd_ctx->pvosContext,
			sta_id);

	if (tl_context == NULL) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR,
				"Not able to get TL context sta_id: %d",
				sta_id);
		ret = -EINVAL;
		goto end;
	}

	iface_context->tl_context = tl_context;

	ret = hdd_ipa_add_header_info(hdd_ipa, iface_context,
			adapter->dev->dev_addr);

	if (ret)
		goto end;

	/* Configure the TX and RX pipes filter rules */
	ret = hdd_ipa_register_interface(hdd_ipa, iface_context);
	if (ret)
		goto cleanup_header;

	return ret;

cleanup_header:

	hdd_ipa_clean_hdr(adapter);
end:
	if (iface_context)
		hdd_ipa_cleanup_iface(iface_context);
	return ret;
}


static void hdd_ipa_msg_free_fn(void *buff, uint32_t len, uint32_t type)
{
	HDD_IPA_LOG(VOS_TRACE_LEVEL_INFO, "msg type:%d, len:%d", type, len);
	ghdd_ipa->stats.num_free_msg++;
	adf_os_mem_free(buff);
}

static inline char *hdd_ipa_wlan_event_to_str(enum ipa_wlan_event event)
{
	switch(event) {
	case WLAN_CLIENT_CONNECT:  return "WLAN_CLIENT_CONNECT";
	case WLAN_CLIENT_DISCONNECT: return "WLAN_CLIENT_DISCONNECT";
	case WLAN_CLIENT_POWER_SAVE_MODE: return "WLAN_CLIENT_POWER_SAVE_MODE";
	case WLAN_CLIENT_NORMAL_MODE: return "WLAN_CLIENT_NORMAL_MODE";
	case SW_ROUTING_ENABLE: return "SW_ROUTING_ENABLE";
	case SW_ROUTING_DISABLE: return "SW_ROUTING_DISABLE";
	case WLAN_AP_CONNECT: return "WLAN_AP_CONNECT";
	case WLAN_AP_DISCONNECT: return "WLAN_AP_DISCONNECT";
	case WLAN_STA_CONNECT: return "WLAN_STA_CONNECT";
	case WLAN_STA_DISCONNECT: return "WLAN_STA_DISCONNECT";
	case WLAN_CLIENT_CONNECT_EX: return "WLAN_CLIENT_CONNECT_EX";

	case IPA_WLAN_EVENT_MAX:
	default:
	return "UNKNOWN";
	}
}

int hdd_ipa_wlan_evt(hdd_adapter_t *adapter, uint8_t sta_id,
			enum ipa_wlan_event type, uint8_t *mac_addr)
{
	struct hdd_ipa_priv *hdd_ipa = ghdd_ipa;
	struct ipa_msg_meta meta;
	struct ipa_wlan_msg *msg;
	struct ipa_wlan_msg_ex *msg_ex = NULL;
	int ret;

	HDD_IPA_LOG(VOS_TRACE_LEVEL_INFO, "%s: %s evt, MAC: %pM sta_id: %d",
			adapter->dev->name, hdd_ipa_wlan_event_to_str(type), mac_addr,
			sta_id);

	if (type >= IPA_WLAN_EVENT_MAX)
		return -EINVAL;

	if (WARN_ON(is_zero_ether_addr(mac_addr)))
		return -EINVAL;

	hdd_ipa->stats.event[type]++;

	switch (type) {
	case WLAN_STA_CONNECT:
	case WLAN_AP_CONNECT:
		ret = hdd_ipa_setup_iface(hdd_ipa, adapter, sta_id);
		if (ret)
			goto end;
		break;

	case WLAN_STA_DISCONNECT:
	case WLAN_AP_DISCONNECT:
		hdd_ipa_cleanup_iface(adapter->ipa_context);
		break;

	case WLAN_CLIENT_CONNECT_EX:
		HDD_IPA_LOG(VOS_TRACE_LEVEL_INFO, "%d %d",
				adapter->dev->ifindex, sta_id);

		meta.msg_type = type;
		meta.msg_len = (sizeof(struct ipa_wlan_msg_ex) +
				sizeof(struct ipa_wlan_hdr_attrib_val));
		msg_ex = adf_os_mem_alloc (NULL, meta.msg_len);

		if (msg_ex == NULL) {
			HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR,
					"msg_ex allocation failed");
			return -ENOMEM;
		}
		strlcpy(msg_ex->name, adapter->dev->name,
				IPA_RESOURCE_NAME_MAX);
		msg_ex->num_of_attribs = 1;
		msg_ex->attribs[0].attrib_type = WLAN_HDR_ATTRIB_MAC_ADDR;
		msg_ex->attribs[0].offset = HDD_IPA_WLAN_HDR_DES_MAC_OFFSET;
		memcpy(msg_ex->attribs[0].u.mac_addr, mac_addr,
				IPA_MAC_ADDR_SIZE);

		ret = ipa_send_msg(&meta, msg_ex, hdd_ipa_msg_free_fn);

		if (ret) {
			HDD_IPA_LOG(VOS_TRACE_LEVEL_INFO, "%s: Evt: %d : %d",
					msg_ex->name, meta.msg_type,  ret);
			adf_os_mem_free(msg_ex);
			return ret;
		}
		hdd_ipa->stats.num_send_msg++;

		return 0;
	case WLAN_CLIENT_DISCONNECT:
		break;

	default:
		return 0;
	}

	meta.msg_len = sizeof(struct ipa_wlan_msg);
	msg = adf_os_mem_alloc(NULL, meta.msg_len);
	if (msg == NULL) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR, "msg allocation failed");
		return -ENOMEM;
	}

	meta.msg_type = type;
	strlcpy(msg->name, adapter->dev->name, IPA_RESOURCE_NAME_MAX);
	memcpy(msg->mac_addr, mac_addr, ETH_ALEN);

	HDD_IPA_LOG(VOS_TRACE_LEVEL_INFO, "%s: Evt: %d",
					msg->name, meta.msg_type);

	ret = ipa_send_msg(&meta, msg, hdd_ipa_msg_free_fn);

	if (ret) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_INFO, "%s: Evt: %d fail:%d",
					msg->name, meta.msg_type,  ret);
		adf_os_mem_free(msg);
		return ret;
	}

	hdd_ipa->stats.num_send_msg++;

end:
	return ret;
}


static void hdd_ipa_rx_pipe_desc_free(void)
{
	struct hdd_ipa_priv *hdd_ipa = ghdd_ipa;
	uint32_t i = 0, max_desc_cnt;
	struct ipa_tx_data_desc *desc, *tmp;

	max_desc_cnt = hdd_ipa->hw_desc_cnt * HDD_IPA_DESC_BUFFER_RATIO;

	spin_lock_bh(&hdd_ipa->q_lock);
	list_for_each_entry_safe(desc, tmp, &hdd_ipa->free_desc_head, link) {
		list_del(&desc->link);
		spin_unlock_bh(&hdd_ipa->q_lock);
		adf_os_mem_free(desc);
		spin_lock_bh(&hdd_ipa->q_lock);
		i++;
	}
	spin_unlock_bh(&hdd_ipa->q_lock);

	if (i != max_desc_cnt)
		HDD_IPA_LOG(VOS_TRACE_LEVEL_FATAL, "free desc leak");

}


static int hdd_ipa_rx_pipe_desc_alloc(void)
{
	struct hdd_ipa_priv *hdd_ipa = ghdd_ipa;
	uint32_t i, max_desc_cnt;
	int ret = 0;
	struct ipa_tx_data_desc *tmp_desc;

	hdd_ipa->hw_desc_cnt = IPA_NUM_OF_FIFO_DESC(
				hdd_ipa->hdd_ctx->cfg_ini->IpaDescSize);
	max_desc_cnt = hdd_ipa->hw_desc_cnt * HDD_IPA_DESC_BUFFER_RATIO;

	spin_lock_init(&hdd_ipa->q_lock);

	INIT_LIST_HEAD(&hdd_ipa->free_desc_head);
	INIT_LIST_HEAD(&hdd_ipa->pend_desc_head);
	hdd_ipa->freeq_cnt = max_desc_cnt;
	for (i = 0; i < max_desc_cnt; i++) {
		tmp_desc = adf_os_mem_alloc(NULL,
				sizeof(struct ipa_tx_data_desc));
		if (!tmp_desc) {
			ret = -ENOMEM;

			HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR,
					"Descriptor allocation failed");
			goto fail;
		}
		spin_lock_bh(&hdd_ipa->q_lock);
		list_add_tail(&tmp_desc->link, &hdd_ipa->free_desc_head);
		spin_unlock_bh(&hdd_ipa->q_lock);
	}


	HDD_IPA_LOG(VOS_TRACE_LEVEL_INFO,
		"Desc sz:%d h_desc_cnt:%d freeq_cnt:%u",
		hdd_ipa->hdd_ctx->cfg_ini->IpaDescSize, hdd_ipa->hw_desc_cnt,
						hdd_ipa->freeq_cnt);
	return ret;
fail:
	hdd_ipa_rx_pipe_desc_free();
	return ret;
}

static inline char *hdd_ipa_rm_state_to_str(enum hdd_ipa_rm_state state)
{
	switch (state) {
	case HDD_IPA_RM_RELEASED: return "RELEASED";
	case HDD_IPA_RM_GRANT_PENDING: return "GRANT_PENDING";
	case HDD_IPA_RM_GRANTED: return "GRANTED";
	}

	return "UNKNOWN";
}

static ssize_t hdd_ipa_debugfs_read_ipa_stats(struct file *file,
		char __user *user_buf, size_t count, loff_t *ppos)
{
	struct  hdd_ipa_priv *hdd_ipa = file->private_data;
	char *buf;
	unsigned int len = 0, buf_len = 4096;
	ssize_t ret_cnt;
	int i;
	struct hdd_ipa_iface_context *iface_context = NULL;
#define HDD_IPA_STATS(_buf, _len, _hdd_ipa, _name) \
	scnprintf(_buf, _len, "%30s: %llu\n", #_name, _hdd_ipa->stats._name)

#define HDD_IPA_IFACE_STATS(_buf, _len, _iface, _name) \
	scnprintf(_buf, _len, "%30s: %llu\n", #_name, _iface->stats._name)

	buf = kzalloc(buf_len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	len += scnprintf(buf + len, buf_len - len,
			"\nhw_desc_cnt/pending_cnt: %u/%u, "
			"freeq_cnt/pend_q_cnt: %u/%u\n", hdd_ipa->hw_desc_cnt,
			hdd_ipa->pending_hw_desc_cnt, hdd_ipa->freeq_cnt,
			hdd_ipa->pend_q_cnt);

	len += scnprintf(buf + len, buf_len - len,
			"\n<------------------ WLAN EVENTS STATS"
			" ------------------>\n");
	for (i = 0; i < IPA_WLAN_EVENT_MAX; i++) {
		len += scnprintf(buf + len, buf_len - len, "%30s: %u\n",
				hdd_ipa_wlan_event_to_str(i),
				hdd_ipa->stats.event[i]);
	}
	len += HDD_IPA_STATS(buf + len, buf_len - len, hdd_ipa, num_send_msg);
	len += HDD_IPA_STATS(buf + len, buf_len - len, hdd_ipa, num_free_msg);

	if (!hdd_ipa_is_rm_enabled(hdd_ipa))
		goto skip;

	len += scnprintf(buf + len, buf_len - len,
			"\n<------------------ IPA RM STATS"
			" ------------------>\n");

	len += scnprintf(buf + len, buf_len - len, "%30s: %s\n", "rm_state",
			hdd_ipa_rm_state_to_str(hdd_ipa->rm_state));

	len += scnprintf(buf + len, buf_len - len, "%30s: %d\n", "tx_ref_cnt",
			atomic_read(&hdd_ipa->tx_ref_cnt));

	len += HDD_IPA_STATS(buf + len, buf_len - len, hdd_ipa, num_rm_grant);
	len += HDD_IPA_STATS(buf + len, buf_len - len, hdd_ipa, num_rm_release);
	len += HDD_IPA_STATS(buf + len, buf_len - len, hdd_ipa,
			num_rm_grant_imm);

	if (!hdd_ipa_is_clk_scaling_enabled(hdd_ipa))
		goto skip;

	len += HDD_IPA_STATS(buf + len, buf_len - len, hdd_ipa,
			num_cons_perf_req);
	len += HDD_IPA_STATS(buf + len, buf_len - len, hdd_ipa,
			num_prod_perf_req);
	len += scnprintf(buf + len, buf_len - len, "%30s: %u\n", "curr_prod_bw",
			hdd_ipa->curr_prod_bw);
	len += scnprintf(buf + len, buf_len - len, "%30s: %u\n", "curr_cons_bw",
			hdd_ipa->curr_cons_bw);

skip:
	len += scnprintf(buf + len, buf_len - len,
			"\n<------------------ IPA STATS"
			" ------------------>\n");
	len += HDD_IPA_STATS(buf + len, buf_len - len, hdd_ipa, num_rx_drop);
	len += HDD_IPA_STATS(buf + len, buf_len - len, hdd_ipa,
			num_rx_ipa_tx_dp);
	len += HDD_IPA_STATS(buf + len, buf_len - len, hdd_ipa,
			num_rx_ipa_splice);
	len += HDD_IPA_STATS(buf + len, buf_len - len, hdd_ipa,
			num_rx_ipa_loop);
	len += HDD_IPA_STATS(buf + len, buf_len - len, hdd_ipa,
			num_rx_ipa_tx_dp_err);
	len += HDD_IPA_STATS(buf + len, buf_len - len, hdd_ipa,
			num_rx_ipa_write_done);
	len += HDD_IPA_STATS(buf + len, buf_len - len, hdd_ipa,
			num_max_ipa_tx_mul);
	len += HDD_IPA_STATS(buf + len, buf_len - len, hdd_ipa,
			num_rx_ipa_hw_maxed_out);
	len += HDD_IPA_STATS(buf + len, buf_len - len, hdd_ipa,
			max_pend_q_cnt);

	len += HDD_IPA_STATS(buf + len, buf_len - len, hdd_ipa,
			num_tx_comp_cnt);

	len += HDD_IPA_STATS(buf + len, buf_len - len, hdd_ipa,
			num_freeq_empty);
	len += HDD_IPA_STATS(buf + len, buf_len - len, hdd_ipa,
			num_pri_freeq_empty);

	len += scnprintf(buf + len, buf_len - len,
			"\n<------------------ IPA IFACE STATS"
			" ------------------>\n");

	for (i = 0; i < HDD_IPA_MAX_IFACE; i++) {

		iface_context = &hdd_ipa->iface_context[i];

		if (iface_context->adapter == NULL)
			continue;

		len += scnprintf(buf + len, buf_len - len,
				"\n%s: iface_id: %u, sta_id: %u,"
				" device_mode: %u\n",
				iface_context->adapter->dev->name,
				iface_context->iface_id,
				iface_context->sta_id,
				iface_context->adapter->device_mode);
		len += HDD_IPA_IFACE_STATS(buf + len, buf_len - len,
				iface_context, num_tx);
		len += HDD_IPA_IFACE_STATS(buf + len, buf_len - len,
				iface_context, num_tx_drop);
		len += HDD_IPA_IFACE_STATS(buf + len, buf_len - len,
				iface_context, num_tx_err);
		len += HDD_IPA_IFACE_STATS(buf + len, buf_len - len,
				iface_context, num_tx_cac_drop);
		len += HDD_IPA_IFACE_STATS(buf + len, buf_len - len,
				iface_context, num_rx_prefilter);
		len += HDD_IPA_IFACE_STATS(buf + len, buf_len - len,
				iface_context, num_rx_ipa_excep);
		len += HDD_IPA_IFACE_STATS(buf + len, buf_len - len,
				iface_context, num_rx_recv);
		len += HDD_IPA_IFACE_STATS(buf + len, buf_len - len,
				iface_context, num_rx_recv_mul);
		len += HDD_IPA_IFACE_STATS(buf + len, buf_len - len,
				iface_context, num_rx_send_desc_err);
		len += HDD_IPA_IFACE_STATS(buf + len, buf_len - len,
				iface_context, max_rx_mul);
	}

	ret_cnt = simple_read_from_buffer(user_buf, count, ppos, buf, len);

	kfree(buf);
	return ret_cnt;
#undef HDD_IPA_STATS
#undef HDD_IPA_IFACE_STATS
}

static ssize_t hdd_ipa_debugfs_write_ipa_stats(struct file *file,
		const char __user *user_buf, size_t count, loff_t *ppos)
{
	struct  hdd_ipa_priv *hdd_ipa = file->private_data;
	struct hdd_ipa_iface_context *iface_context = NULL;
	int ret;
	uint32_t val;
	int i;

	ret = kstrtou32_from_user(user_buf, count, 0, &val);

	if (ret)
		return ret;

	if (val == 0) {
		for (i = 0; i < HDD_IPA_MAX_IFACE; i++) {
			iface_context = &hdd_ipa->iface_context[i];
			memset(&iface_context->stats, 0,
					sizeof(iface_context->stats));
		}

		memset(&hdd_ipa->stats, 0, sizeof(hdd_ipa->stats));
	}

	return count;
}

static const struct file_operations fops_ipa_stats = {
		.read = hdd_ipa_debugfs_read_ipa_stats,
		.write = hdd_ipa_debugfs_write_ipa_stats,
		.open = simple_open,
		.owner = THIS_MODULE,
		.llseek = default_llseek,
};


static int hdd_ipa_debugfs_init(struct hdd_ipa_priv *hdd_ipa)
{
#ifdef WLAN_OPEN_SOURCE
	hdd_ipa->debugfs_dir = debugfs_create_dir("cld",
					hdd_ipa->hdd_ctx->wiphy->debugfsdir);
	if (!hdd_ipa->debugfs_dir)
		return -ENOMEM;

	debugfs_create_file("ipa-stats", S_IRUSR, hdd_ipa->debugfs_dir,
						hdd_ipa, &fops_ipa_stats);
#endif
	return 0;
}

static void hdd_ipa_debugfs_remove(struct hdd_ipa_priv *hdd_ipa)
{
#ifdef WLAN_OPEN_SOURCE
	debugfs_remove_recursive(hdd_ipa->debugfs_dir);
#endif
}

/**
* hdd_ipa_init() - Allocate hdd_ipa resources, ipa pipe resource and register
* wlan interface with IPA module.
* @param
* hdd_ctx  : [in] pointer to HDD context
* @return         : VOS_STATUS_E_FAILURE - Errors
*                 : VOS_STATUS_SUCCESS - Ok
*/
VOS_STATUS hdd_ipa_init(hdd_context_t *hdd_ctx)
{
	struct hdd_ipa_priv *hdd_ipa = NULL;
	int ret, i;
	struct hdd_ipa_iface_context *iface_context = NULL;

	if (!hdd_ipa_is_enabled(hdd_ctx))
		return VOS_STATUS_SUCCESS;

	hdd_ipa = adf_os_mem_alloc(NULL, sizeof(struct hdd_ipa_priv));
	if (!hdd_ipa) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_FATAL, "hdd_ipa allocation failed");
		goto fail_setup_rm;
	}

	hdd_ctx->hdd_ipa = hdd_ipa;
	ghdd_ipa = hdd_ipa;
	hdd_ipa->hdd_ctx = hdd_ctx;

	/* Create the interface context */
	for (i = 0; i < HDD_IPA_MAX_IFACE; i++) {
		iface_context = &hdd_ipa->iface_context[i];
		iface_context->hdd_ipa = hdd_ipa;
		iface_context->cons_client =
			hdd_ipa_adapter_2_client[i].cons_client;
		iface_context->prod_client =
			hdd_ipa_adapter_2_client[i].prod_client;
		iface_context->iface_id = i;
		adf_os_spinlock_init(&iface_context->interface_lock);
	}

	ret = hdd_ipa_setup_rm(hdd_ipa);
	if (ret)
		goto fail_setup_rm;

	ret = hdd_ipa_setup_sys_pipe(hdd_ipa);
	if (ret)
		goto fail_create_sys_pipe;

	ret = hdd_ipa_rx_pipe_desc_alloc();
	if (ret)
		goto fail_alloc_rx_pipe_desc;

	ret = hdd_ipa_debugfs_init(hdd_ipa);
	if (ret)
		goto fail_alloc_rx_pipe_desc;
	hdd_ipa->ipv4_notifier.notifier_call = hdd_ipa_ipv4_changed;
	ret = register_inetaddr_notifier(&hdd_ipa->ipv4_notifier);
	if (ret)
		HDD_IPA_LOG(VOS_TRACE_LEVEL_ERROR,
				"WLAN IPv4 local filter register failed");

	return VOS_STATUS_SUCCESS;
fail_alloc_rx_pipe_desc:
	hdd_ipa_rx_pipe_desc_free();
fail_create_sys_pipe:
	hdd_ipa_destory_rm_resource(hdd_ipa);
fail_setup_rm:
	if (hdd_ipa)
		adf_os_mem_free(hdd_ipa);

	return VOS_STATUS_E_FAILURE;
}

VOS_STATUS hdd_ipa_cleanup(hdd_context_t *hdd_ctx)
{
	struct hdd_ipa_priv *hdd_ipa = hdd_ctx->hdd_ipa;
	int i;
	struct hdd_ipa_iface_context *iface_context = NULL;

	if (!hdd_ipa_is_enabled(hdd_ctx))
		return VOS_STATUS_SUCCESS;

	/* destory the interface lock */
	for (i = 0; i < HDD_IPA_MAX_IFACE; i++) {
		iface_context = &hdd_ipa->iface_context[i];
		adf_os_spinlock_destroy(&iface_context->interface_lock);
	}

	hdd_ipa_debugfs_remove(hdd_ipa);

	if (hdd_ipa->pending_hw_desc_cnt != 0) {
		HDD_IPA_LOG(VOS_TRACE_LEVEL_FATAL, "IPA Pending write done: %d",
				hdd_ipa->pending_hw_desc_cnt);
		msleep(5);
	}
	hdd_ipa_rx_pipe_desc_free();
	hdd_ipa_teardown_sys_pipe(hdd_ipa);
	hdd_ipa_destory_rm_resource(hdd_ipa);

	unregister_inetaddr_notifier(&hdd_ipa->ipv4_notifier);
	adf_os_mem_free(hdd_ipa);
	hdd_ctx->hdd_ipa = NULL;

	return VOS_STATUS_SUCCESS;
}
#endif
