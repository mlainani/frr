/* NHRP routing functions
 * Copyright (c) 2014-2015 Timo Teräs
 *
 * This file is free software: you may copy, redistribute and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "nhrpd.h"
#include "table.h"
#include "memory.h"
#include "stream.h"
#include "log.h"
#include "zclient.h"

DEFINE_MTYPE_STATIC(NHRPD, NHRP_ROUTE, "NHRP routing entry")

static struct zclient *zclient;
static struct route_table *zebra_rib[AFI_MAX];

struct route_info {
	union sockunion via;
	struct interface *ifp;
	struct interface *nhrp_ifp;
};

static struct route_node *nhrp_route_update_get(const struct prefix *p,
						int create)
{
	struct route_node *rn;
	afi_t afi = family2afi(PREFIX_FAMILY(p));

	if (!zebra_rib[afi])
		return NULL;

	if (create) {
		rn = route_node_get(zebra_rib[afi], p);
		if (!rn->info) {
			rn->info = XCALLOC(MTYPE_NHRP_ROUTE,
					   sizeof(struct route_info));
			route_lock_node(rn);
		}
		return rn;
	} else {
		return route_node_lookup(zebra_rib[afi], p);
	}
}

static void nhrp_route_update_put(struct route_node *rn)
{
	struct route_info *ri = rn->info;

	if (!ri->ifp && !ri->nhrp_ifp
	    && sockunion_family(&ri->via) == AF_UNSPEC) {
		XFREE(MTYPE_NHRP_ROUTE, rn->info);
		route_unlock_node(rn);
	}
	route_unlock_node(rn);
}

static void nhrp_route_update_zebra(const struct prefix *p,
				    union sockunion *nexthop,
				    struct interface *ifp)
{
	struct route_node *rn;
	struct route_info *ri;

	rn = nhrp_route_update_get(
		p, (sockunion_family(nexthop) != AF_UNSPEC) || ifp);
	if (rn) {
		ri = rn->info;
		ri->via = *nexthop;
		ri->ifp = ifp;
		nhrp_route_update_put(rn);
	}
}

void nhrp_route_update_nhrp(const struct prefix *p, struct interface *ifp)
{
	struct route_node *rn;
	struct route_info *ri;

	rn = nhrp_route_update_get(p, ifp != NULL);
	if (rn) {
		ri = rn->info;
		ri->nhrp_ifp = ifp;
		nhrp_route_update_put(rn);
	}
}

void nhrp_route_announce(int add, enum nhrp_cache_type type,
			 const struct prefix *p, struct interface *ifp,
			 const union sockunion *nexthop, uint32_t mtu)
{
	struct zapi_route api;
	struct zapi_nexthop *api_nh;
	union sockunion *nexthop_ref = (union sockunion *)nexthop;

	if (zclient->sock < 0)
		return;

	memset(&api, 0, sizeof(api));
	api.type = ZEBRA_ROUTE_NHRP;
	api.safi = SAFI_UNICAST;
	api.vrf_id = VRF_DEFAULT;
	api.prefix = *p;

	switch (type) {
	case NHRP_CACHE_NEGATIVE:
		zapi_route_set_blackhole(&api, BLACKHOLE_REJECT);
		ifp = NULL;
		nexthop = NULL;
		break;
	case NHRP_CACHE_DYNAMIC:
	case NHRP_CACHE_NHS:
	case NHRP_CACHE_STATIC:
		/* Regular route, so these are announced
		 * to other routing daemons */
		break;
	default:
		SET_FLAG(api.flags, ZEBRA_FLAG_FIB_OVERRIDE);
		break;
	}
	SET_FLAG(api.flags, ZEBRA_FLAG_ALLOW_RECURSION);

	SET_FLAG(api.message, ZAPI_MESSAGE_NEXTHOP);
	api.nexthop_num = 1;
	api_nh = &api.nexthops[0];
	api_nh->vrf_id = VRF_DEFAULT;

	switch (api.prefix.family) {
	case AF_INET:
		if (api.prefix.prefixlen == IPV4_MAX_BITLEN &&
		    nexthop_ref &&
		    memcmp(&nexthop_ref->sin.sin_addr, &api.prefix.u.prefix4,
			   sizeof(struct in_addr)) == 0) {
			nexthop_ref = NULL;
		}
		if (nexthop_ref) {
			api_nh->gate.ipv4 = nexthop_ref->sin.sin_addr;
			api_nh->type = NEXTHOP_TYPE_IPV4;
		}
		if (ifp) {
			api_nh->ifindex = ifp->ifindex;
			if (api_nh->type == NEXTHOP_TYPE_IPV4)
				api_nh->type = NEXTHOP_TYPE_IPV4_IFINDEX;
			else
				api_nh->type = NEXTHOP_TYPE_IFINDEX;
		}
		break;
	case AF_INET6:
		if (api.prefix.prefixlen == IPV6_MAX_BITLEN &&
		    nexthop_ref &&
		    memcmp(&nexthop_ref->sin6.sin6_addr, &api.prefix.u.prefix6,
			   sizeof(struct in6_addr)) == 0) {
			nexthop_ref = NULL;
		}
		if (nexthop_ref) {
			api_nh->gate.ipv6 = nexthop_ref->sin6.sin6_addr;
			api_nh->type = NEXTHOP_TYPE_IPV6;
		}
		if (ifp) {
			api_nh->ifindex = ifp->ifindex;
			if (api_nh->type == NEXTHOP_TYPE_IPV6)
				api_nh->type = NEXTHOP_TYPE_IPV6_IFINDEX;
			else
				api_nh->type = NEXTHOP_TYPE_IFINDEX;
		}
		break;
	}
	if (mtu) {
		SET_FLAG(api.message, ZAPI_MESSAGE_MTU);
		api.mtu = mtu;
	}

	if (unlikely(debug_flags & NHRP_DEBUG_ROUTE)) {
		char buf[2][PREFIX_STRLEN];

		prefix2str(&api.prefix, buf[0], sizeof(buf[0]));
		zlog_debug(
			"Zebra send: route %s %s nexthop %s metric %u count %d dev %s",
			add ? "add" : "del", buf[0],
			nexthop_ref ? inet_ntop(api.prefix.family,
						&api_nh->gate,
						buf[1], sizeof(buf[1]))
			: "<onlink>",
			api.metric, api.nexthop_num, ifp ? ifp->name : "none");
	}

	zclient_route_send(add ? ZEBRA_ROUTE_ADD : ZEBRA_ROUTE_DELETE, zclient,
			   &api);
}

int nhrp_route_read(ZAPI_CALLBACK_ARGS)
{
	struct zapi_route api;
	struct zapi_nexthop *api_nh;
	struct interface *ifp = NULL;
	union sockunion nexthop_addr;
	char buf[2][PREFIX_STRLEN];
	int added;

	if (zapi_route_decode(zclient->ibuf, &api) < 0)
		return -1;

	/* we completely ignore srcdest routes for now. */
	if (CHECK_FLAG(api.message, ZAPI_MESSAGE_SRCPFX))
		return 0;

	/* ignore our routes */
	if (api.type == ZEBRA_ROUTE_NHRP)
		return 0;

	/*
	 * Ignore policy-based routing routes. A default IPv6 route added to a
	 * routing table other than main will interfere with our custom NBMA
	 * interface IPv6 address selection.
	 */
	if (api.type == ZEBRA_ROUTE_PBR)
		return 0;

	sockunion_family(&nexthop_addr) = AF_UNSPEC;
	if (CHECK_FLAG(api.message, ZAPI_MESSAGE_NEXTHOP)) {
		api_nh = &api.nexthops[0];

		nexthop_addr.sa.sa_family = api.prefix.family;
		switch (nexthop_addr.sa.sa_family) {
		case AF_INET:
			nexthop_addr.sin.sin_addr = api_nh->gate.ipv4;
			break;
		case AF_INET6:
			nexthop_addr.sin6.sin6_addr = api_nh->gate.ipv6;
			break;
		}

		if (api_nh->ifindex != IFINDEX_INTERNAL)
			ifp = if_lookup_by_index(api_nh->ifindex, VRF_DEFAULT);
	}

	added = (cmd == ZEBRA_REDISTRIBUTE_ROUTE_ADD);
	debugf(NHRP_DEBUG_ROUTE, "if-route-%s: %s via %s dev %s",
	       added ? "add" : "del",
	       prefix2str(&api.prefix, buf[0], sizeof(buf[0])),
	       sockunion2str(&nexthop_addr, buf[1], sizeof(buf[1])),
	       ifp ? ifp->name : "(none)");

	nhrp_route_update_zebra(&api.prefix, &nexthop_addr, ifp);
	nhrp_shortcut_prefix_change(&api.prefix, !added);

	if (added && (api.prefix.family == AF_INET6) &&
	    sockunion_family(&nexthop_addr) != AF_UNSPEC && ifp) {
		struct prefix nexthop_prefix = {0};
		struct listnode *cnode;
		struct connected *c, *default_gw_iface_prefix;
		char prfx_buf[2][PREFIX_STRLEN] = {{0}, {0}};
		union sockunion default_gw_iface_addr;
		struct nhrp_interface *nifp = ifp->info;
		struct nhrp_afi_data *if_ad = &nifp->afi[AFI_IP6];
		struct nhrp_cache *nc;

		if (is_default_prefix(&api.prefix)) {
			debugf(NHRP_DEBUG_ROUTE, "IPv6 default route added via %s",
			       ifp->name);
			sockunion2hostprefix(&nexthop_addr, &nexthop_prefix);
		}
		else if (!memcmp(&nexthop_addr.sin6.sin6_addr,
				 &in6addr_any, sizeof(struct in6_addr))) {
			struct prefix p = {0};
			struct route_node *rn;
			struct route_info *ri;

			debugf(NHRP_DEBUG_ROUTE, "on-link route added via %s",
			       ifp->name);

			if (!zebra_rib[AFI_IP6])
				goto out;

			p.family = AF_INET6;
			rn = route_node_lookup(zebra_rib[AFI_IP6], &p);
			if (!rn)
				goto out;

			ri = rn->info;

			debugf(NHRP_DEBUG_ROUTE, "current IPv6 dflt gw %s via %s",
			       sockunion2str(&ri->via, buf[0], sizeof(buf[0])),
			       ri->ifp->name);

			if (ifp != ri->ifp)
			  goto out;

			sockunion2hostprefix(&ri->via, &nexthop_prefix);
		}

		default_gw_iface_prefix = NULL;
		for (ALL_LIST_ELEMENTS_RO(ifp->connected, cnode, c)) {
			if (PREFIX_FAMILY(c->address) != AF_INET6)
				continue;

			if (IN6_IS_ADDR_LINKLOCAL(&c->address->u.prefix6))
				continue;

			if (!prefix_match(c->address, &nexthop_prefix))
				continue;

			default_gw_iface_prefix = c;
			break;
		}

		if (!default_gw_iface_prefix)
			goto out;

		debugf(NHRP_DEBUG_ROUTE, "%s: prefix %s includes dflt gw addr %s",
		       ifp->name,
		       prefix2str(default_gw_iface_prefix->address, prfx_buf[0],
				  sizeof(prfx_buf[0])),
		       prefix2str(&nexthop_prefix, prfx_buf[1],
				  sizeof(prfx_buf[1])));

		prefix2sockunion(default_gw_iface_prefix->address,
				 &default_gw_iface_addr);

		if (sockunion_same(&default_gw_iface_addr, &if_ad->addr)) {
			debugf(NHRP_DEBUG_ROUTE, "%s: already using %s as the primary IPv6 address",
			       ifp->name,
			       prefix2str(default_gw_iface_prefix->address, prfx_buf[0],
					  sizeof(prfx_buf[0])));
			goto out;
		}

		debugf(NHRP_DEBUG_ROUTE, "%s: using %s as the primary IPv6 address (was %s)",
		       ifp->name,
		       prefix2str(default_gw_iface_prefix->address, prfx_buf[0], sizeof(prfx_buf[0])),
		       sockunion2str(&if_ad->addr, prfx_buf[1], sizeof(prfx_buf[1])));

		if (sockunion_family(&if_ad->addr) != AF_UNSPEC) {
			nc = nhrp_cache_get(ifp, &if_ad->addr, 0);
			if (nc)
				nhrp_cache_update_binding(nc, NHRP_CACHE_LOCAL, -1,
							  NULL, 0, NULL);
		}

		if_ad->addr = default_gw_iface_addr;

		if (if_ad->configured && sockunion_family(&if_ad->addr) != AF_UNSPEC) {
			nc = nhrp_cache_get(ifp, &default_gw_iface_addr, 1);
			if (nc)
				nhrp_cache_update_binding(nc, NHRP_CACHE_LOCAL, 0, NULL,
							  0, NULL);
		}

		notifier_call(&nifp->notifier_list, NOTIFY_INTERFACE_V6_ADDRESS_CHANGED);
	}
out:

	return 0;
}

int nhrp_route_get_nexthop(const union sockunion *addr, struct prefix *p,
			   union sockunion *via, struct interface **ifp)
{
	struct route_node *rn;
	struct route_info *ri;
	struct prefix lookup;
	afi_t afi = family2afi(sockunion_family(addr));
	char buf[PREFIX_STRLEN];

	sockunion2hostprefix(addr, &lookup);

	rn = route_node_match(zebra_rib[afi], &lookup);
	if (!rn)
		return 0;

	ri = rn->info;
	if (ri->nhrp_ifp) {
		debugf(NHRP_DEBUG_ROUTE, "lookup %s: nhrp_if=%s",
		       prefix2str(&lookup, buf, sizeof(buf)),
		       ri->nhrp_ifp->name);

		if (via)
			sockunion_family(via) = AF_UNSPEC;
		if (ifp)
			*ifp = ri->nhrp_ifp;
	} else {
		debugf(NHRP_DEBUG_ROUTE, "lookup %s: zebra route dev %s",
		       prefix2str(&lookup, buf, sizeof(buf)),
		       ri->ifp ? ri->ifp->name : "(none)");

		if (via)
			*via = ri->via;
		if (ifp)
			*ifp = ri->ifp;
	}
	if (p)
		*p = rn->p;
	route_unlock_node(rn);
	return 1;
}

enum nhrp_route_type nhrp_route_address(struct interface *in_ifp,
					union sockunion *addr, struct prefix *p,
					struct nhrp_peer **peer)
{
	struct interface *ifp = in_ifp;
	struct nhrp_interface *nifp;
	struct nhrp_cache *c;
	union sockunion via[4];
	uint32_t network_id = 0;
	afi_t afi = family2afi(sockunion_family(addr));
	int i;

	if (ifp) {
		nifp = ifp->info;
		network_id = nifp->afi[afi].network_id;

		c = nhrp_cache_get(ifp, addr, 0);
		if (c && c->cur.type == NHRP_CACHE_LOCAL) {
			if (p)
				memset(p, 0, sizeof(*p));
			return NHRP_ROUTE_LOCAL;
		}
	}

	for (i = 0; i < 4; i++) {
		if (!nhrp_route_get_nexthop(addr, p, &via[i], &ifp))
			return NHRP_ROUTE_BLACKHOLE;
		if (ifp) {
			/* Departing from nbma network? */
			nifp = ifp->info;
			if (network_id
			    && network_id != nifp->afi[afi].network_id)
				return NHRP_ROUTE_OFF_NBMA;
		}
		if (sockunion_family(&via[i]) == AF_UNSPEC)
			break;
		/* Resolve via node, but return the prefix of first match */
		addr = &via[i];
		p = NULL;
	}

	if (ifp) {
		c = nhrp_cache_get(ifp, addr, 0);
		if (c && c->cur.type >= NHRP_CACHE_DYNAMIC) {
			if (p)
				memset(p, 0, sizeof(*p));
			if (c->cur.type == NHRP_CACHE_LOCAL)
				return NHRP_ROUTE_LOCAL;
			if (peer)
				*peer = nhrp_peer_ref(c->cur.peer);
			return NHRP_ROUTE_NBMA_NEXTHOP;
		}
	}

	return NHRP_ROUTE_BLACKHOLE;
}

static void nhrp_zebra_connected(struct zclient *zclient)
{
	zclient_send_reg_requests(zclient, VRF_DEFAULT);
	zebra_redistribute_send(ZEBRA_REDISTRIBUTE_ADD, zclient, AFI_IP,
				ZEBRA_ROUTE_ALL, 0, VRF_DEFAULT);
	zebra_redistribute_send(ZEBRA_REDISTRIBUTE_ADD, zclient, AFI_IP6,
				ZEBRA_ROUTE_ALL, 0, VRF_DEFAULT);
}

void nhrp_zebra_init(void)
{
	zebra_rib[AFI_IP] = route_table_init();
	zebra_rib[AFI_IP6] = route_table_init();

	zclient = zclient_new(master, &zclient_options_default);
	zclient->zebra_connected = nhrp_zebra_connected;
	zclient->interface_address_add = nhrp_interface_address_add;
	zclient->interface_address_delete = nhrp_interface_address_delete;
	zclient->redistribute_route_add = nhrp_route_read;
	zclient->redistribute_route_del = nhrp_route_read;

	zclient_init(zclient, ZEBRA_ROUTE_NHRP, 0, &nhrpd_privs);
}

static void nhrp_table_node_cleanup(struct route_table *table,
				    struct route_node *node)
{
	if (!node->info)
		return;

	XFREE(MTYPE_NHRP_ROUTE, node->info);
}

void nhrp_zebra_terminate(void)
{
	zclient_stop(zclient);
	zclient_free(zclient);

	zebra_rib[AFI_IP]->cleanup = nhrp_table_node_cleanup;
	zebra_rib[AFI_IP6]->cleanup = nhrp_table_node_cleanup;
	route_table_finish(zebra_rib[AFI_IP]);
	route_table_finish(zebra_rib[AFI_IP6]);
}
