/*
 * Author: Chen Minqiang <ptpt52@gmail.com>
 *  Date : Sun, 05 Jun 2016 16:23:40 +0800
 */
#include <linux/module.h>
#include <linux/version.h>
#include <linux/ip.h>
#include <linux/if_ether.h>
#include <linux/netfilter.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_core.h>
#include <net/netfilter/nf_conntrack_zones.h>
#include <net/netfilter/nf_nat.h>
#include <net/netfilter/nf_nat_core.h>
#include "natcap.h"
#include "natcap_common.h"
#include "natcap_server.h"

static inline int verify_client(const struct net_device *in, const struct net_device *out, struct sk_buff *skb, const struct natcap_tcp_option *nto)
{
	int ret;
	unsigned char old_mac[ETH_ALEN];
	struct ethhdr *eth = eth_hdr(skb);
	memcpy(old_mac, eth->h_source, ETH_ALEN);
	memcpy(eth->h_source, nto->mac_addr, ETH_ALEN);
	ret = ip_set_test_src_mac(in, out, skb, "vclist");
	memcpy(eth->h_source, old_mac, ETH_ALEN);
	if (ret > 0)
		return 0;
	return -1;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 13, 0)
static unsigned int natcap_server_in_hook(unsigned int hooknum,
		struct sk_buff *skb,
		const struct net_device *in,
		const struct net_device *out,
		int (*okfn)(struct sk_buff *))
{
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4, 1, 0)
static unsigned int natcap_server_in_hook(const struct nf_hook_ops *ops,
		struct sk_buff *skb,
		const struct net_device *in,
		const struct net_device *out,
		int (*okfn)(struct sk_buff *))
{
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
static unsigned int natcap_server_in_hook(const struct nf_hook_ops *ops,
		struct sk_buff *skb,
		const struct nf_hook_state *state)
{
	const struct net_device *in = state->in;
	const struct net_device *out = state->out;
#else
static unsigned int natcap_server_in_hook(void *priv,
		struct sk_buff *skb,
		const struct nf_hook_state *state)
{
	const struct net_device *in = state->in;
	const struct net_device *out = state->out;
#endif
	int ret = 0;
	enum ip_conntrack_info ctinfo;
	struct nf_conn *ct;
	struct iphdr *iph;
	struct tcphdr *tcph;
	struct natcap_tcp_option nto;
	struct tuple server;

	iph = ip_hdr(skb);

	if (iph->protocol != IPPROTO_TCP)
		return NF_ACCEPT;

	tcph = (struct tcphdr *)((void *)iph + iph->ihl * 4);

	ct = nf_ct_get(skb, &ctinfo);
	if (NULL == ct) {
		return NF_ACCEPT;
	}

	if (CTINFO2DIR(ctinfo) != IP_CT_DIR_ORIGINAL) {
		return NF_ACCEPT;
	}

	if (test_bit(IPS_NATCAP_BYPASS_BIT, &ct->status)) {
		return NF_ACCEPT;
	}

	if (test_bit(IPS_NATCAP_BIT, &ct->status)) {
		if (skb_csum_test(skb) != 0) {
			NATCAP_ERROR("(SERVER_IN)" DEBUG_FMT ": checksum failed\n", DEBUG_ARG(iph,tcph));
			return NF_DROP;
		}
		NATCAP_DEBUG("(SERVER_IN)" DEBUG_FMT ": before decode\n", DEBUG_ARG(iph,tcph));

		nto.opt.dnat = 0;
		nto.opt.encryption = !!test_bit(IPS_NATCAP_ENC_BIT, &ct->status);

		ret = natcap_tcp_decode(skb, &nto, 1);
		//reload
		iph = ip_hdr(skb);
		tcph = (struct tcphdr *)((void *)iph + iph->ihl*4);
	} else {
		if (!tcph->syn || tcph->ack) {
			NATCAP_WARN("(SERVER_IN)" DEBUG_FMT ": first packet in but not syn\n", DEBUG_ARG(iph,tcph));
			set_bit(IPS_NATCAP_BYPASS_BIT, &ct->status);
			return NF_ACCEPT;
		}

		nto.opt.dnat = 0;
		nto.opt.encryption = 0;
		ret = natcap_tcp_decode(skb, &nto, 1);
		//reload
		iph = ip_hdr(skb);
		tcph = (struct tcphdr *)((void *)iph + iph->ihl*4);

		//not a natcap packet
		if (ret != 0) {
			set_bit(IPS_NATCAP_BYPASS_BIT, &ct->status);
			return NF_ACCEPT;
		}

		if (verify_client(in, out, skb, &nto) != 0) {
			set_bit(IPS_NATCAP_BYPASS_BIT, &ct->status);
			NATCAP_INFO("(SERVER_IN)" DEBUG_FMT ": client mac=%02X:%02X:%02X:%02X:%02X:%02X, u_hash=%u verified failed\n", DEBUG_ARG(iph,tcph),
					nto.mac_addr[0], nto.mac_addr[1], nto.mac_addr[2], nto.mac_addr[3], nto.mac_addr[4], nto.mac_addr[5], ntohs(nto.u_hash));
			return NF_ACCEPT;
		}

		NATCAP_INFO("(SERVER_IN)" DEBUG_FMT ": client mac=%02X:%02X:%02X:%02X:%02X:%02X, u_hash=%u verified ok\n", DEBUG_ARG(iph,tcph),
				nto.mac_addr[0], nto.mac_addr[1], nto.mac_addr[2], nto.mac_addr[3], nto.mac_addr[4], nto.mac_addr[5], ntohs(nto.u_hash));

		server.ip = nto.opt.ip;
		server.port = nto.opt.port;
		server.encryption = nto.opt.encryption;

		if (!test_and_set_bit(IPS_NATCAP_BIT, &ct->status)) { /* first time */
			NATCAP_INFO("(SERVER_IN)" DEBUG_FMT ": new natcaped connection in, after decode target=" TUPLE_FMT "\n",
					DEBUG_ARG(iph,tcph), TUPLE_ARG(&server));

			if (nto.opt.dnat && natcap_tcp_dnat_setup(ct, server.ip, server.port) != NF_ACCEPT) {
				NATCAP_ERROR("(SERVER_IN)" DEBUG_FMT ": natcap_tcp_dnat_setup failed, target=" TUPLE_FMT "\n",
						DEBUG_ARG(iph,tcph), TUPLE_ARG(&server));
				set_bit(IPS_NATCAP_BYPASS_BIT, &ct->status);
				return NF_DROP;
			}
			if (server.encryption) {
				set_bit(IPS_NATCAP_ENC_BIT, &ct->status);
			}
		}
	}

	if (ret != 0) {
		NATCAP_ERROR("(SERVER_IN)" DEBUG_FMT ": natcap_tcp_decode ret = %d\n",
			DEBUG_ARG(iph,tcph), ret);
		return NF_DROP;
	}

	skb->mark = XT_MARK_NATCAP;

	NATCAP_DEBUG("(SERVER_IN)" DEBUG_FMT ": after decode\n", DEBUG_ARG(iph,tcph));

	return NF_ACCEPT;
}

//PREROUTING->*POSTROUTING*
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 13, 0)
static unsigned int natcap_server_out_hook(unsigned int hooknum,
		struct sk_buff *skb,
		const struct net_device *in,
		const struct net_device *out,
		int (*okfn)(struct sk_buff *))
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4, 1, 0)
static unsigned int natcap_server_out_hook(const struct nf_hook_ops *ops,
		struct sk_buff *skb,
		const struct net_device *in,
		const struct net_device *out,
		int (*okfn)(struct sk_buff *))
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
static unsigned int natcap_server_out_hook(const struct nf_hook_ops *ops,
		struct sk_buff *skb,
		const struct nf_hook_state *state)
#else
static unsigned int natcap_server_out_hook(void *priv,
		struct sk_buff *skb,
		const struct nf_hook_state *state)
#endif
{
	int ret = 0;
	enum ip_conntrack_info ctinfo;
	struct nf_conn *ct;
	struct iphdr *iph;
	struct tcphdr *tcph;
	struct natcap_tcp_option nto;

	iph = ip_hdr(skb);

	if (iph->protocol != IPPROTO_TCP)
		return NF_ACCEPT;

	tcph = (struct tcphdr *)((void *)iph + iph->ihl * 4);

	ct = nf_ct_get(skb, &ctinfo);
	if (NULL == ct) {
		return NF_ACCEPT;
	}

	if (CTINFO2DIR(ctinfo) == IP_CT_DIR_ORIGINAL) {
		return NF_ACCEPT;
	}

	if (test_bit(IPS_NATCAP_BYPASS_BIT, &ct->status)) {
		return NF_ACCEPT;
	}

	if (test_bit(IPS_NATCAP_BIT, &ct->status)) {
		//matched
		NATCAP_DEBUG("(SERVER_OUT)" DEBUG_FMT ": before encode\n", DEBUG_ARG(iph,tcph));

		nto.opt.dnat = 0;
		nto.opt.encryption = !!test_bit(IPS_NATCAP_ENC_BIT, &ct->status);
		nto.opt.port = tcph->source;
		nto.opt.ip = iph->saddr;
	} else {
		return NF_ACCEPT;
	}

	/* XXX I just confirm it first  */
	ret = nf_conntrack_confirm(skb);
	if (ret != NF_ACCEPT) {
		return ret;
	}

	//reload
	iph = ip_hdr(skb);
	tcph = (struct tcphdr *)((void *)iph + iph->ihl * 4);

	ret = natcap_tcp_encode(skb, &nto, 1);
	if (ret != 0) {
		NATCAP_ERROR("(SERVER_OUT)" DEBUG_FMT ": natcap_tcp_encode@server ret=%d\n",
				DEBUG_ARG(iph,tcph), ret);
		set_bit(IPS_NATCAP_BYPASS_BIT, &ct->status);
		return NF_DROP;
	}

	NATCAP_DEBUG("(SERVER_OUT)" DEBUG_FMT ":after encode\n", DEBUG_ARG(iph,tcph));

	return NF_ACCEPT;
}

static struct nf_hook_ops server_hooks[] = {
	{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
		.owner = THIS_MODULE,
#endif
		.hook = natcap_server_in_hook,
		.pf = PF_INET,
		.hooknum = NF_INET_PRE_ROUTING,
		.priority = NF_IP_PRI_CONNTRACK + 1,
	},
	{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
		.owner = THIS_MODULE,
#endif
		.hook = natcap_server_out_hook,
		.pf = PF_INET,
		.hooknum = NF_INET_POST_ROUTING,
		.priority = NF_IP_PRI_LAST,
	},
};

int natcap_server_init(void)
{
	int ret = 0;

	need_conntrack();

	ret = nf_register_hooks(server_hooks, ARRAY_SIZE(server_hooks));
	return ret;
}

void natcap_server_exit(void)
{
	nf_unregister_hooks(server_hooks, ARRAY_SIZE(server_hooks));
}