// SPDX-License-Identifier: GPL-2.0-only
/*
 * GENEVE: Generic Network Virtualization Encapsulation
 *
 * Copyright (c) 2015 Red Hat, Inc.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/ethtool.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/etherdevice.h>
#include <linux/hash.h>
#include <net/ipv6_stubs.h>
#include <net/dst_metadata.h>
#include <net/gro_cells.h>
#include <net/rtnetlink.h>
#include <net/geneve.h>
#include <net/gro.h>
#include <net/netdev_lock.h>
#include <net/protocol.h>

#define GENEVE_NETDEV_VER	"0.6"

#define GENEVE_N_VID		(1u << 24)
#define GENEVE_VID_MASK		(GENEVE_N_VID - 1)

#define VNI_HASH_BITS		10
#define VNI_HASH_SIZE		(1<<VNI_HASH_BITS)

static bool log_ecn_error = true;
module_param(log_ecn_error, bool, 0644);
MODULE_PARM_DESC(log_ecn_error, "Log packets received with corrupted ECN");

#define GENEVE_VER 0
#define GENEVE_BASE_HLEN (sizeof(struct udphdr) + sizeof(struct genevehdr))
#define GENEVE_IPV4_HLEN (ETH_HLEN + sizeof(struct iphdr) + GENEVE_BASE_HLEN)
#define GENEVE_IPV6_HLEN (ETH_HLEN + sizeof(struct ipv6hdr) + GENEVE_BASE_HLEN)

/* per-network namespace private data for this module */
struct geneve_net {
	struct list_head	geneve_list;
	/* sock_list is protected by rtnl lock */
	struct list_head	sock_list;
};

static unsigned int geneve_net_id;

struct geneve_dev_node {
	struct hlist_node hlist;
	struct geneve_dev *geneve;
};

struct geneve_config {
	struct ip_tunnel_info	info;
	bool			collect_md;
	bool			use_udp6_rx_checksums;
	bool			ttl_inherit;
	enum ifla_geneve_df	df;
	bool			inner_proto_inherit;
	u16			port_min;
	u16			port_max;
};

/* Pseudo network device */
struct geneve_dev {
	struct geneve_dev_node hlist4;	/* vni hash table for IPv4 socket */
#if IS_ENABLED(CONFIG_IPV6)
	struct geneve_dev_node hlist6;	/* vni hash table for IPv6 socket */
#endif
	struct net	   *net;	/* netns for packet i/o */
	struct net_device  *dev;	/* netdev for geneve tunnel */
	struct geneve_sock __rcu *sock4;	/* IPv4 socket used for geneve tunnel */
#if IS_ENABLED(CONFIG_IPV6)
	struct geneve_sock __rcu *sock6;	/* IPv6 socket used for geneve tunnel */
#endif
	struct list_head   next;	/* geneve's per namespace list */
	struct gro_cells   gro_cells;
	struct geneve_config cfg;
};

struct geneve_sock {
	bool			collect_md;
	struct list_head	list;
	struct socket		*sock;
	struct rcu_head		rcu;
	int			refcnt;
	struct hlist_head	vni_list[VNI_HASH_SIZE];
};

static inline __u32 geneve_net_vni_hash(u8 vni[3])
{
	__u32 vnid;

	vnid = (vni[0] << 16) | (vni[1] << 8) | vni[2];
	return hash_32(vnid, VNI_HASH_BITS);
}

static __be64 vni_to_tunnel_id(const __u8 *vni)
{
#ifdef __BIG_ENDIAN
	return (vni[0] << 16) | (vni[1] << 8) | vni[2];
#else
	return (__force __be64)(((__force u64)vni[0] << 40) |
				((__force u64)vni[1] << 48) |
				((__force u64)vni[2] << 56));
#endif
}

/* Convert 64 bit tunnel ID to 24 bit VNI. */
static void tunnel_id_to_vni(__be64 tun_id, __u8 *vni)
{
#ifdef __BIG_ENDIAN
	vni[0] = (__force __u8)(tun_id >> 16);
	vni[1] = (__force __u8)(tun_id >> 8);
	vni[2] = (__force __u8)tun_id;
#else
	vni[0] = (__force __u8)((__force u64)tun_id >> 40);
	vni[1] = (__force __u8)((__force u64)tun_id >> 48);
	vni[2] = (__force __u8)((__force u64)tun_id >> 56);
#endif
}

static bool eq_tun_id_and_vni(u8 *tun_id, u8 *vni)
{
	return !memcmp(vni, &tun_id[5], 3);
}

static sa_family_t geneve_get_sk_family(struct geneve_sock *gs)
{
	return gs->sock->sk->sk_family;
}

static struct geneve_dev *geneve_lookup(struct geneve_sock *gs,
					__be32 addr, u8 vni[])
{
	struct hlist_head *vni_list_head;
	struct geneve_dev_node *node;
	__u32 hash;

	/* Find the device for this VNI */
	hash = geneve_net_vni_hash(vni);
	vni_list_head = &gs->vni_list[hash];
	hlist_for_each_entry_rcu(node, vni_list_head, hlist) {
		if (eq_tun_id_and_vni((u8 *)&node->geneve->cfg.info.key.tun_id, vni) &&
		    addr == node->geneve->cfg.info.key.u.ipv4.dst)
			return node->geneve;
	}
	return NULL;
}

#if IS_ENABLED(CONFIG_IPV6)
static struct geneve_dev *geneve6_lookup(struct geneve_sock *gs,
					 struct in6_addr addr6, u8 vni[])
{
	struct hlist_head *vni_list_head;
	struct geneve_dev_node *node;
	__u32 hash;

	/* Find the device for this VNI */
	hash = geneve_net_vni_hash(vni);
	vni_list_head = &gs->vni_list[hash];
	hlist_for_each_entry_rcu(node, vni_list_head, hlist) {
		if (eq_tun_id_and_vni((u8 *)&node->geneve->cfg.info.key.tun_id, vni) &&
		    ipv6_addr_equal(&addr6, &node->geneve->cfg.info.key.u.ipv6.dst))
			return node->geneve;
	}
	return NULL;
}
#endif

static inline struct genevehdr *geneve_hdr(const struct sk_buff *skb)
{
	return (struct genevehdr *)(udp_hdr(skb) + 1);
}

static struct geneve_dev *geneve_lookup_skb(struct geneve_sock *gs,
					    struct sk_buff *skb)
{
	static u8 zero_vni[3];
	u8 *vni;

	if (geneve_get_sk_family(gs) == AF_INET) {
		struct iphdr *iph;
		__be32 addr;

		iph = ip_hdr(skb); /* outer IP header... */

		if (gs->collect_md) {
			vni = zero_vni;
			addr = 0;
		} else {
			vni = geneve_hdr(skb)->vni;
			addr = iph->saddr;
		}

		return geneve_lookup(gs, addr, vni);
#if IS_ENABLED(CONFIG_IPV6)
	} else if (geneve_get_sk_family(gs) == AF_INET6) {
		static struct in6_addr zero_addr6;
		struct ipv6hdr *ip6h;
		struct in6_addr addr6;

		ip6h = ipv6_hdr(skb); /* outer IPv6 header... */

		if (gs->collect_md) {
			vni = zero_vni;
			addr6 = zero_addr6;
		} else {
			vni = geneve_hdr(skb)->vni;
			addr6 = ip6h->saddr;
		}

		return geneve6_lookup(gs, addr6, vni);
#endif
	}
	return NULL;
}

/* geneve receive/decap routine */
static void geneve_rx(struct geneve_dev *geneve, struct geneve_sock *gs,
		      struct sk_buff *skb)
{
	struct genevehdr *gnvh = geneve_hdr(skb);
	struct metadata_dst *tun_dst = NULL;
	unsigned int len;
	int nh, err = 0;
	void *oiph;

	if (ip_tunnel_collect_metadata() || gs->collect_md) {
		IP_TUNNEL_DECLARE_FLAGS(flags) = { };

		__set_bit(IP_TUNNEL_KEY_BIT, flags);
		__assign_bit(IP_TUNNEL_OAM_BIT, flags, gnvh->oam);
		__assign_bit(IP_TUNNEL_CRIT_OPT_BIT, flags, gnvh->critical);

		tun_dst = udp_tun_rx_dst(skb, geneve_get_sk_family(gs), flags,
					 vni_to_tunnel_id(gnvh->vni),
					 gnvh->opt_len * 4);
		if (!tun_dst) {
			dev_dstats_rx_dropped(geneve->dev);
			goto drop;
		}
		/* Update tunnel dst according to Geneve options. */
		ip_tunnel_flags_zero(flags);
		__set_bit(IP_TUNNEL_GENEVE_OPT_BIT, flags);
		ip_tunnel_info_opts_set(&tun_dst->u.tun_info,
					gnvh->options, gnvh->opt_len * 4,
					flags);
	} else {
		/* Drop packets w/ critical options,
		 * since we don't support any...
		 */
		if (gnvh->critical) {
			DEV_STATS_INC(geneve->dev, rx_frame_errors);
			DEV_STATS_INC(geneve->dev, rx_errors);
			goto drop;
		}
	}

	if (tun_dst)
		skb_dst_set(skb, &tun_dst->dst);

	if (gnvh->proto_type == htons(ETH_P_TEB)) {
		skb_reset_mac_header(skb);
		skb->protocol = eth_type_trans(skb, geneve->dev);
		skb_postpull_rcsum(skb, eth_hdr(skb), ETH_HLEN);

		/* Ignore packet loops (and multicast echo) */
		if (ether_addr_equal(eth_hdr(skb)->h_source,
				     geneve->dev->dev_addr)) {
			DEV_STATS_INC(geneve->dev, rx_errors);
			goto drop;
		}
	} else {
		skb_reset_mac_header(skb);
		skb->dev = geneve->dev;
		skb->pkt_type = PACKET_HOST;
	}

	/* Save offset of outer header relative to skb->head,
	 * because we are going to reset the network header to the inner header
	 * and might change skb->head.
	 */
	nh = skb_network_header(skb) - skb->head;

	skb_reset_network_header(skb);

	if (!pskb_inet_may_pull(skb)) {
		DEV_STATS_INC(geneve->dev, rx_length_errors);
		DEV_STATS_INC(geneve->dev, rx_errors);
		goto drop;
	}

	/* Get the outer header. */
	oiph = skb->head + nh;

	if (geneve_get_sk_family(gs) == AF_INET)
		err = IP_ECN_decapsulate(oiph, skb);
#if IS_ENABLED(CONFIG_IPV6)
	else
		err = IP6_ECN_decapsulate(oiph, skb);
#endif

	if (unlikely(err)) {
		if (log_ecn_error) {
			if (geneve_get_sk_family(gs) == AF_INET)
				net_info_ratelimited("non-ECT from %pI4 "
						     "with TOS=%#x\n",
						     &((struct iphdr *)oiph)->saddr,
						     ((struct iphdr *)oiph)->tos);
#if IS_ENABLED(CONFIG_IPV6)
			else
				net_info_ratelimited("non-ECT from %pI6\n",
						     &((struct ipv6hdr *)oiph)->saddr);
#endif
		}
		if (err > 1) {
			DEV_STATS_INC(geneve->dev, rx_frame_errors);
			DEV_STATS_INC(geneve->dev, rx_errors);
			goto drop;
		}
	}

	len = skb->len;
	err = gro_cells_receive(&geneve->gro_cells, skb);
	if (likely(err == NET_RX_SUCCESS))
		dev_dstats_rx_add(geneve->dev, len);

	return;
drop:
	/* Consume bad packet */
	kfree_skb(skb);
}

/* Setup stats when device is created */
static int geneve_init(struct net_device *dev)
{
	struct geneve_dev *geneve = netdev_priv(dev);
	int err;

	err = gro_cells_init(&geneve->gro_cells, dev);
	if (err)
		return err;

	err = dst_cache_init(&geneve->cfg.info.dst_cache, GFP_KERNEL);
	if (err) {
		gro_cells_destroy(&geneve->gro_cells);
		return err;
	}
	netdev_lockdep_set_classes(dev);
	return 0;
}

static void geneve_uninit(struct net_device *dev)
{
	struct geneve_dev *geneve = netdev_priv(dev);

	dst_cache_destroy(&geneve->cfg.info.dst_cache);
	gro_cells_destroy(&geneve->gro_cells);
}

/* Callback from net/ipv4/udp.c to receive packets */
static int geneve_udp_encap_recv(struct sock *sk, struct sk_buff *skb)
{
	struct genevehdr *geneveh;
	struct geneve_dev *geneve;
	struct geneve_sock *gs;
	__be16 inner_proto;
	int opts_len;

	/* Need UDP and Geneve header to be present */
	if (unlikely(!pskb_may_pull(skb, GENEVE_BASE_HLEN)))
		goto drop;

	/* Return packets with reserved bits set */
	geneveh = geneve_hdr(skb);
	if (unlikely(geneveh->ver != GENEVE_VER))
		goto drop;

	gs = rcu_dereference_sk_user_data(sk);
	if (!gs)
		goto drop;

	geneve = geneve_lookup_skb(gs, skb);
	if (!geneve)
		goto drop;

	inner_proto = geneveh->proto_type;

	if (unlikely((!geneve->cfg.inner_proto_inherit &&
		      inner_proto != htons(ETH_P_TEB)))) {
		dev_dstats_rx_dropped(geneve->dev);
		goto drop;
	}

	opts_len = geneveh->opt_len * 4;
	if (iptunnel_pull_header(skb, GENEVE_BASE_HLEN + opts_len, inner_proto,
				 !net_eq(geneve->net, dev_net(geneve->dev)))) {
		dev_dstats_rx_dropped(geneve->dev);
		goto drop;
	}

	geneve_rx(geneve, gs, skb);
	return 0;

drop:
	/* Consume bad packet */
	kfree_skb(skb);
	return 0;
}

/* Callback from net/ipv{4,6}/udp.c to check that we have a tunnel for errors */
static int geneve_udp_encap_err_lookup(struct sock *sk, struct sk_buff *skb)
{
	struct genevehdr *geneveh;
	struct geneve_sock *gs;
	u8 zero_vni[3] = { 0 };
	u8 *vni = zero_vni;

	if (!pskb_may_pull(skb, skb_transport_offset(skb) + GENEVE_BASE_HLEN))
		return -EINVAL;

	geneveh = geneve_hdr(skb);
	if (geneveh->ver != GENEVE_VER)
		return -EINVAL;

	if (geneveh->proto_type != htons(ETH_P_TEB))
		return -EINVAL;

	gs = rcu_dereference_sk_user_data(sk);
	if (!gs)
		return -ENOENT;

	if (geneve_get_sk_family(gs) == AF_INET) {
		struct iphdr *iph = ip_hdr(skb);
		__be32 addr4 = 0;

		if (!gs->collect_md) {
			vni = geneve_hdr(skb)->vni;
			addr4 = iph->daddr;
		}

		return geneve_lookup(gs, addr4, vni) ? 0 : -ENOENT;
	}

#if IS_ENABLED(CONFIG_IPV6)
	if (geneve_get_sk_family(gs) == AF_INET6) {
		struct ipv6hdr *ip6h = ipv6_hdr(skb);
		struct in6_addr addr6;

		memset(&addr6, 0, sizeof(struct in6_addr));

		if (!gs->collect_md) {
			vni = geneve_hdr(skb)->vni;
			addr6 = ip6h->daddr;
		}

		return geneve6_lookup(gs, addr6, vni) ? 0 : -ENOENT;
	}
#endif

	return -EPFNOSUPPORT;
}

static struct socket *geneve_create_sock(struct net *net, bool ipv6,
					 __be16 port, bool ipv6_rx_csum)
{
	struct socket *sock;
	struct udp_port_cfg udp_conf;
	int err;

	memset(&udp_conf, 0, sizeof(udp_conf));

	if (ipv6) {
		udp_conf.family = AF_INET6;
		udp_conf.ipv6_v6only = 1;
		udp_conf.use_udp6_rx_checksums = ipv6_rx_csum;
	} else {
		udp_conf.family = AF_INET;
		udp_conf.local_ip.s_addr = htonl(INADDR_ANY);
	}

	udp_conf.local_udp_port = port;

	/* Open UDP socket */
	err = udp_sock_create(net, &udp_conf, &sock);
	if (err < 0)
		return ERR_PTR(err);

	udp_allow_gso(sock->sk);
	return sock;
}

static int geneve_hlen(struct genevehdr *gh)
{
	return sizeof(*gh) + gh->opt_len * 4;
}

static struct sk_buff *geneve_gro_receive(struct sock *sk,
					  struct list_head *head,
					  struct sk_buff *skb)
{
	struct sk_buff *pp = NULL;
	struct sk_buff *p;
	struct genevehdr *gh, *gh2;
	unsigned int hlen, gh_len, off_gnv;
	const struct packet_offload *ptype;
	__be16 type;
	int flush = 1;

	off_gnv = skb_gro_offset(skb);
	hlen = off_gnv + sizeof(*gh);
	gh = skb_gro_header(skb, hlen, off_gnv);
	if (unlikely(!gh))
		goto out;

	if (gh->ver != GENEVE_VER || gh->oam)
		goto out;
	gh_len = geneve_hlen(gh);

	hlen = off_gnv + gh_len;
	if (!skb_gro_may_pull(skb, hlen)) {
		gh = skb_gro_header_slow(skb, hlen, off_gnv);
		if (unlikely(!gh))
			goto out;
	}

	list_for_each_entry(p, head, list) {
		if (!NAPI_GRO_CB(p)->same_flow)
			continue;

		gh2 = (struct genevehdr *)(p->data + off_gnv);
		if (gh->opt_len != gh2->opt_len ||
		    memcmp(gh, gh2, gh_len)) {
			NAPI_GRO_CB(p)->same_flow = 0;
			continue;
		}
	}

	skb_gro_pull(skb, gh_len);
	skb_gro_postpull_rcsum(skb, gh, gh_len);
	type = gh->proto_type;
	if (likely(type == htons(ETH_P_TEB)))
		return call_gro_receive(eth_gro_receive, head, skb);

	ptype = gro_find_receive_by_type(type);
	if (!ptype)
		goto out;

	pp = call_gro_receive(ptype->callbacks.gro_receive, head, skb);
	flush = 0;

out:
	skb_gro_flush_final(skb, pp, flush);

	return pp;
}

static int geneve_gro_complete(struct sock *sk, struct sk_buff *skb,
			       int nhoff)
{
	struct genevehdr *gh;
	struct packet_offload *ptype;
	__be16 type;
	int gh_len;
	int err = -ENOSYS;

	gh = (struct genevehdr *)(skb->data + nhoff);
	gh_len = geneve_hlen(gh);
	type = gh->proto_type;

	/* since skb->encapsulation is set, eth_gro_complete() sets the inner mac header */
	if (likely(type == htons(ETH_P_TEB)))
		return eth_gro_complete(skb, nhoff + gh_len);

	ptype = gro_find_complete_by_type(type);
	if (ptype)
		err = ptype->callbacks.gro_complete(skb, nhoff + gh_len);

	skb_set_inner_mac_header(skb, nhoff + gh_len);

	return err;
}

/* Create new listen socket if needed */
static struct geneve_sock *geneve_socket_create(struct net *net, __be16 port,
						bool ipv6, bool ipv6_rx_csum)
{
	struct geneve_net *gn = net_generic(net, geneve_net_id);
	struct geneve_sock *gs;
	struct socket *sock;
	struct udp_tunnel_sock_cfg tunnel_cfg;
	int h;

	gs = kzalloc(sizeof(*gs), GFP_KERNEL);
	if (!gs)
		return ERR_PTR(-ENOMEM);

	sock = geneve_create_sock(net, ipv6, port, ipv6_rx_csum);
	if (IS_ERR(sock)) {
		kfree(gs);
		return ERR_CAST(sock);
	}

	gs->sock = sock;
	gs->refcnt = 1;
	for (h = 0; h < VNI_HASH_SIZE; ++h)
		INIT_HLIST_HEAD(&gs->vni_list[h]);

	/* Initialize the geneve udp offloads structure */
	udp_tunnel_notify_add_rx_port(gs->sock, UDP_TUNNEL_TYPE_GENEVE);

	/* Mark socket as an encapsulation socket */
	memset(&tunnel_cfg, 0, sizeof(tunnel_cfg));
	tunnel_cfg.sk_user_data = gs;
	tunnel_cfg.encap_type = 1;
	tunnel_cfg.gro_receive = geneve_gro_receive;
	tunnel_cfg.gro_complete = geneve_gro_complete;
	tunnel_cfg.encap_rcv = geneve_udp_encap_recv;
	tunnel_cfg.encap_err_lookup = geneve_udp_encap_err_lookup;
	tunnel_cfg.encap_destroy = NULL;
	setup_udp_tunnel_sock(net, sock, &tunnel_cfg);
	list_add(&gs->list, &gn->sock_list);
	return gs;
}

static void __geneve_sock_release(struct geneve_sock *gs)
{
	if (!gs || --gs->refcnt)
		return;

	list_del(&gs->list);
	udp_tunnel_notify_del_rx_port(gs->sock, UDP_TUNNEL_TYPE_GENEVE);
	udp_tunnel_sock_release(gs->sock);
	kfree_rcu(gs, rcu);
}

static void geneve_sock_release(struct geneve_dev *geneve)
{
	struct geneve_sock *gs4 = rtnl_dereference(geneve->sock4);
#if IS_ENABLED(CONFIG_IPV6)
	struct geneve_sock *gs6 = rtnl_dereference(geneve->sock6);

	rcu_assign_pointer(geneve->sock6, NULL);
#endif

	rcu_assign_pointer(geneve->sock4, NULL);
	synchronize_net();

	__geneve_sock_release(gs4);
#if IS_ENABLED(CONFIG_IPV6)
	__geneve_sock_release(gs6);
#endif
}

static struct geneve_sock *geneve_find_sock(struct geneve_net *gn,
					    sa_family_t family,
					    __be16 dst_port)
{
	struct geneve_sock *gs;

	list_for_each_entry(gs, &gn->sock_list, list) {
		if (inet_sk(gs->sock->sk)->inet_sport == dst_port &&
		    geneve_get_sk_family(gs) == family) {
			return gs;
		}
	}
	return NULL;
}

static int geneve_sock_add(struct geneve_dev *geneve, bool ipv6)
{
	struct net *net = geneve->net;
	struct geneve_net *gn = net_generic(net, geneve_net_id);
	struct geneve_dev_node *node;
	struct geneve_sock *gs;
	__u8 vni[3];
	__u32 hash;

	gs = geneve_find_sock(gn, ipv6 ? AF_INET6 : AF_INET, geneve->cfg.info.key.tp_dst);
	if (gs) {
		gs->refcnt++;
		goto out;
	}

	gs = geneve_socket_create(net, geneve->cfg.info.key.tp_dst, ipv6,
				  geneve->cfg.use_udp6_rx_checksums);
	if (IS_ERR(gs))
		return PTR_ERR(gs);

out:
	gs->collect_md = geneve->cfg.collect_md;
#if IS_ENABLED(CONFIG_IPV6)
	if (ipv6) {
		rcu_assign_pointer(geneve->sock6, gs);
		node = &geneve->hlist6;
	} else
#endif
	{
		rcu_assign_pointer(geneve->sock4, gs);
		node = &geneve->hlist4;
	}
	node->geneve = geneve;

	tunnel_id_to_vni(geneve->cfg.info.key.tun_id, vni);
	hash = geneve_net_vni_hash(vni);
	hlist_add_head_rcu(&node->hlist, &gs->vni_list[hash]);
	return 0;
}

static int geneve_open(struct net_device *dev)
{
	struct geneve_dev *geneve = netdev_priv(dev);
	bool metadata = geneve->cfg.collect_md;
	bool ipv4, ipv6;
	int ret = 0;

	ipv6 = geneve->cfg.info.mode & IP_TUNNEL_INFO_IPV6 || metadata;
	ipv4 = !ipv6 || metadata;
#if IS_ENABLED(CONFIG_IPV6)
	if (ipv6) {
		ret = geneve_sock_add(geneve, true);
		if (ret < 0 && ret != -EAFNOSUPPORT)
			ipv4 = false;
	}
#endif
	if (ipv4)
		ret = geneve_sock_add(geneve, false);
	if (ret < 0)
		geneve_sock_release(geneve);

	return ret;
}

static int geneve_stop(struct net_device *dev)
{
	struct geneve_dev *geneve = netdev_priv(dev);

	hlist_del_init_rcu(&geneve->hlist4.hlist);
#if IS_ENABLED(CONFIG_IPV6)
	hlist_del_init_rcu(&geneve->hlist6.hlist);
#endif
	geneve_sock_release(geneve);
	return 0;
}

static void geneve_build_header(struct genevehdr *geneveh,
				const struct ip_tunnel_info *info,
				__be16 inner_proto)
{
	geneveh->ver = GENEVE_VER;
	geneveh->opt_len = info->options_len / 4;
	geneveh->oam = test_bit(IP_TUNNEL_OAM_BIT, info->key.tun_flags);
	geneveh->critical = test_bit(IP_TUNNEL_CRIT_OPT_BIT,
				     info->key.tun_flags);
	geneveh->rsvd1 = 0;
	tunnel_id_to_vni(info->key.tun_id, geneveh->vni);
	geneveh->proto_type = inner_proto;
	geneveh->rsvd2 = 0;

	if (test_bit(IP_TUNNEL_GENEVE_OPT_BIT, info->key.tun_flags))
		ip_tunnel_info_opts_get(geneveh->options, info);
}

static int geneve_build_skb(struct dst_entry *dst, struct sk_buff *skb,
			    const struct ip_tunnel_info *info,
			    bool xnet, int ip_hdr_len,
			    bool inner_proto_inherit)
{
	bool udp_sum = test_bit(IP_TUNNEL_CSUM_BIT, info->key.tun_flags);
	struct genevehdr *gnvh;
	__be16 inner_proto;
	int min_headroom;
	int err;

	skb_reset_mac_header(skb);
	skb_scrub_packet(skb, xnet);

	min_headroom = LL_RESERVED_SPACE(dst->dev) + dst->header_len +
		       GENEVE_BASE_HLEN + info->options_len + ip_hdr_len;
	err = skb_cow_head(skb, min_headroom);
	if (unlikely(err))
		goto free_dst;

	err = udp_tunnel_handle_offloads(skb, udp_sum);
	if (err)
		goto free_dst;

	gnvh = __skb_push(skb, sizeof(*gnvh) + info->options_len);
	inner_proto = inner_proto_inherit ? skb->protocol : htons(ETH_P_TEB);
	geneve_build_header(gnvh, info, inner_proto);
	skb_set_inner_protocol(skb, inner_proto);
	return 0;

free_dst:
	dst_release(dst);
	return err;
}

static u8 geneve_get_dsfield(struct sk_buff *skb, struct net_device *dev,
			     const struct ip_tunnel_info *info,
			     bool *use_cache)
{
	struct geneve_dev *geneve = netdev_priv(dev);
	u8 dsfield;

	dsfield = info->key.tos;
	if (dsfield == 1 && !geneve->cfg.collect_md) {
		dsfield = ip_tunnel_get_dsfield(ip_hdr(skb), skb);
		*use_cache = false;
	}

	return dsfield;
}

static int geneve_xmit_skb(struct sk_buff *skb, struct net_device *dev,
			   struct geneve_dev *geneve,
			   const struct ip_tunnel_info *info)
{
	bool inner_proto_inherit = geneve->cfg.inner_proto_inherit;
	bool xnet = !net_eq(geneve->net, dev_net(geneve->dev));
	struct geneve_sock *gs4 = rcu_dereference(geneve->sock4);
	const struct ip_tunnel_key *key = &info->key;
	struct rtable *rt;
	bool use_cache;
	__u8 tos, ttl;
	__be16 df = 0;
	__be32 saddr;
	__be16 sport;
	int err;

	if (skb_vlan_inet_prepare(skb, inner_proto_inherit))
		return -EINVAL;

	if (!gs4)
		return -EIO;

	use_cache = ip_tunnel_dst_cache_usable(skb, info);
	tos = geneve_get_dsfield(skb, dev, info, &use_cache);
	sport = udp_flow_src_port(geneve->net, skb,
				  geneve->cfg.port_min,
				  geneve->cfg.port_max, true);

	rt = udp_tunnel_dst_lookup(skb, dev, geneve->net, 0, &saddr,
				   &info->key,
				   sport, geneve->cfg.info.key.tp_dst, tos,
				   use_cache ?
				   (struct dst_cache *)&info->dst_cache : NULL);
	if (IS_ERR(rt))
		return PTR_ERR(rt);

	err = skb_tunnel_check_pmtu(skb, &rt->dst,
				    GENEVE_IPV4_HLEN + info->options_len,
				    netif_is_any_bridge_port(dev));
	if (err < 0) {
		dst_release(&rt->dst);
		return err;
	} else if (err) {
		struct ip_tunnel_info *info;

		info = skb_tunnel_info(skb);
		if (info) {
			struct ip_tunnel_info *unclone;

			unclone = skb_tunnel_info_unclone(skb);
			if (unlikely(!unclone)) {
				dst_release(&rt->dst);
				return -ENOMEM;
			}

			unclone->key.u.ipv4.dst = saddr;
			unclone->key.u.ipv4.src = info->key.u.ipv4.dst;
		}

		if (!pskb_may_pull(skb, ETH_HLEN)) {
			dst_release(&rt->dst);
			return -EINVAL;
		}

		skb->protocol = eth_type_trans(skb, geneve->dev);
		__netif_rx(skb);
		dst_release(&rt->dst);
		return -EMSGSIZE;
	}

	tos = ip_tunnel_ecn_encap(tos, ip_hdr(skb), skb);
	if (geneve->cfg.collect_md) {
		ttl = key->ttl;

		df = test_bit(IP_TUNNEL_DONT_FRAGMENT_BIT, key->tun_flags) ?
		     htons(IP_DF) : 0;
	} else {
		if (geneve->cfg.ttl_inherit)
			ttl = ip_tunnel_get_ttl(ip_hdr(skb), skb);
		else
			ttl = key->ttl;
		ttl = ttl ? : ip4_dst_hoplimit(&rt->dst);

		if (geneve->cfg.df == GENEVE_DF_SET) {
			df = htons(IP_DF);
		} else if (geneve->cfg.df == GENEVE_DF_INHERIT) {
			struct ethhdr *eth = skb_eth_hdr(skb);

			if (ntohs(eth->h_proto) == ETH_P_IPV6) {
				df = htons(IP_DF);
			} else if (ntohs(eth->h_proto) == ETH_P_IP) {
				struct iphdr *iph = ip_hdr(skb);

				if (iph->frag_off & htons(IP_DF))
					df = htons(IP_DF);
			}
		}
	}

	err = geneve_build_skb(&rt->dst, skb, info, xnet, sizeof(struct iphdr),
			       inner_proto_inherit);
	if (unlikely(err))
		return err;

	udp_tunnel_xmit_skb(rt, gs4->sock->sk, skb, saddr, info->key.u.ipv4.dst,
			    tos, ttl, df, sport, geneve->cfg.info.key.tp_dst,
			    !net_eq(geneve->net, dev_net(geneve->dev)),
			    !test_bit(IP_TUNNEL_CSUM_BIT, info->key.tun_flags),
			    0);
	return 0;
}

#if IS_ENABLED(CONFIG_IPV6)
static int geneve6_xmit_skb(struct sk_buff *skb, struct net_device *dev,
			    struct geneve_dev *geneve,
			    const struct ip_tunnel_info *info)
{
	bool inner_proto_inherit = geneve->cfg.inner_proto_inherit;
	bool xnet = !net_eq(geneve->net, dev_net(geneve->dev));
	struct geneve_sock *gs6 = rcu_dereference(geneve->sock6);
	const struct ip_tunnel_key *key = &info->key;
	struct dst_entry *dst = NULL;
	struct in6_addr saddr;
	bool use_cache;
	__u8 prio, ttl;
	__be16 sport;
	int err;

	if (skb_vlan_inet_prepare(skb, inner_proto_inherit))
		return -EINVAL;

	if (!gs6)
		return -EIO;

	use_cache = ip_tunnel_dst_cache_usable(skb, info);
	prio = geneve_get_dsfield(skb, dev, info, &use_cache);
	sport = udp_flow_src_port(geneve->net, skb,
				  geneve->cfg.port_min,
				  geneve->cfg.port_max, true);

	dst = udp_tunnel6_dst_lookup(skb, dev, geneve->net, gs6->sock, 0,
				     &saddr, key, sport,
				     geneve->cfg.info.key.tp_dst, prio,
				     use_cache ?
				     (struct dst_cache *)&info->dst_cache : NULL);
	if (IS_ERR(dst))
		return PTR_ERR(dst);

	err = skb_tunnel_check_pmtu(skb, dst,
				    GENEVE_IPV6_HLEN + info->options_len,
				    netif_is_any_bridge_port(dev));
	if (err < 0) {
		dst_release(dst);
		return err;
	} else if (err) {
		struct ip_tunnel_info *info = skb_tunnel_info(skb);

		if (info) {
			struct ip_tunnel_info *unclone;

			unclone = skb_tunnel_info_unclone(skb);
			if (unlikely(!unclone)) {
				dst_release(dst);
				return -ENOMEM;
			}

			unclone->key.u.ipv6.dst = saddr;
			unclone->key.u.ipv6.src = info->key.u.ipv6.dst;
		}

		if (!pskb_may_pull(skb, ETH_HLEN)) {
			dst_release(dst);
			return -EINVAL;
		}

		skb->protocol = eth_type_trans(skb, geneve->dev);
		__netif_rx(skb);
		dst_release(dst);
		return -EMSGSIZE;
	}

	prio = ip_tunnel_ecn_encap(prio, ip_hdr(skb), skb);
	if (geneve->cfg.collect_md) {
		ttl = key->ttl;
	} else {
		if (geneve->cfg.ttl_inherit)
			ttl = ip_tunnel_get_ttl(ip_hdr(skb), skb);
		else
			ttl = key->ttl;
		ttl = ttl ? : ip6_dst_hoplimit(dst);
	}
	err = geneve_build_skb(dst, skb, info, xnet, sizeof(struct ipv6hdr),
			       inner_proto_inherit);
	if (unlikely(err))
		return err;

	udp_tunnel6_xmit_skb(dst, gs6->sock->sk, skb, dev,
			     &saddr, &key->u.ipv6.dst, prio, ttl,
			     info->key.label, sport, geneve->cfg.info.key.tp_dst,
			     !test_bit(IP_TUNNEL_CSUM_BIT,
				       info->key.tun_flags),
			     0);
	return 0;
}
#endif

static netdev_tx_t geneve_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct geneve_dev *geneve = netdev_priv(dev);
	struct ip_tunnel_info *info = NULL;
	int err;

	if (geneve->cfg.collect_md) {
		info = skb_tunnel_info(skb);
		if (unlikely(!info || !(info->mode & IP_TUNNEL_INFO_TX))) {
			netdev_dbg(dev, "no tunnel metadata\n");
			dev_kfree_skb(skb);
			dev_dstats_tx_dropped(dev);
			return NETDEV_TX_OK;
		}
	} else {
		info = &geneve->cfg.info;
	}

	rcu_read_lock();
#if IS_ENABLED(CONFIG_IPV6)
	if (info->mode & IP_TUNNEL_INFO_IPV6)
		err = geneve6_xmit_skb(skb, dev, geneve, info);
	else
#endif
		err = geneve_xmit_skb(skb, dev, geneve, info);
	rcu_read_unlock();

	if (likely(!err))
		return NETDEV_TX_OK;

	if (err != -EMSGSIZE)
		dev_kfree_skb(skb);

	if (err == -ELOOP)
		DEV_STATS_INC(dev, collisions);
	else if (err == -ENETUNREACH)
		DEV_STATS_INC(dev, tx_carrier_errors);

	DEV_STATS_INC(dev, tx_errors);
	return NETDEV_TX_OK;
}

static int geneve_change_mtu(struct net_device *dev, int new_mtu)
{
	if (new_mtu > dev->max_mtu)
		new_mtu = dev->max_mtu;
	else if (new_mtu < dev->min_mtu)
		new_mtu = dev->min_mtu;

	WRITE_ONCE(dev->mtu, new_mtu);
	return 0;
}

static int geneve_fill_metadata_dst(struct net_device *dev, struct sk_buff *skb)
{
	struct ip_tunnel_info *info = skb_tunnel_info(skb);
	struct geneve_dev *geneve = netdev_priv(dev);
	__be16 sport;

	if (ip_tunnel_info_af(info) == AF_INET) {
		struct rtable *rt;
		struct geneve_sock *gs4 = rcu_dereference(geneve->sock4);
		bool use_cache;
		__be32 saddr;
		u8 tos;

		if (!gs4)
			return -EIO;

		use_cache = ip_tunnel_dst_cache_usable(skb, info);
		tos = geneve_get_dsfield(skb, dev, info, &use_cache);
		sport = udp_flow_src_port(geneve->net, skb,
					  geneve->cfg.port_min,
					  geneve->cfg.port_max, true);

		rt = udp_tunnel_dst_lookup(skb, dev, geneve->net, 0, &saddr,
					   &info->key,
					   sport, geneve->cfg.info.key.tp_dst,
					   tos,
					   use_cache ? &info->dst_cache : NULL);
		if (IS_ERR(rt))
			return PTR_ERR(rt);

		ip_rt_put(rt);
		info->key.u.ipv4.src = saddr;
#if IS_ENABLED(CONFIG_IPV6)
	} else if (ip_tunnel_info_af(info) == AF_INET6) {
		struct dst_entry *dst;
		struct geneve_sock *gs6 = rcu_dereference(geneve->sock6);
		struct in6_addr saddr;
		bool use_cache;
		u8 prio;

		if (!gs6)
			return -EIO;

		use_cache = ip_tunnel_dst_cache_usable(skb, info);
		prio = geneve_get_dsfield(skb, dev, info, &use_cache);
		sport = udp_flow_src_port(geneve->net, skb,
					  geneve->cfg.port_min,
					  geneve->cfg.port_max, true);

		dst = udp_tunnel6_dst_lookup(skb, dev, geneve->net, gs6->sock, 0,
					     &saddr, &info->key, sport,
					     geneve->cfg.info.key.tp_dst, prio,
					     use_cache ? &info->dst_cache : NULL);
		if (IS_ERR(dst))
			return PTR_ERR(dst);

		dst_release(dst);
		info->key.u.ipv6.src = saddr;
#endif
	} else {
		return -EINVAL;
	}

	info->key.tp_src = sport;
	info->key.tp_dst = geneve->cfg.info.key.tp_dst;
	return 0;
}

static const struct net_device_ops geneve_netdev_ops = {
	.ndo_init		= geneve_init,
	.ndo_uninit		= geneve_uninit,
	.ndo_open		= geneve_open,
	.ndo_stop		= geneve_stop,
	.ndo_start_xmit		= geneve_xmit,
	.ndo_change_mtu		= geneve_change_mtu,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_fill_metadata_dst	= geneve_fill_metadata_dst,
};

static void geneve_get_drvinfo(struct net_device *dev,
			       struct ethtool_drvinfo *drvinfo)
{
	strscpy(drvinfo->version, GENEVE_NETDEV_VER, sizeof(drvinfo->version));
	strscpy(drvinfo->driver, "geneve", sizeof(drvinfo->driver));
}

static const struct ethtool_ops geneve_ethtool_ops = {
	.get_drvinfo	= geneve_get_drvinfo,
	.get_link	= ethtool_op_get_link,
};

/* Info for udev, that this is a virtual tunnel endpoint */
static const struct device_type geneve_type = {
	.name = "geneve",
};

/* Calls the ndo_udp_tunnel_add of the caller in order to
 * supply the listening GENEVE udp ports. Callers are expected
 * to implement the ndo_udp_tunnel_add.
 */
static void geneve_offload_rx_ports(struct net_device *dev, bool push)
{
	struct net *net = dev_net(dev);
	struct geneve_net *gn = net_generic(net, geneve_net_id);
	struct geneve_sock *gs;

	ASSERT_RTNL();

	list_for_each_entry(gs, &gn->sock_list, list) {
		if (push) {
			udp_tunnel_push_rx_port(dev, gs->sock,
						UDP_TUNNEL_TYPE_GENEVE);
		} else {
			udp_tunnel_drop_rx_port(dev, gs->sock,
						UDP_TUNNEL_TYPE_GENEVE);
		}
	}
}

/* Initialize the device structure. */
static void geneve_setup(struct net_device *dev)
{
	ether_setup(dev);

	dev->netdev_ops = &geneve_netdev_ops;
	dev->ethtool_ops = &geneve_ethtool_ops;
	dev->needs_free_netdev = true;

	SET_NETDEV_DEVTYPE(dev, &geneve_type);

	dev->features    |= NETIF_F_SG | NETIF_F_HW_CSUM | NETIF_F_FRAGLIST;
	dev->features    |= NETIF_F_RXCSUM;
	dev->features    |= NETIF_F_GSO_SOFTWARE;

	dev->hw_features |= NETIF_F_SG | NETIF_F_HW_CSUM | NETIF_F_FRAGLIST;
	dev->hw_features |= NETIF_F_RXCSUM;
	dev->hw_features |= NETIF_F_GSO_SOFTWARE;

	dev->pcpu_stat_type = NETDEV_PCPU_STAT_DSTATS;
	/* MTU range: 68 - (something less than 65535) */
	dev->min_mtu = ETH_MIN_MTU;
	/* The max_mtu calculation does not take account of GENEVE
	 * options, to avoid excluding potentially valid
	 * configurations. This will be further reduced by IPvX hdr size.
	 */
	dev->max_mtu = IP_MAX_MTU - GENEVE_BASE_HLEN - dev->hard_header_len;

	netif_keep_dst(dev);
	dev->priv_flags &= ~IFF_TX_SKB_SHARING;
	dev->priv_flags |= IFF_LIVE_ADDR_CHANGE | IFF_NO_QUEUE;
	dev->lltx = true;
	eth_hw_addr_random(dev);
}

static const struct nla_policy geneve_policy[IFLA_GENEVE_MAX + 1] = {
	[IFLA_GENEVE_UNSPEC]		= { .strict_start_type = IFLA_GENEVE_INNER_PROTO_INHERIT },
	[IFLA_GENEVE_ID]		= { .type = NLA_U32 },
	[IFLA_GENEVE_REMOTE]		= { .len = sizeof_field(struct iphdr, daddr) },
	[IFLA_GENEVE_REMOTE6]		= { .len = sizeof(struct in6_addr) },
	[IFLA_GENEVE_TTL]		= { .type = NLA_U8 },
	[IFLA_GENEVE_TOS]		= { .type = NLA_U8 },
	[IFLA_GENEVE_LABEL]		= { .type = NLA_U32 },
	[IFLA_GENEVE_PORT]		= { .type = NLA_U16 },
	[IFLA_GENEVE_COLLECT_METADATA]	= { .type = NLA_FLAG },
	[IFLA_GENEVE_UDP_CSUM]		= { .type = NLA_U8 },
	[IFLA_GENEVE_UDP_ZERO_CSUM6_TX]	= { .type = NLA_U8 },
	[IFLA_GENEVE_UDP_ZERO_CSUM6_RX]	= { .type = NLA_U8 },
	[IFLA_GENEVE_TTL_INHERIT]	= { .type = NLA_U8 },
	[IFLA_GENEVE_DF]		= { .type = NLA_U8 },
	[IFLA_GENEVE_INNER_PROTO_INHERIT]	= { .type = NLA_FLAG },
	[IFLA_GENEVE_PORT_RANGE]	= NLA_POLICY_EXACT_LEN(sizeof(struct ifla_geneve_port_range)),
};

static int geneve_validate(struct nlattr *tb[], struct nlattr *data[],
			   struct netlink_ext_ack *extack)
{
	if (tb[IFLA_ADDRESS]) {
		if (nla_len(tb[IFLA_ADDRESS]) != ETH_ALEN) {
			NL_SET_ERR_MSG_ATTR(extack, tb[IFLA_ADDRESS],
					    "Provided link layer address is not Ethernet");
			return -EINVAL;
		}

		if (!is_valid_ether_addr(nla_data(tb[IFLA_ADDRESS]))) {
			NL_SET_ERR_MSG_ATTR(extack, tb[IFLA_ADDRESS],
					    "Provided Ethernet address is not unicast");
			return -EADDRNOTAVAIL;
		}
	}

	if (!data) {
		NL_SET_ERR_MSG(extack,
			       "Not enough attributes provided to perform the operation");
		return -EINVAL;
	}

	if (data[IFLA_GENEVE_ID]) {
		__u32 vni =  nla_get_u32(data[IFLA_GENEVE_ID]);

		if (vni >= GENEVE_N_VID) {
			NL_SET_ERR_MSG_ATTR(extack, data[IFLA_GENEVE_ID],
					    "Geneve ID must be lower than 16777216");
			return -ERANGE;
		}
	}

	if (data[IFLA_GENEVE_DF]) {
		enum ifla_geneve_df df = nla_get_u8(data[IFLA_GENEVE_DF]);

		if (df < 0 || df > GENEVE_DF_MAX) {
			NL_SET_ERR_MSG_ATTR(extack, data[IFLA_GENEVE_DF],
					    "Invalid DF attribute");
			return -EINVAL;
		}
	}

	if (data[IFLA_GENEVE_PORT_RANGE]) {
		const struct ifla_geneve_port_range *p;

		p = nla_data(data[IFLA_GENEVE_PORT_RANGE]);
		if (ntohs(p->high) < ntohs(p->low)) {
			NL_SET_ERR_MSG_ATTR(extack, data[IFLA_GENEVE_PORT_RANGE],
					    "Invalid source port range");
			return -EINVAL;
		}
	}

	return 0;
}

static struct geneve_dev *geneve_find_dev(struct geneve_net *gn,
					  const struct ip_tunnel_info *info,
					  bool *tun_on_same_port,
					  bool *tun_collect_md)
{
	struct geneve_dev *geneve, *t = NULL;

	*tun_on_same_port = false;
	*tun_collect_md = false;
	list_for_each_entry(geneve, &gn->geneve_list, next) {
		if (info->key.tp_dst == geneve->cfg.info.key.tp_dst) {
			*tun_collect_md = geneve->cfg.collect_md;
			*tun_on_same_port = true;
		}
		if (info->key.tun_id == geneve->cfg.info.key.tun_id &&
		    info->key.tp_dst == geneve->cfg.info.key.tp_dst &&
		    !memcmp(&info->key.u, &geneve->cfg.info.key.u, sizeof(info->key.u)))
			t = geneve;
	}
	return t;
}

static bool is_tnl_info_zero(const struct ip_tunnel_info *info)
{
	return !(info->key.tun_id || info->key.tos ||
		 !ip_tunnel_flags_empty(info->key.tun_flags) ||
		 info->key.ttl || info->key.label || info->key.tp_src ||
		 memchr_inv(&info->key.u, 0, sizeof(info->key.u)));
}

static bool geneve_dst_addr_equal(struct ip_tunnel_info *a,
				  struct ip_tunnel_info *b)
{
	if (ip_tunnel_info_af(a) == AF_INET)
		return a->key.u.ipv4.dst == b->key.u.ipv4.dst;
	else
		return ipv6_addr_equal(&a->key.u.ipv6.dst, &b->key.u.ipv6.dst);
}

static int geneve_configure(struct net *net, struct net_device *dev,
			    struct netlink_ext_ack *extack,
			    const struct geneve_config *cfg)
{
	struct geneve_net *gn = net_generic(net, geneve_net_id);
	struct geneve_dev *t, *geneve = netdev_priv(dev);
	const struct ip_tunnel_info *info = &cfg->info;
	bool tun_collect_md, tun_on_same_port;
	int err, encap_len;

	if (cfg->collect_md && !is_tnl_info_zero(info)) {
		NL_SET_ERR_MSG(extack,
			       "Device is externally controlled, so attributes (VNI, Port, and so on) must not be specified");
		return -EINVAL;
	}

	geneve->net = net;
	geneve->dev = dev;

	t = geneve_find_dev(gn, info, &tun_on_same_port, &tun_collect_md);
	if (t)
		return -EBUSY;

	/* make enough headroom for basic scenario */
	encap_len = GENEVE_BASE_HLEN + ETH_HLEN;
	if (!cfg->collect_md && ip_tunnel_info_af(info) == AF_INET) {
		encap_len += sizeof(struct iphdr);
		dev->max_mtu -= sizeof(struct iphdr);
	} else {
		encap_len += sizeof(struct ipv6hdr);
		dev->max_mtu -= sizeof(struct ipv6hdr);
	}
	dev->needed_headroom = encap_len + ETH_HLEN;

	if (cfg->collect_md) {
		if (tun_on_same_port) {
			NL_SET_ERR_MSG(extack,
				       "There can be only one externally controlled device on a destination port");
			return -EPERM;
		}
	} else {
		if (tun_collect_md) {
			NL_SET_ERR_MSG(extack,
				       "There already exists an externally controlled device on this destination port");
			return -EPERM;
		}
	}

	dst_cache_reset(&geneve->cfg.info.dst_cache);
	memcpy(&geneve->cfg, cfg, sizeof(*cfg));

	if (geneve->cfg.inner_proto_inherit) {
		dev->header_ops = NULL;
		dev->type = ARPHRD_NONE;
		dev->hard_header_len = 0;
		dev->addr_len = 0;
		dev->flags = IFF_POINTOPOINT | IFF_NOARP;
	}

	err = register_netdevice(dev);
	if (err)
		return err;

	list_add(&geneve->next, &gn->geneve_list);
	return 0;
}

static void init_tnl_info(struct ip_tunnel_info *info, __u16 dst_port)
{
	memset(info, 0, sizeof(*info));
	info->key.tp_dst = htons(dst_port);
}

static int geneve_nl2info(struct nlattr *tb[], struct nlattr *data[],
			  struct netlink_ext_ack *extack,
			  struct geneve_config *cfg, bool changelink)
{
	struct ip_tunnel_info *info = &cfg->info;
	int attrtype;

	if (data[IFLA_GENEVE_REMOTE] && data[IFLA_GENEVE_REMOTE6]) {
		NL_SET_ERR_MSG(extack,
			       "Cannot specify both IPv4 and IPv6 Remote addresses");
		return -EINVAL;
	}

	if (data[IFLA_GENEVE_REMOTE]) {
		if (changelink && (ip_tunnel_info_af(info) == AF_INET6)) {
			attrtype = IFLA_GENEVE_REMOTE;
			goto change_notsup;
		}

		info->key.u.ipv4.dst =
			nla_get_in_addr(data[IFLA_GENEVE_REMOTE]);

		if (ipv4_is_multicast(info->key.u.ipv4.dst)) {
			NL_SET_ERR_MSG_ATTR(extack, data[IFLA_GENEVE_REMOTE],
					    "Remote IPv4 address cannot be Multicast");
			return -EINVAL;
		}
	}

	if (data[IFLA_GENEVE_REMOTE6]) {
#if IS_ENABLED(CONFIG_IPV6)
		if (changelink && (ip_tunnel_info_af(info) == AF_INET)) {
			attrtype = IFLA_GENEVE_REMOTE6;
			goto change_notsup;
		}

		info->mode = IP_TUNNEL_INFO_IPV6;
		info->key.u.ipv6.dst =
			nla_get_in6_addr(data[IFLA_GENEVE_REMOTE6]);

		if (ipv6_addr_type(&info->key.u.ipv6.dst) &
		    IPV6_ADDR_LINKLOCAL) {
			NL_SET_ERR_MSG_ATTR(extack, data[IFLA_GENEVE_REMOTE6],
					    "Remote IPv6 address cannot be link-local");
			return -EINVAL;
		}
		if (ipv6_addr_is_multicast(&info->key.u.ipv6.dst)) {
			NL_SET_ERR_MSG_ATTR(extack, data[IFLA_GENEVE_REMOTE6],
					    "Remote IPv6 address cannot be Multicast");
			return -EINVAL;
		}
		__set_bit(IP_TUNNEL_CSUM_BIT, info->key.tun_flags);
		cfg->use_udp6_rx_checksums = true;
#else
		NL_SET_ERR_MSG_ATTR(extack, data[IFLA_GENEVE_REMOTE6],
				    "IPv6 support not enabled in the kernel");
		return -EPFNOSUPPORT;
#endif
	}

	if (data[IFLA_GENEVE_ID]) {
		__u32 vni;
		__u8 tvni[3];
		__be64 tunid;

		vni = nla_get_u32(data[IFLA_GENEVE_ID]);
		tvni[0] = (vni & 0x00ff0000) >> 16;
		tvni[1] = (vni & 0x0000ff00) >> 8;
		tvni[2] =  vni & 0x000000ff;

		tunid = vni_to_tunnel_id(tvni);
		if (changelink && (tunid != info->key.tun_id)) {
			attrtype = IFLA_GENEVE_ID;
			goto change_notsup;
		}
		info->key.tun_id = tunid;
	}

	if (data[IFLA_GENEVE_TTL_INHERIT]) {
		if (nla_get_u8(data[IFLA_GENEVE_TTL_INHERIT]))
			cfg->ttl_inherit = true;
		else
			cfg->ttl_inherit = false;
	} else if (data[IFLA_GENEVE_TTL]) {
		info->key.ttl = nla_get_u8(data[IFLA_GENEVE_TTL]);
		cfg->ttl_inherit = false;
	}

	if (data[IFLA_GENEVE_TOS])
		info->key.tos = nla_get_u8(data[IFLA_GENEVE_TOS]);

	if (data[IFLA_GENEVE_DF])
		cfg->df = nla_get_u8(data[IFLA_GENEVE_DF]);

	if (data[IFLA_GENEVE_LABEL]) {
		info->key.label = nla_get_be32(data[IFLA_GENEVE_LABEL]) &
				  IPV6_FLOWLABEL_MASK;
		if (info->key.label && (!(info->mode & IP_TUNNEL_INFO_IPV6))) {
			NL_SET_ERR_MSG_ATTR(extack, data[IFLA_GENEVE_LABEL],
					    "Label attribute only applies for IPv6 Geneve devices");
			return -EINVAL;
		}
	}

	if (data[IFLA_GENEVE_PORT]) {
		if (changelink) {
			attrtype = IFLA_GENEVE_PORT;
			goto change_notsup;
		}
		info->key.tp_dst = nla_get_be16(data[IFLA_GENEVE_PORT]);
	}

	if (data[IFLA_GENEVE_PORT_RANGE]) {
		const struct ifla_geneve_port_range *p;

		if (changelink) {
			attrtype = IFLA_GENEVE_PORT_RANGE;
			goto change_notsup;
		}
		p = nla_data(data[IFLA_GENEVE_PORT_RANGE]);
		cfg->port_min = ntohs(p->low);
		cfg->port_max = ntohs(p->high);
	}

	if (data[IFLA_GENEVE_COLLECT_METADATA]) {
		if (changelink) {
			attrtype = IFLA_GENEVE_COLLECT_METADATA;
			goto change_notsup;
		}
		cfg->collect_md = true;
	}

	if (data[IFLA_GENEVE_UDP_CSUM]) {
		if (changelink) {
			attrtype = IFLA_GENEVE_UDP_CSUM;
			goto change_notsup;
		}
		if (nla_get_u8(data[IFLA_GENEVE_UDP_CSUM]))
			__set_bit(IP_TUNNEL_CSUM_BIT, info->key.tun_flags);
	}

	if (data[IFLA_GENEVE_UDP_ZERO_CSUM6_TX]) {
#if IS_ENABLED(CONFIG_IPV6)
		if (changelink) {
			attrtype = IFLA_GENEVE_UDP_ZERO_CSUM6_TX;
			goto change_notsup;
		}
		if (nla_get_u8(data[IFLA_GENEVE_UDP_ZERO_CSUM6_TX]))
			__clear_bit(IP_TUNNEL_CSUM_BIT, info->key.tun_flags);
#else
		NL_SET_ERR_MSG_ATTR(extack, data[IFLA_GENEVE_UDP_ZERO_CSUM6_TX],
				    "IPv6 support not enabled in the kernel");
		return -EPFNOSUPPORT;
#endif
	}

	if (data[IFLA_GENEVE_UDP_ZERO_CSUM6_RX]) {
#if IS_ENABLED(CONFIG_IPV6)
		if (changelink) {
			attrtype = IFLA_GENEVE_UDP_ZERO_CSUM6_RX;
			goto change_notsup;
		}
		if (nla_get_u8(data[IFLA_GENEVE_UDP_ZERO_CSUM6_RX]))
			cfg->use_udp6_rx_checksums = false;
#else
		NL_SET_ERR_MSG_ATTR(extack, data[IFLA_GENEVE_UDP_ZERO_CSUM6_RX],
				    "IPv6 support not enabled in the kernel");
		return -EPFNOSUPPORT;
#endif
	}

	if (data[IFLA_GENEVE_INNER_PROTO_INHERIT]) {
		if (changelink) {
			attrtype = IFLA_GENEVE_INNER_PROTO_INHERIT;
			goto change_notsup;
		}
		cfg->inner_proto_inherit = true;
	}

	return 0;
change_notsup:
	NL_SET_ERR_MSG_ATTR(extack, data[attrtype],
			    "Changing VNI, Port, endpoint IP address family, external, inner_proto_inherit, and UDP checksum attributes are not supported");
	return -EOPNOTSUPP;
}

static void geneve_link_config(struct net_device *dev,
			       struct ip_tunnel_info *info, struct nlattr *tb[])
{
	struct geneve_dev *geneve = netdev_priv(dev);
	int ldev_mtu = 0;

	if (tb[IFLA_MTU]) {
		geneve_change_mtu(dev, nla_get_u32(tb[IFLA_MTU]));
		return;
	}

	switch (ip_tunnel_info_af(info)) {
	case AF_INET: {
		struct flowi4 fl4 = { .daddr = info->key.u.ipv4.dst };
		struct rtable *rt = ip_route_output_key(geneve->net, &fl4);

		if (!IS_ERR(rt) && rt->dst.dev) {
			ldev_mtu = rt->dst.dev->mtu - GENEVE_IPV4_HLEN;
			ip_rt_put(rt);
		}
		break;
	}
#if IS_ENABLED(CONFIG_IPV6)
	case AF_INET6: {
		struct rt6_info *rt;

		if (!__in6_dev_get(dev))
			break;

		rt = rt6_lookup(geneve->net, &info->key.u.ipv6.dst, NULL, 0,
				NULL, 0);

		if (rt && rt->dst.dev)
			ldev_mtu = rt->dst.dev->mtu - GENEVE_IPV6_HLEN;
		ip6_rt_put(rt);
		break;
	}
#endif
	}

	if (ldev_mtu <= 0)
		return;

	geneve_change_mtu(dev, ldev_mtu - info->options_len);
}

static int geneve_newlink(struct net_device *dev,
			  struct rtnl_newlink_params *params,
			  struct netlink_ext_ack *extack)
{
	struct net *link_net = rtnl_newlink_link_net(params);
	struct nlattr **data = params->data;
	struct nlattr **tb = params->tb;
	struct geneve_config cfg = {
		.df = GENEVE_DF_UNSET,
		.use_udp6_rx_checksums = false,
		.ttl_inherit = false,
		.collect_md = false,
		.port_min = 1,
		.port_max = USHRT_MAX,
	};
	int err;

	init_tnl_info(&cfg.info, GENEVE_UDP_PORT);
	err = geneve_nl2info(tb, data, extack, &cfg, false);
	if (err)
		return err;

	err = geneve_configure(link_net, dev, extack, &cfg);
	if (err)
		return err;

	geneve_link_config(dev, &cfg.info, tb);

	return 0;
}

/* Quiesces the geneve device data path for both TX and RX.
 *
 * On transmit geneve checks for non-NULL geneve_sock before it proceeds.
 * So, if we set that socket to NULL under RCU and wait for synchronize_net()
 * to complete for the existing set of in-flight packets to be transmitted,
 * then we would have quiesced the transmit data path. All the future packets
 * will get dropped until we unquiesce the data path.
 *
 * On receive geneve dereference the geneve_sock stashed in the socket. So,
 * if we set that to NULL under RCU and wait for synchronize_net() to
 * complete, then we would have quiesced the receive data path.
 */
static void geneve_quiesce(struct geneve_dev *geneve, struct geneve_sock **gs4,
			   struct geneve_sock **gs6)
{
	*gs4 = rtnl_dereference(geneve->sock4);
	rcu_assign_pointer(geneve->sock4, NULL);
	if (*gs4)
		rcu_assign_sk_user_data((*gs4)->sock->sk, NULL);
#if IS_ENABLED(CONFIG_IPV6)
	*gs6 = rtnl_dereference(geneve->sock6);
	rcu_assign_pointer(geneve->sock6, NULL);
	if (*gs6)
		rcu_assign_sk_user_data((*gs6)->sock->sk, NULL);
#else
	*gs6 = NULL;
#endif
	synchronize_net();
}

/* Resumes the geneve device data path for both TX and RX. */
static void geneve_unquiesce(struct geneve_dev *geneve, struct geneve_sock *gs4,
			     struct geneve_sock __maybe_unused *gs6)
{
	rcu_assign_pointer(geneve->sock4, gs4);
	if (gs4)
		rcu_assign_sk_user_data(gs4->sock->sk, gs4);
#if IS_ENABLED(CONFIG_IPV6)
	rcu_assign_pointer(geneve->sock6, gs6);
	if (gs6)
		rcu_assign_sk_user_data(gs6->sock->sk, gs6);
#endif
	synchronize_net();
}

static int geneve_changelink(struct net_device *dev, struct nlattr *tb[],
			     struct nlattr *data[],
			     struct netlink_ext_ack *extack)
{
	struct geneve_dev *geneve = netdev_priv(dev);
	struct geneve_sock *gs4, *gs6;
	struct geneve_config cfg;
	int err;

	/* If the geneve device is configured for metadata (or externally
	 * controlled, for example, OVS), then nothing can be changed.
	 */
	if (geneve->cfg.collect_md)
		return -EOPNOTSUPP;

	/* Start with the existing info. */
	memcpy(&cfg, &geneve->cfg, sizeof(cfg));
	err = geneve_nl2info(tb, data, extack, &cfg, true);
	if (err)
		return err;

	if (!geneve_dst_addr_equal(&geneve->cfg.info, &cfg.info)) {
		dst_cache_reset(&cfg.info.dst_cache);
		geneve_link_config(dev, &cfg.info, tb);
	}

	geneve_quiesce(geneve, &gs4, &gs6);
	memcpy(&geneve->cfg, &cfg, sizeof(cfg));
	geneve_unquiesce(geneve, gs4, gs6);

	return 0;
}

static void geneve_dellink(struct net_device *dev, struct list_head *head)
{
	struct geneve_dev *geneve = netdev_priv(dev);

	list_del(&geneve->next);
	unregister_netdevice_queue(dev, head);
}

static size_t geneve_get_size(const struct net_device *dev)
{
	return nla_total_size(sizeof(__u32)) +	/* IFLA_GENEVE_ID */
		nla_total_size(sizeof(struct in6_addr)) + /* IFLA_GENEVE_REMOTE{6} */
		nla_total_size(sizeof(__u8)) +  /* IFLA_GENEVE_TTL */
		nla_total_size(sizeof(__u8)) +  /* IFLA_GENEVE_TOS */
		nla_total_size(sizeof(__u8)) +	/* IFLA_GENEVE_DF */
		nla_total_size(sizeof(__be32)) +  /* IFLA_GENEVE_LABEL */
		nla_total_size(sizeof(__be16)) +  /* IFLA_GENEVE_PORT */
		nla_total_size(0) +	 /* IFLA_GENEVE_COLLECT_METADATA */
		nla_total_size(sizeof(__u8)) + /* IFLA_GENEVE_UDP_CSUM */
		nla_total_size(sizeof(__u8)) + /* IFLA_GENEVE_UDP_ZERO_CSUM6_TX */
		nla_total_size(sizeof(__u8)) + /* IFLA_GENEVE_UDP_ZERO_CSUM6_RX */
		nla_total_size(sizeof(__u8)) + /* IFLA_GENEVE_TTL_INHERIT */
		nla_total_size(0) +	 /* IFLA_GENEVE_INNER_PROTO_INHERIT */
		nla_total_size(sizeof(struct ifla_geneve_port_range)) + /* IFLA_GENEVE_PORT_RANGE */
		0;
}

static int geneve_fill_info(struct sk_buff *skb, const struct net_device *dev)
{
	struct geneve_dev *geneve = netdev_priv(dev);
	struct ip_tunnel_info *info = &geneve->cfg.info;
	bool ttl_inherit = geneve->cfg.ttl_inherit;
	bool metadata = geneve->cfg.collect_md;
	struct ifla_geneve_port_range ports = {
		.low	= htons(geneve->cfg.port_min),
		.high	= htons(geneve->cfg.port_max),
	};
	__u8 tmp_vni[3];
	__u32 vni;

	tunnel_id_to_vni(info->key.tun_id, tmp_vni);
	vni = (tmp_vni[0] << 16) | (tmp_vni[1] << 8) | tmp_vni[2];
	if (nla_put_u32(skb, IFLA_GENEVE_ID, vni))
		goto nla_put_failure;

	if (!metadata && ip_tunnel_info_af(info) == AF_INET) {
		if (nla_put_in_addr(skb, IFLA_GENEVE_REMOTE,
				    info->key.u.ipv4.dst))
			goto nla_put_failure;
		if (nla_put_u8(skb, IFLA_GENEVE_UDP_CSUM,
			       test_bit(IP_TUNNEL_CSUM_BIT,
					info->key.tun_flags)))
			goto nla_put_failure;

#if IS_ENABLED(CONFIG_IPV6)
	} else if (!metadata) {
		if (nla_put_in6_addr(skb, IFLA_GENEVE_REMOTE6,
				     &info->key.u.ipv6.dst))
			goto nla_put_failure;
		if (nla_put_u8(skb, IFLA_GENEVE_UDP_ZERO_CSUM6_TX,
			       !test_bit(IP_TUNNEL_CSUM_BIT,
					 info->key.tun_flags)))
			goto nla_put_failure;
#endif
	}

	if (nla_put_u8(skb, IFLA_GENEVE_TTL, info->key.ttl) ||
	    nla_put_u8(skb, IFLA_GENEVE_TOS, info->key.tos) ||
	    nla_put_be32(skb, IFLA_GENEVE_LABEL, info->key.label))
		goto nla_put_failure;

	if (nla_put_u8(skb, IFLA_GENEVE_DF, geneve->cfg.df))
		goto nla_put_failure;

	if (nla_put_be16(skb, IFLA_GENEVE_PORT, info->key.tp_dst))
		goto nla_put_failure;

	if (metadata && nla_put_flag(skb, IFLA_GENEVE_COLLECT_METADATA))
		goto nla_put_failure;

#if IS_ENABLED(CONFIG_IPV6)
	if (nla_put_u8(skb, IFLA_GENEVE_UDP_ZERO_CSUM6_RX,
		       !geneve->cfg.use_udp6_rx_checksums))
		goto nla_put_failure;
#endif

	if (nla_put_u8(skb, IFLA_GENEVE_TTL_INHERIT, ttl_inherit))
		goto nla_put_failure;

	if (geneve->cfg.inner_proto_inherit &&
	    nla_put_flag(skb, IFLA_GENEVE_INNER_PROTO_INHERIT))
		goto nla_put_failure;

	if (nla_put(skb, IFLA_GENEVE_PORT_RANGE, sizeof(ports), &ports))
		goto nla_put_failure;

	return 0;

nla_put_failure:
	return -EMSGSIZE;
}

static struct rtnl_link_ops geneve_link_ops __read_mostly = {
	.kind		= "geneve",
	.maxtype	= IFLA_GENEVE_MAX,
	.policy		= geneve_policy,
	.priv_size	= sizeof(struct geneve_dev),
	.setup		= geneve_setup,
	.validate	= geneve_validate,
	.newlink	= geneve_newlink,
	.changelink	= geneve_changelink,
	.dellink	= geneve_dellink,
	.get_size	= geneve_get_size,
	.fill_info	= geneve_fill_info,
};

struct net_device *geneve_dev_create_fb(struct net *net, const char *name,
					u8 name_assign_type, u16 dst_port)
{
	struct nlattr *tb[IFLA_MAX + 1];
	struct net_device *dev;
	LIST_HEAD(list_kill);
	int err;
	struct geneve_config cfg = {
		.df = GENEVE_DF_UNSET,
		.use_udp6_rx_checksums = true,
		.ttl_inherit = false,
		.collect_md = true,
		.port_min = 1,
		.port_max = USHRT_MAX,
	};

	memset(tb, 0, sizeof(tb));
	dev = rtnl_create_link(net, name, name_assign_type,
			       &geneve_link_ops, tb, NULL);
	if (IS_ERR(dev))
		return dev;

	init_tnl_info(&cfg.info, dst_port);
	err = geneve_configure(net, dev, NULL, &cfg);
	if (err) {
		free_netdev(dev);
		return ERR_PTR(err);
	}

	/* openvswitch users expect packet sizes to be unrestricted,
	 * so set the largest MTU we can.
	 */
	err = geneve_change_mtu(dev, IP_MAX_MTU);
	if (err)
		goto err;

	err = rtnl_configure_link(dev, NULL, 0, NULL);
	if (err < 0)
		goto err;

	return dev;
err:
	geneve_dellink(dev, &list_kill);
	unregister_netdevice_many(&list_kill);
	return ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(geneve_dev_create_fb);

static int geneve_netdevice_event(struct notifier_block *unused,
				  unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	if (event == NETDEV_UDP_TUNNEL_PUSH_INFO)
		geneve_offload_rx_ports(dev, true);
	else if (event == NETDEV_UDP_TUNNEL_DROP_INFO)
		geneve_offload_rx_ports(dev, false);

	return NOTIFY_DONE;
}

static struct notifier_block geneve_notifier_block __read_mostly = {
	.notifier_call = geneve_netdevice_event,
};

static __net_init int geneve_init_net(struct net *net)
{
	struct geneve_net *gn = net_generic(net, geneve_net_id);

	INIT_LIST_HEAD(&gn->geneve_list);
	INIT_LIST_HEAD(&gn->sock_list);
	return 0;
}

static void __net_exit geneve_exit_rtnl_net(struct net *net,
					    struct list_head *dev_to_kill)
{
	struct geneve_net *gn = net_generic(net, geneve_net_id);
	struct geneve_dev *geneve, *next;

	list_for_each_entry_safe(geneve, next, &gn->geneve_list, next)
		geneve_dellink(geneve->dev, dev_to_kill);
}

static void __net_exit geneve_exit_net(struct net *net)
{
	const struct geneve_net *gn = net_generic(net, geneve_net_id);

	WARN_ON_ONCE(!list_empty(&gn->sock_list));
}

static struct pernet_operations geneve_net_ops = {
	.init = geneve_init_net,
	.exit_rtnl = geneve_exit_rtnl_net,
	.exit = geneve_exit_net,
	.id   = &geneve_net_id,
	.size = sizeof(struct geneve_net),
};

static int __init geneve_init_module(void)
{
	int rc;

	rc = register_pernet_subsys(&geneve_net_ops);
	if (rc)
		goto out1;

	rc = register_netdevice_notifier(&geneve_notifier_block);
	if (rc)
		goto out2;

	rc = rtnl_link_register(&geneve_link_ops);
	if (rc)
		goto out3;

	return 0;
out3:
	unregister_netdevice_notifier(&geneve_notifier_block);
out2:
	unregister_pernet_subsys(&geneve_net_ops);
out1:
	return rc;
}
late_initcall(geneve_init_module);

static void __exit geneve_cleanup_module(void)
{
	rtnl_link_unregister(&geneve_link_ops);
	unregister_netdevice_notifier(&geneve_notifier_block);
	unregister_pernet_subsys(&geneve_net_ops);
}
module_exit(geneve_cleanup_module);

MODULE_LICENSE("GPL");
MODULE_VERSION(GENEVE_NETDEV_VER);
MODULE_AUTHOR("John W. Linville <linville@tuxdriver.com>");
MODULE_DESCRIPTION("Interface driver for GENEVE encapsulated traffic");
MODULE_ALIAS_RTNL_LINK("geneve");
