/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2013 Red Hat, Inc.
 */

#include "nm-default.h"

#include "nm-ndisc.h"

#include <stdlib.h>
#include <arpa/inet.h>

#include "nm-setting-ip6-config.h"

#include "nm-ndisc-private.h"
#include "nm-utils.h"
#include "platform/nm-platform.h"
#include "nm-platform/nmp-netns.h"
#include "nm-l3-config-data.h"

#define _NMLOG_PREFIX_NAME "ndisc"

#define MAX_RTR_SOLICITATION_DELAY_MSEC ((gint64) 1)

/*****************************************************************************/

struct _NMNDiscPrivate {
    /* this *must* be the first field. */
    NMNDiscDataInternal rdata;

    char *   last_error;
    GSource *ra_timeout_source;

    union {
        gint32 solicitations_left;
        gint32 announcements_left;
    };
    union {
        guint send_rs_id;
        guint send_ra_id;
    };
    union {
        gint32 last_rs;
        gint32 last_ra;
    };

    GSource *timeout_expire_source;

    NMUtilsIPv6IfaceId iid;

    /* immutable values: */
    int                           ifindex;
    char *                        ifname;
    char *                        network_id;
    NMSettingIP6ConfigAddrGenMode addr_gen_mode;
    NMUtilsStableType             stable_type;
    guint32                       ra_timeout;
    gint32                        max_addresses;
    gint32                        router_solicitations;
    gint32                        router_solicitation_interval;
    NMNDiscNodeType               node_type;

    NMPlatform *platform;
    NMPNetns *  netns;
};

typedef struct _NMNDiscPrivate NMNDiscPrivate;

NM_GOBJECT_PROPERTIES_DEFINE_BASE(PROP_PLATFORM,
                                  PROP_IFINDEX,
                                  PROP_IFNAME,
                                  PROP_STABLE_TYPE,
                                  PROP_NETWORK_ID,
                                  PROP_ADDR_GEN_MODE,
                                  PROP_MAX_ADDRESSES,
                                  PROP_RA_TIMEOUT,
                                  PROP_ROUTER_SOLICITATIONS,
                                  PROP_ROUTER_SOLICITATION_INTERVAL,
                                  PROP_NODE_TYPE, );

enum { CONFIG_RECEIVED, RA_TIMEOUT_SIGNAL, LAST_SIGNAL };

static guint signals[LAST_SIGNAL] = {0};

G_DEFINE_TYPE(NMNDisc, nm_ndisc, G_TYPE_OBJECT)

#define NM_NDISC_GET_PRIVATE(self) _NM_GET_PRIVATE_PTR(self, NMNDisc, NM_IS_NDISC)

/*****************************************************************************/

static void     _config_changed_log(NMNDisc *ndisc, NMNDiscConfigMap changed);
static gboolean timeout_expire_cb(gpointer user_data);

/*****************************************************************************/

NML3ConfigData *
nm_ndisc_data_to_l3cd(NMDedupMultiIndex *       multi_idx,
                      int                       ifindex,
                      const NMNDiscData *       rdata,
                      NMSettingIP6ConfigPrivacy ip6_privacy,
                      guint32                   route_table,
                      guint32                   route_metric,
                      gboolean                  kernel_support_rta_pref,
                      gboolean                  kernel_support_extended_ifa_flags)
{
    nm_auto_unref_l3cd_init NML3ConfigData *l3cd = NULL;
    guint32                                 ifa_flags;
    guint8                                  plen;
    guint                                   i;
    const gint32                            now_sec = nm_utils_get_monotonic_timestamp_sec();

    l3cd = nm_l3_config_data_new(multi_idx, ifindex);

    nm_l3_config_data_set_source(l3cd, NM_IP_CONFIG_SOURCE_NDISC);

    nm_l3_config_data_set_ip6_privacy(l3cd, ip6_privacy);

    /* Check, whether kernel is recent enough to help user space handling RA.
     * If it's not supported, we have no ipv6-privacy and must add autoconf
     * addresses as /128. The reason for the /128 is to prevent the kernel
     * from adding a prefix route for this address. */
    ifa_flags = 0;
    if (kernel_support_extended_ifa_flags) {
        ifa_flags |= IFA_F_NOPREFIXROUTE;
        if (NM_IN_SET(ip6_privacy,
                      NM_SETTING_IP6_CONFIG_PRIVACY_PREFER_TEMP_ADDR,
                      NM_SETTING_IP6_CONFIG_PRIVACY_PREFER_PUBLIC_ADDR))
            ifa_flags |= IFA_F_MANAGETEMPADDR;
        plen = 64;
    } else
        plen = 128;

    for (i = 0; i < rdata->addresses_n; i++) {
        const NMNDiscAddress *ndisc_addr = &rdata->addresses[i];
        NMPlatformIP6Address  a;

        a = (NMPlatformIP6Address){
            .ifindex   = ifindex,
            .address   = ndisc_addr->address,
            .plen      = plen,
            .timestamp = now_sec,
            .lifetime  = _nm_ndisc_lifetime_from_expiry(((gint64) now_sec) * 1000,
                                                       ndisc_addr->expiry_msec,
                                                       TRUE),
            .preferred = _nm_ndisc_lifetime_from_expiry(
                ((gint64) now_sec) * 1000,
                NM_MIN(ndisc_addr->expiry_msec, ndisc_addr->expiry_preferred_msec),
                TRUE),
            .addr_source = NM_IP_CONFIG_SOURCE_NDISC,
            .n_ifa_flags = ifa_flags,
        };

        nm_l3_config_data_add_address_6(l3cd, &a);
    }

    for (i = 0; i < rdata->routes_n; i++) {
        const NMNDiscRoute *ndisc_route = &rdata->routes[i];
        NMPlatformIP6Route  r;

        r = (NMPlatformIP6Route){
            .ifindex       = ifindex,
            .network       = ndisc_route->network,
            .plen          = ndisc_route->plen,
            .gateway       = ndisc_route->gateway,
            .rt_source     = NM_IP_CONFIG_SOURCE_NDISC,
            .table_coerced = nm_platform_route_table_coerce(route_table),
            .metric        = route_metric,
            .rt_pref       = ndisc_route->preference,
        };
        nm_assert((NMIcmpv6RouterPref) r.rt_pref == ndisc_route->preference);

        nm_l3_config_data_add_route_6(l3cd, &r);
    }

    if (rdata->gateways_n > 0) {
        const NMIcmpv6RouterPref first_pref = rdata->gateways[0].preference;
        NMPlatformIP6Route       r          = {
            .rt_source     = NM_IP_CONFIG_SOURCE_NDISC,
            .ifindex       = ifindex,
            .table_coerced = nm_platform_route_table_coerce(route_table),
            .metric        = route_metric,
        };

        for (i = 0; i < rdata->gateways_n; i++) {
            r.gateway = rdata->gateways[i].address;
            r.rt_pref = rdata->gateways[i].preference;
            nm_assert((NMIcmpv6RouterPref) r.rt_pref == rdata->gateways[i].preference);
            nm_l3_config_data_add_route_6(l3cd, &r);

            if (first_pref != rdata->gateways[i].preference && !kernel_support_rta_pref) {
                /* We are unable to configure a router preference. Hence, we skip all gateways
                 * with a different preference from the first gateway. Note, that the gateways
                 * are sorted in order of highest to lowest preference. */
                break;
            }
        }
    }

    for (i = 0; i < rdata->dns_servers_n; i++)
        nm_l3_config_data_add_nameserver(l3cd, AF_INET6, &rdata->dns_servers[i].address);

    for (i = 0; i < rdata->dns_domains_n; i++)
        nm_l3_config_data_add_search(l3cd, AF_INET6, rdata->dns_domains[i].domain);

    nm_l3_config_data_set_ndisc_hop_limit(l3cd, rdata->hop_limit);
    nm_l3_config_data_set_ndisc_reachable_time_msec(l3cd, rdata->reachable_time_ms);
    nm_l3_config_data_set_ndisc_retrans_timer_msec(l3cd, rdata->retrans_timer_ms);

    nm_l3_config_data_set_ip6_mtu(l3cd, rdata->mtu);

    return g_steal_pointer(&l3cd);
}

/*****************************************************************************/

static guint8
_preference_to_priority(NMIcmpv6RouterPref pref)
{
    switch (pref) {
    case NM_ICMPV6_ROUTER_PREF_LOW:
        return 1;
    case NM_ICMPV6_ROUTER_PREF_MEDIUM:
        return 2;
    case NM_ICMPV6_ROUTER_PREF_HIGH:
        return 3;
    case NM_ICMPV6_ROUTER_PREF_INVALID:
        break;
    }
    return 0;
}

/*****************************************************************************/

static gboolean
expiry_next(gint64 now_msec, gint64 expiry_msec, gint64 *next_msec)
{
    if (expiry_msec == NM_NDISC_EXPIRY_INFINITY)
        return TRUE;

    if (expiry_msec <= now_msec) {
        /* expired. */
        return FALSE;
    }

    if (next_msec) {
        if (*next_msec > expiry_msec)
            *next_msec = expiry_msec;
    }

    /* the timestamp is good (not yet expired) */
    return TRUE;
}

static const char *
_get_exp(char *buf, gsize buf_size, gint64 now_msec, gint64 expiry_time)
{
    int l;

    if (expiry_time == NM_NDISC_EXPIRY_INFINITY)
        return "permanent";
    l = g_snprintf(buf, buf_size, "%.3f", ((double) (expiry_time - now_msec)) / 1000);
    nm_assert(l < buf_size);
    return buf;
}

#define get_exp(buf, now_msec, item) \
    _get_exp((buf), G_N_ELEMENTS(buf), (now_msec), (item)->expiry_msec)

/*****************************************************************************/

NMPNetns *
nm_ndisc_netns_get(NMNDisc *self)
{
    g_return_val_if_fail(NM_IS_NDISC(self), NULL);

    return NM_NDISC_GET_PRIVATE(self)->netns;
}

gboolean
nm_ndisc_netns_push(NMNDisc *self, NMPNetns **netns)
{
    NMNDiscPrivate *priv;

    g_return_val_if_fail(NM_IS_NDISC(self), FALSE);

    priv = NM_NDISC_GET_PRIVATE(self);
    if (priv->netns && !nmp_netns_push(priv->netns)) {
        NM_SET_OUT(netns, NULL);
        return FALSE;
    }

    NM_SET_OUT(netns, priv->netns);
    return TRUE;
}

/*****************************************************************************/

int
nm_ndisc_get_ifindex(NMNDisc *self)
{
    g_return_val_if_fail(NM_IS_NDISC(self), 0);

    return NM_NDISC_GET_PRIVATE(self)->ifindex;
}

const char *
nm_ndisc_get_ifname(NMNDisc *self)
{
    g_return_val_if_fail(NM_IS_NDISC(self), NULL);

    return NM_NDISC_GET_PRIVATE(self)->ifname;
}

NMNDiscNodeType
nm_ndisc_get_node_type(NMNDisc *self)
{
    g_return_val_if_fail(NM_IS_NDISC(self), NM_NDISC_NODE_TYPE_INVALID);

    return NM_NDISC_GET_PRIVATE(self)->node_type;
}

/*****************************************************************************/

static void
_ASSERT_data_gateways(const NMNDiscDataInternal *data)
{
#if NM_MORE_ASSERTS > 10
    guint                 i, j;
    const NMNDiscGateway *item_prev = NULL;

    if (!data->gateways->len)
        return;

    for (i = 0; i < data->gateways->len; i++) {
        const NMNDiscGateway *item = &g_array_index(data->gateways, NMNDiscGateway, i);

        nm_assert(!IN6_IS_ADDR_UNSPECIFIED(&item->address));
        nm_assert(item->expiry_msec >= 10000);
        for (j = 0; j < i; j++) {
            const NMNDiscGateway *item2 = &g_array_index(data->gateways, NMNDiscGateway, j);

            nm_assert(!IN6_ARE_ADDR_EQUAL(&item->address, &item2->address));
        }

        if (i > 0) {
            nm_assert(_preference_to_priority(item_prev->preference)
                      >= _preference_to_priority(item->preference));
        }

        item_prev = item;
    }
#endif
}

/*****************************************************************************/

static const NMNDiscData *
_data_complete(NMNDiscDataInternal *data)
{
    _ASSERT_data_gateways(data);

#define _SET(data, field)                                      \
    G_STMT_START                                               \
    {                                                          \
        if ((data->public.field##_n = data->field->len) > 0)   \
            data->public.field = (gpointer) data->field->data; \
        else                                                   \
            data->public.field = NULL;                         \
    }                                                          \
    G_STMT_END
    _SET(data, gateways);
    _SET(data, addresses);
    _SET(data, routes);
    _SET(data, dns_servers);
    _SET(data, dns_domains);
#undef _SET
    return &data->public;
}

void
nm_ndisc_emit_config_change(NMNDisc *self, NMNDiscConfigMap changed)
{
    _config_changed_log(self, changed);
    g_signal_emit(self,
                  signals[CONFIG_RECEIVED],
                  0,
                  _data_complete(&NM_NDISC_GET_PRIVATE(self)->rdata),
                  (guint) changed);
}

/*****************************************************************************/

gboolean
nm_ndisc_add_gateway(NMNDisc *ndisc, const NMNDiscGateway *new_item, gint64 now_msec)
{
    NMNDiscDataInternal *rdata = &NM_NDISC_GET_PRIVATE(ndisc)->rdata;
    guint                i;
    guint                insert_idx = G_MAXUINT;

    for (i = 0; i < rdata->gateways->len;) {
        NMNDiscGateway *item = &g_array_index(rdata->gateways, NMNDiscGateway, i);

        if (IN6_ARE_ADDR_EQUAL(&item->address, &new_item->address)) {
            if (new_item->expiry_msec <= now_msec) {
                g_array_remove_index(rdata->gateways, i);
                _ASSERT_data_gateways(rdata);
                return TRUE;
            }

            if (item->preference != new_item->preference) {
                g_array_remove_index(rdata->gateways, i);
                continue;
            }

            if (item->expiry_msec == new_item->expiry_msec)
                return FALSE;

            item->expiry_msec = new_item->expiry_msec;
            _ASSERT_data_gateways(rdata);
            return TRUE;
        }

        /* Put before less preferable gateways. */
        if (_preference_to_priority(item->preference)
                < _preference_to_priority(new_item->preference)
            && insert_idx == G_MAXUINT)
            insert_idx = i;

        i++;
    }

    if (new_item->expiry_msec <= now_msec)
        return FALSE;

    g_array_insert_val(rdata->gateways,
                       insert_idx == G_MAXUINT ? rdata->gateways->len : insert_idx,
                       *new_item);
    _ASSERT_data_gateways(rdata);
    return TRUE;
}

/**
 * complete_address:
 * @ndisc: the #NMNDisc
 * @addr: the #NMNDiscAddress
 *
 * Adds the host part to the address that has network part set.
 * If the address already has a host part, add a different host part
 * if possible (this is useful in case DAD failed).
 *
 * Can fail if a different address can not be generated (DAD failure
 * for an EUI-64 address or DAD counter overflow).
 *
 * Returns: %TRUE if the address could be completed, %FALSE otherwise.
 **/
static gboolean
complete_address(NMNDisc *ndisc, NMNDiscAddress *addr)
{
    NMNDiscPrivate *priv;
    GError *        error = NULL;

    g_return_val_if_fail(NM_IS_NDISC(ndisc), FALSE);

    priv = NM_NDISC_GET_PRIVATE(ndisc);
    if (priv->addr_gen_mode == NM_SETTING_IP6_CONFIG_ADDR_GEN_MODE_STABLE_PRIVACY) {
        if (!nm_utils_ipv6_addr_set_stable_privacy(priv->stable_type,
                                                   &addr->address,
                                                   priv->ifname,
                                                   priv->network_id,
                                                   addr->dad_counter++,
                                                   &error)) {
            _LOGW("complete-address: failed to generate an stable-privacy address: %s",
                  error->message);
            g_clear_error(&error);
            return FALSE;
        }
        _LOGD("complete-address: using an stable-privacy address");
        return TRUE;
    }

    if (!priv->iid.id) {
        _LOGW("complete-address: can't generate an EUI-64 address: no interface identifier");
        return FALSE;
    }

    if (addr->address.s6_addr32[2] == 0x0 && addr->address.s6_addr32[3] == 0x0) {
        _LOGD("complete-address: adding an EUI-64 address");
        nm_utils_ipv6_addr_set_interface_identifier(&addr->address, priv->iid);
        return TRUE;
    }

    _LOGW("complete-address: can't generate a new_item EUI-64 address");
    return FALSE;
}

static gboolean
nm_ndisc_add_address(NMNDisc *             ndisc,
                     const NMNDiscAddress *new_item,
                     gint64                now_msec,
                     gboolean              from_ra)
{
    NMNDiscPrivate *     priv  = NM_NDISC_GET_PRIVATE(ndisc);
    NMNDiscDataInternal *rdata = &priv->rdata;
    NMNDiscAddress *     new2;
    NMNDiscAddress *     existing = NULL;
    guint                i;

    nm_assert(new_item);
    nm_assert(!IN6_IS_ADDR_UNSPECIFIED(&new_item->address));
    nm_assert(!IN6_IS_ADDR_LINKLOCAL(&new_item->address));
    nm_assert(new_item->expiry_preferred_msec <= new_item->expiry_msec);
    nm_assert((!!from_ra) == (now_msec > 0));

    for (i = 0; i < rdata->addresses->len; i++) {
        NMNDiscAddress *item = &g_array_index(rdata->addresses, NMNDiscAddress, i);

        if (from_ra) {
            /* RFC4862 5.5.3.d, we find an existing address with the same prefix.
             * (note that all prefixes at this point have implicitly length /64). */
            if (memcmp(&item->address, &new_item->address, 8) == 0) {
                existing = item;
                break;
            }
        } else {
            if (IN6_ARE_ADDR_EQUAL(&item->address, &new_item->address)) {
                existing = item;
                break;
            }
        }
    }

    if (existing) {
        gint64 new_expiry_preferred_msec;
        gint64 new_expiry_msec;

        if (from_ra) {
            if (new_item->expiry_msec == NM_NDISC_EXPIRY_INFINITY)
                new_expiry_msec = NM_NDISC_EXPIRY_INFINITY;
            else {
                const gint64 NDISC_PREFIX_LFT_MIN_MSEC = 7200 * 1000; /* RFC4862 5.5.3.e */
                gint64       new_lifetime;
                gint64       existing_lifetime;

                new_lifetime = new_item->expiry_msec - now_msec;
                if (existing->expiry_msec == NM_NDISC_EXPIRY_INFINITY)
                    existing_lifetime = G_MAXINT64;
                else
                    existing_lifetime = existing->expiry_msec - now_msec;

                /* see RFC4862 5.5.3.e */
                if (new_lifetime >= NDISC_PREFIX_LFT_MIN_MSEC
                    || new_lifetime >= existing_lifetime) {
                    /* either extend the lifetime of the new_item lifetime is longer than
                     * NDISC_PREFIX_LFT_MIN_MSEC. */
                    new_expiry_msec = new_item->expiry_msec;
                } else if (existing_lifetime <= NDISC_PREFIX_LFT_MIN_MSEC) {
                    /* keep the current lifetime. */
                    new_expiry_msec = existing->expiry_msec;
                } else {
                    /* trim the current lifetime to NDISC_PREFIX_LFT_MIN_MSEC. */
                    new_expiry_msec = now_msec + NDISC_PREFIX_LFT_MIN_MSEC;
                }
            }

            new_expiry_preferred_msec =
                NM_MIN(new_item->expiry_preferred_msec, new_item->expiry_msec);
            new_expiry_preferred_msec = NM_MIN(new_expiry_preferred_msec, new_expiry_msec);
        } else {
            if (new_item->expiry_msec <= now_msec) {
                g_array_remove_index(rdata->addresses, i);
                return TRUE;
            }

            new_expiry_msec = new_item->expiry_msec;
            new_expiry_preferred_msec =
                NM_MIN(new_item->expiry_preferred_msec, new_item->expiry_msec);
        }

        /* the dad_counter does not get modified. */
        if (new_expiry_msec == existing->expiry_msec
            && new_expiry_preferred_msec == existing->expiry_preferred_msec) {
            return FALSE;
        }

        existing->expiry_msec           = new_expiry_msec;
        existing->expiry_preferred_msec = new_expiry_preferred_msec;
        return TRUE;
    }

    /* we create at most max_addresses autoconf addresses. This is different from
     * what the kernel does, because it considers *all* addresses (including
     * static and other temporary addresses).
     **/
    if (priv->max_addresses && rdata->addresses->len >= priv->max_addresses)
        return FALSE;

    if (new_item->expiry_msec <= now_msec)
        return FALSE;

    new2 = nm_g_array_append_new(rdata->addresses, NMNDiscAddress);

    *new2 = *new_item;

    new2->expiry_preferred_msec = NM_MIN(new2->expiry_preferred_msec, new2->expiry_msec);

    if (from_ra) {
        new2->dad_counter = 0;
        if (!complete_address(ndisc, new2)) {
            g_array_set_size(rdata->addresses, rdata->addresses->len - 1);
            return FALSE;
        }
    }

    return TRUE;
}

gboolean
nm_ndisc_complete_and_add_address(NMNDisc *ndisc, const NMNDiscAddress *new_item, gint64 now_msec)
{
    return nm_ndisc_add_address(ndisc, new_item, now_msec, TRUE);
}

gboolean
nm_ndisc_add_route(NMNDisc *ndisc, const NMNDiscRoute *new_item, gint64 now_msec)
{
    NMNDiscPrivate *     priv;
    NMNDiscDataInternal *rdata;
    guint                i;
    guint                insert_idx = G_MAXUINT;
    gboolean             changed    = FALSE;

    if (new_item->plen == 0 || new_item->plen > 128) {
        /* Only expect non-default routes.  The router has no idea what the
         * local configuration or user preferences are, so sending routes
         * with a prefix length of 0 must be ignored by NMNDisc.
         *
         * Also, upper layers also don't expect that NMNDisc exposes routes
         * with a plen or zero or larger then 128.
         */
        g_return_val_if_reached(FALSE);
    }

    priv  = NM_NDISC_GET_PRIVATE(ndisc);
    rdata = &priv->rdata;

    for (i = 0; i < rdata->routes->len;) {
        NMNDiscRoute *item = &g_array_index(rdata->routes, NMNDiscRoute, i);

        if (IN6_ARE_ADDR_EQUAL(&item->network, &new_item->network)
            && item->plen == new_item->plen) {
            if (new_item->expiry_msec <= now_msec) {
                g_array_remove_index(rdata->routes, i);
                return TRUE;
            }

            if (item->preference != new_item->preference) {
                g_array_remove_index(rdata->routes, i);
                changed = TRUE;
                continue;
            }

            if (item->expiry_msec == new_item->expiry_msec
                && IN6_ARE_ADDR_EQUAL(&item->gateway, &new_item->gateway))
                return FALSE;

            item->expiry_msec = new_item->expiry_msec;
            item->gateway     = new_item->gateway;
            return TRUE;
        }

        /* Put before less preferable routes. */
        if (_preference_to_priority(item->preference)
                < _preference_to_priority(new_item->preference)
            && insert_idx == G_MAXUINT)
            insert_idx = i;

        i++;
    }

    if (new_item->expiry_msec <= now_msec) {
        nm_assert(!changed);
        return FALSE;
    }

    g_array_insert_val(rdata->routes, insert_idx == G_MAXUINT ? 0u : insert_idx, *new_item);
    return TRUE;
}

gboolean
nm_ndisc_add_dns_server(NMNDisc *ndisc, const NMNDiscDNSServer *new_item, gint64 now_msec)
{
    NMNDiscPrivate *     priv;
    NMNDiscDataInternal *rdata;
    guint                i;

    priv  = NM_NDISC_GET_PRIVATE(ndisc);
    rdata = &priv->rdata;

    for (i = 0; i < rdata->dns_servers->len; i++) {
        NMNDiscDNSServer *item = &g_array_index(rdata->dns_servers, NMNDiscDNSServer, i);

        if (IN6_ARE_ADDR_EQUAL(&item->address, &new_item->address)) {
            if (new_item->expiry_msec <= now_msec) {
                g_array_remove_index(rdata->dns_servers, i);
                return TRUE;
            }

            if (item->expiry_msec == new_item->expiry_msec)
                return FALSE;

            item->expiry_msec = new_item->expiry_msec;
            return TRUE;
        }
    }

    if (new_item->expiry_msec <= now_msec)
        return FALSE;

    g_array_append_val(rdata->dns_servers, *new_item);
    return TRUE;
}

/* Copies new_item->domain if 'new_item' is added to the dns_domains list */
gboolean
nm_ndisc_add_dns_domain(NMNDisc *ndisc, const NMNDiscDNSDomain *new_item, gint64 now_msec)
{
    NMNDiscPrivate *     priv;
    NMNDiscDataInternal *rdata;
    NMNDiscDNSDomain *   item;
    guint                i;

    priv  = NM_NDISC_GET_PRIVATE(ndisc);
    rdata = &priv->rdata;

    for (i = 0; i < rdata->dns_domains->len; i++) {
        item = &g_array_index(rdata->dns_domains, NMNDiscDNSDomain, i);

        if (nm_streq(item->domain, new_item->domain)) {
            if (new_item->expiry_msec <= now_msec) {
                g_array_remove_index(rdata->dns_domains, i);
                return TRUE;
            }

            if (item->expiry_msec == new_item->expiry_msec)
                return FALSE;

            item->expiry_msec = new_item->expiry_msec;
            return TRUE;
        }
    }

    if (new_item->expiry_msec <= now_msec)
        return FALSE;

    item  = nm_g_array_append_new(rdata->dns_domains, NMNDiscDNSDomain);
    *item = (NMNDiscDNSDomain){
        .domain      = g_strdup(new_item->domain),
        .expiry_msec = new_item->expiry_msec,
    };
    return TRUE;
}

/*****************************************************************************/

#define _MAYBE_WARN(...)                                                   \
    G_STMT_START                                                           \
    {                                                                      \
        gboolean _different_message;                                       \
                                                                           \
        _different_message = !nm_streq0(priv->last_error, error->message); \
        _NMLOG(_different_message ? LOGL_WARN : LOGL_DEBUG, __VA_ARGS__);  \
        if (_different_message) {                                          \
            nm_clear_g_free(&priv->last_error);                            \
            priv->last_error = g_strdup(error->message);                   \
        }                                                                  \
    }                                                                      \
    G_STMT_END

static gboolean
send_rs_timeout(NMNDisc *ndisc)
{
    nm_auto_pop_netns NMPNetns *netns = NULL;
    NMNDiscClass *              klass = NM_NDISC_GET_CLASS(ndisc);
    NMNDiscPrivate *            priv  = NM_NDISC_GET_PRIVATE(ndisc);
    GError *                    error = NULL;

    priv->send_rs_id = 0;

    if (!nm_ndisc_netns_push(ndisc, &netns))
        return G_SOURCE_REMOVE;

    if (klass->send_rs(ndisc, &error)) {
        _LOGD("router solicitation sent");
        priv->solicitations_left--;
        nm_clear_g_free(&priv->last_error);
    } else {
        _MAYBE_WARN("failure sending router solicitation: %s", error->message);
        g_clear_error(&error);
    }

    priv->last_rs = nm_utils_get_monotonic_timestamp_sec();
    if (priv->solicitations_left > 0) {
        _LOGD("scheduling router solicitation retry in %d seconds.",
              (int) priv->router_solicitation_interval);
        priv->send_rs_id = g_timeout_add_seconds(priv->router_solicitation_interval,
                                                 (GSourceFunc) send_rs_timeout,
                                                 ndisc);
    } else {
        _LOGD("did not receive a router advertisement after %d solicitations.",
              (int) priv->router_solicitations);
    }

    return G_SOURCE_REMOVE;
}

static void
solicit_routers(NMNDisc *ndisc)
{
    NMNDiscPrivate *priv = NM_NDISC_GET_PRIVATE(ndisc);
    gint32          now, next;
    gint64          t;

    if (priv->send_rs_id)
        return;

    now                      = nm_utils_get_monotonic_timestamp_sec();
    priv->solicitations_left = priv->router_solicitations;

    t    = (((gint64) priv->last_rs) + priv->router_solicitation_interval) - now;
    next = CLAMP(t, 0, G_MAXINT32);
    _LOGD("scheduling explicit router solicitation request in %" G_GINT32_FORMAT " seconds.", next);
    priv->send_rs_id = g_timeout_add_seconds((guint32) next, (GSourceFunc) send_rs_timeout, ndisc);
}

static gboolean
announce_router(NMNDisc *ndisc)
{
    nm_auto_pop_netns NMPNetns *netns = NULL;
    NMNDiscClass *              klass = NM_NDISC_GET_CLASS(ndisc);
    NMNDiscPrivate *            priv  = NM_NDISC_GET_PRIVATE(ndisc);
    GError *                    error = NULL;

    if (!nm_ndisc_netns_push(ndisc, &netns))
        return G_SOURCE_REMOVE;

    priv->last_ra = nm_utils_get_monotonic_timestamp_sec();
    if (klass->send_ra(ndisc, &error)) {
        _LOGD("router advertisement sent");
        nm_clear_g_free(&priv->last_error);
    } else {
        _MAYBE_WARN("failure sending router advertisement: %s", error->message);
        g_clear_error(&error);
    }

    if (--priv->announcements_left) {
        _LOGD("will resend an initial router advertisement");

        /* Schedule next initial announcement retransmit. */
        priv->send_ra_id =
            g_timeout_add_seconds(g_random_int_range(NM_NDISC_ROUTER_ADVERT_DELAY,
                                                     NM_NDISC_ROUTER_ADVERT_INITIAL_INTERVAL),
                                  (GSourceFunc) announce_router,
                                  ndisc);
    } else {
        _LOGD("will send an unsolicited router advertisement");

        /* Schedule next unsolicited announcement. */
        priv->announcements_left = 1;
        priv->send_ra_id         = g_timeout_add_seconds(NM_NDISC_ROUTER_ADVERT_MAX_INTERVAL,
                                                 (GSourceFunc) announce_router,
                                                 ndisc);
    }

    return G_SOURCE_REMOVE;
}

static void
announce_router_initial(NMNDisc *ndisc)
{
    NMNDiscPrivate *priv = NM_NDISC_GET_PRIVATE(ndisc);

    _LOGD("will send an initial router advertisement");

    /* Retry three more times. */
    priv->announcements_left = NM_NDISC_ROUTER_ADVERTISEMENTS_DEFAULT;

    /* Unschedule an unsolicited resend if we are allowed to send now. */
    if (G_LIKELY(nm_utils_get_monotonic_timestamp_sec() - priv->last_ra
                 > NM_NDISC_ROUTER_ADVERT_DELAY))
        nm_clear_g_source(&priv->send_ra_id);

    /* Schedule the initial send rather early. Clamp the delay by minimal
     * delay and not the initial advert internal so that we start fast. */
    if (G_LIKELY(!priv->send_ra_id)) {
        priv->send_ra_id =
            g_timeout_add_seconds(g_random_int_range(0, NM_NDISC_ROUTER_ADVERT_DELAY),
                                  (GSourceFunc) announce_router,
                                  ndisc);
    }
}

static void
announce_router_solicited(NMNDisc *ndisc)
{
    NMNDiscPrivate *priv = NM_NDISC_GET_PRIVATE(ndisc);

    _LOGD("will send an solicited router advertisement");

    /* Unschedule an unsolicited resend if we are allowed to send now. */
    if (nm_utils_get_monotonic_timestamp_sec() - priv->last_ra > NM_NDISC_ROUTER_ADVERT_DELAY)
        nm_clear_g_source(&priv->send_ra_id);

    if (!priv->send_ra_id) {
        priv->send_ra_id = g_timeout_add(g_random_int_range(0, NM_NDISC_ROUTER_ADVERT_DELAY_MS),
                                         (GSourceFunc) announce_router,
                                         ndisc);
    }
}

/*****************************************************************************/

void
nm_ndisc_set_config(NMNDisc *     ndisc,
                    const GArray *addresses,
                    const GArray *dns_servers,
                    const GArray *dns_domains)
{
    gboolean changed = FALSE;
    guint    i;

    for (i = 0; i < addresses->len; i++) {
        if (nm_ndisc_add_address(ndisc, &g_array_index(addresses, NMNDiscAddress, i), 0, FALSE))
            changed = TRUE;
    }

    for (i = 0; i < dns_servers->len; i++) {
        if (nm_ndisc_add_dns_server(ndisc,
                                    &g_array_index(dns_servers, NMNDiscDNSServer, i),
                                    G_MININT64))
            changed = TRUE;
    }

    for (i = 0; i < dns_domains->len; i++) {
        if (nm_ndisc_add_dns_domain(ndisc,
                                    &g_array_index(dns_domains, NMNDiscDNSDomain, i),
                                    G_MININT64))
            changed = TRUE;
    }

    if (changed)
        announce_router_initial(ndisc);
}

/**
 * nm_ndisc_set_iid:
 * @ndisc: the #NMNDisc
 * @iid: the new interface ID
 *
 * Sets the "Modified EUI-64" interface ID to be used when generating
 * IPv6 addresses using received prefixes. Identifiers are either generated
 * from the hardware addresses or manually set by the operator with
 * "ip token" command.
 *
 * Upon token change (or initial setting) all addresses generated using
 * the old identifier are removed. The caller should ensure the addresses
 * will be reset by soliciting router advertisements.
 *
 * In case the stable privacy addressing is used %FALSE is returned and
 * addresses are left untouched.
 *
 * Returns: %TRUE if addresses need to be regenerated, %FALSE otherwise.
 **/
gboolean
nm_ndisc_set_iid(NMNDisc *ndisc, const NMUtilsIPv6IfaceId iid)
{
    NMNDiscPrivate *     priv;
    NMNDiscDataInternal *rdata;

    g_return_val_if_fail(NM_IS_NDISC(ndisc), FALSE);

    priv  = NM_NDISC_GET_PRIVATE(ndisc);
    rdata = &priv->rdata;

    if (priv->iid.id != iid.id) {
        priv->iid = iid;

        if (priv->addr_gen_mode == NM_SETTING_IP6_CONFIG_ADDR_GEN_MODE_STABLE_PRIVACY)
            return FALSE;

        if (rdata->addresses->len) {
            _LOGD("IPv6 interface identifier changed, flushing addresses");
            g_array_remove_range(rdata->addresses, 0, rdata->addresses->len);
            nm_ndisc_emit_config_change(ndisc, NM_NDISC_CONFIG_ADDRESSES);
            solicit_routers(ndisc);
        }
        return TRUE;
    }

    return FALSE;
}

static gboolean
ndisc_ra_timeout_cb(gpointer user_data)
{
    NMNDisc *ndisc = NM_NDISC(user_data);

    nm_clear_g_source_inst(&NM_NDISC_GET_PRIVATE(ndisc)->ra_timeout_source);
    g_signal_emit(ndisc, signals[RA_TIMEOUT_SIGNAL], 0);
    return G_SOURCE_REMOVE;
}

void
nm_ndisc_start(NMNDisc *ndisc)
{
    nm_auto_pop_netns NMPNetns *netns = NULL;
    NMNDiscPrivate *            priv;

    g_return_if_fail(NM_IS_NDISC(ndisc));

    priv = NM_NDISC_GET_PRIVATE(ndisc);

    nm_assert(NM_NDISC_GET_CLASS(ndisc)->start);
    nm_assert(!priv->ra_timeout_source);

    _LOGD("starting neighbor discovery for ifindex %d%s",
          priv->ifindex,
          priv->node_type == NM_NDISC_NODE_TYPE_HOST ? " (solicit)" : " (announce)");

    if (!nm_ndisc_netns_push(ndisc, &netns))
        return;

    NM_NDISC_GET_CLASS(ndisc)->start(ndisc);

    if (priv->node_type == NM_NDISC_NODE_TYPE_HOST) {
        G_STATIC_ASSERT_EXPR(NM_RA_TIMEOUT_DEFAULT == 0);
        G_STATIC_ASSERT_EXPR(NM_RA_TIMEOUT_INFINITY == G_MAXINT32);
        nm_assert(priv->ra_timeout > 0u);
        nm_assert(priv->ra_timeout <= NM_RA_TIMEOUT_INFINITY);

        if (priv->ra_timeout < NM_RA_TIMEOUT_INFINITY) {
            guint timeout_msec;

            _LOGD("scheduling RA timeout in %u seconds", priv->ra_timeout);
            if (priv->ra_timeout < G_MAXUINT / 1000u)
                timeout_msec = priv->ra_timeout * 1000u;
            else
                timeout_msec = G_MAXUINT;
            priv->ra_timeout_source = nm_g_timeout_source_new(timeout_msec,
                                                              G_PRIORITY_DEFAULT,
                                                              ndisc_ra_timeout_cb,
                                                              ndisc,
                                                              NULL);
            g_source_attach(priv->ra_timeout_source, NULL);
        }

        solicit_routers(ndisc);
        return;
    }

    nm_assert(priv->ra_timeout == 0u);
    nm_assert(priv->node_type == NM_NDISC_NODE_TYPE_ROUTER);
    announce_router_initial(ndisc);
}

void
nm_ndisc_stop(NMNDisc *ndisc)
{
    nm_auto_pop_netns NMPNetns *netns = NULL;
    NMNDiscDataInternal *       rdata;
    NMNDiscPrivate *            priv;

    g_return_if_fail(NM_IS_NDISC(ndisc));

    priv = NM_NDISC_GET_PRIVATE(ndisc);

    nm_assert(NM_NDISC_GET_CLASS(ndisc)->stop);

    _LOGD("stopping neighbor discovery for ifindex %d", priv->ifindex);

    if (!nm_ndisc_netns_push(ndisc, &netns))
        return;

    NM_NDISC_GET_CLASS(ndisc)->stop(ndisc);

    rdata = &priv->rdata;

    g_array_set_size(rdata->gateways, 0);
    g_array_set_size(rdata->addresses, 0);
    g_array_set_size(rdata->routes, 0);
    g_array_set_size(rdata->dns_servers, 0);
    g_array_set_size(rdata->dns_domains, 0);
    priv->rdata.public.hop_limit = 64;

    /* Start at very low number so that last_rs - router_solicitation_interval
     * is much lower than nm_utils_get_monotonic_timestamp_sec() at startup.
     */
    priv->last_rs = G_MININT32;
    nm_clear_g_source_inst(&priv->ra_timeout_source);
    nm_clear_g_source(&priv->send_rs_id);
    nm_clear_g_source(&priv->send_ra_id);
    nm_clear_g_free(&priv->last_error);
    nm_clear_g_source_inst(&priv->timeout_expire_source);

    priv->solicitations_left = 0;
    priv->announcements_left = 0;

    priv->last_rs = G_MININT32;
    priv->last_ra = G_MININT32;
}

NMNDiscConfigMap
nm_ndisc_dad_failed(NMNDisc *ndisc, const struct in6_addr *address, gboolean emit_changed_signal)
{
    NMNDiscDataInternal *rdata;
    guint                i;
    gboolean             changed = FALSE;

    rdata = &NM_NDISC_GET_PRIVATE(ndisc)->rdata;

    for (i = 0; i < rdata->addresses->len;) {
        NMNDiscAddress *item = &g_array_index(rdata->addresses, NMNDiscAddress, i);

        if (IN6_ARE_ADDR_EQUAL(&item->address, address)) {
            char sbuf[NM_UTILS_INET_ADDRSTRLEN];

            _LOGD("DAD failed for discovered address %s", _nm_utils_inet6_ntop(address, sbuf));
            changed = TRUE;
            if (!complete_address(ndisc, item)) {
                g_array_remove_index(rdata->addresses, i);
                continue;
            }
        }
        i++;
    }

    if (emit_changed_signal && changed)
        nm_ndisc_emit_config_change(ndisc, NM_NDISC_CONFIG_ADDRESSES);

    return changed ? NM_NDISC_CONFIG_ADDRESSES : NM_NDISC_CONFIG_NONE;
}

#define CONFIG_MAP_MAX_STR 7

static void
config_map_to_string(NMNDiscConfigMap map, char *p)
{
    if (map & NM_NDISC_CONFIG_DHCP_LEVEL)
        *p++ = 'd';
    if (map & NM_NDISC_CONFIG_GATEWAYS)
        *p++ = 'G';
    if (map & NM_NDISC_CONFIG_ADDRESSES)
        *p++ = 'A';
    if (map & NM_NDISC_CONFIG_ROUTES)
        *p++ = 'R';
    if (map & NM_NDISC_CONFIG_DNS_SERVERS)
        *p++ = 'S';
    if (map & NM_NDISC_CONFIG_DNS_DOMAINS)
        *p++ = 'D';
    *p = '\0';
}

static const char *
dhcp_level_to_string(NMNDiscDHCPLevel dhcp_level)
{
    switch (dhcp_level) {
    case NM_NDISC_DHCP_LEVEL_NONE:
        return "none";
    case NM_NDISC_DHCP_LEVEL_OTHERCONF:
        return "otherconf";
    case NM_NDISC_DHCP_LEVEL_MANAGED:
        return "managed";
    default:
        return "INVALID";
    }
}

static void
_config_changed_log(NMNDisc *ndisc, NMNDiscConfigMap changed)
{
    NMNDiscPrivate *     priv;
    NMNDiscDataInternal *rdata;
    guint                i;
    char                 changedstr[CONFIG_MAP_MAX_STR];
    char                 addrstr[NM_UTILS_INET_ADDRSTRLEN];
    char                 str_pref[35];
    char                 str_exp[100];
    gint64               now_msec;

    if (!_LOGD_ENABLED())
        return;

    now_msec = nm_utils_get_monotonic_timestamp_msec();

    priv  = NM_NDISC_GET_PRIVATE(ndisc);
    rdata = &priv->rdata;

    config_map_to_string(changed, changedstr);
    _LOGD("neighbor discovery configuration changed [%s]:", changedstr);
    _LOGD("  dhcp-level %s", dhcp_level_to_string(priv->rdata.public.dhcp_level));

    if (rdata->public.hop_limit)
        _LOGD("  hop limit      : %d", rdata->public.hop_limit);
    if (rdata->public.reachable_time_ms)
        _LOGD("  reachable time : %u", (guint) rdata->public.reachable_time_ms);
    if (rdata->public.retrans_timer_ms)
        _LOGD("  retrans timer  : %u", (guint) rdata->public.retrans_timer_ms);

    for (i = 0; i < rdata->gateways->len; i++) {
        const NMNDiscGateway *gateway = &g_array_index(rdata->gateways, NMNDiscGateway, i);

        _LOGD("  gateway %s pref %s exp %s",
              _nm_utils_inet6_ntop(&gateway->address, addrstr),
              nm_icmpv6_router_pref_to_string(gateway->preference, str_pref, sizeof(str_pref)),
              get_exp(str_exp, now_msec, gateway));
    }
    for (i = 0; i < rdata->addresses->len; i++) {
        const NMNDiscAddress *address = &g_array_index(rdata->addresses, NMNDiscAddress, i);

        _LOGD("  address %s exp %s",
              _nm_utils_inet6_ntop(&address->address, addrstr),
              get_exp(str_exp, now_msec, address));
    }
    for (i = 0; i < rdata->routes->len; i++) {
        const NMNDiscRoute *route = &g_array_index(rdata->routes, NMNDiscRoute, i);
        char                sbuf[NM_UTILS_INET_ADDRSTRLEN];

        _LOGD("  route %s/%u via %s pref %s exp %s",
              _nm_utils_inet6_ntop(&route->network, addrstr),
              (guint) route->plen,
              _nm_utils_inet6_ntop(&route->gateway, sbuf),
              nm_icmpv6_router_pref_to_string(route->preference, str_pref, sizeof(str_pref)),
              get_exp(str_exp, now_msec, route));
    }
    for (i = 0; i < rdata->dns_servers->len; i++) {
        const NMNDiscDNSServer *dns_server =
            &g_array_index(rdata->dns_servers, NMNDiscDNSServer, i);

        _LOGD("  dns_server %s exp %s",
              _nm_utils_inet6_ntop(&dns_server->address, addrstr),
              get_exp(str_exp, now_msec, dns_server));
    }
    for (i = 0; i < rdata->dns_domains->len; i++) {
        const NMNDiscDNSDomain *dns_domain =
            &g_array_index(rdata->dns_domains, NMNDiscDNSDomain, i);

        _LOGD("  dns_domain %s exp %s", dns_domain->domain, get_exp(str_exp, now_msec, dns_domain));
    }
}

/*****************************************************************************/

static void
clean_gateways(NMNDisc *ndisc, gint64 now_msec, NMNDiscConfigMap *changed, gint64 *next_msec)
{
    NMNDiscDataInternal *rdata = &NM_NDISC_GET_PRIVATE(ndisc)->rdata;
    NMNDiscGateway *     arr;
    guint                i;
    guint                j;

    if (rdata->gateways->len == 0)
        return;

    arr = &g_array_index(rdata->gateways, NMNDiscGateway, 0);

    for (i = 0, j = 0; i < rdata->gateways->len; i++) {
        if (!expiry_next(now_msec, arr[i].expiry_msec, next_msec))
            continue;
        if (i != j)
            arr[j] = arr[i];
        j++;
    }

    if (i != j) {
        *changed |= NM_NDISC_CONFIG_GATEWAYS;
        g_array_set_size(rdata->gateways, j);
    }

    _ASSERT_data_gateways(rdata);
}

static void
clean_addresses(NMNDisc *ndisc, gint64 now_msec, NMNDiscConfigMap *changed, gint64 *next_msec)
{
    NMNDiscDataInternal *rdata = &NM_NDISC_GET_PRIVATE(ndisc)->rdata;
    NMNDiscAddress *     arr;
    guint                i;
    guint                j;

    if (rdata->addresses->len == 0)
        return;

    arr = &g_array_index(rdata->addresses, NMNDiscAddress, 0);

    for (i = 0, j = 0; i < rdata->addresses->len; i++) {
        if (!expiry_next(now_msec, arr[i].expiry_msec, next_msec))
            continue;
        if (i != j)
            arr[j] = arr[i];
        j++;
    }

    if (i != j) {
        *changed = NM_NDISC_CONFIG_ADDRESSES;
        g_array_set_size(rdata->addresses, j);
    }
}

static void
clean_routes(NMNDisc *ndisc, gint64 now_msec, NMNDiscConfigMap *changed, gint64 *next_msec)
{
    NMNDiscDataInternal *rdata = &NM_NDISC_GET_PRIVATE(ndisc)->rdata;
    NMNDiscRoute *       arr;
    guint                i;
    guint                j;

    if (rdata->routes->len == 0)
        return;

    arr = &g_array_index(rdata->routes, NMNDiscRoute, 0);

    for (i = 0, j = 0; i < rdata->routes->len; i++) {
        if (!expiry_next(now_msec, arr[i].expiry_msec, next_msec))
            continue;
        if (i != j)
            arr[j] = arr[i];
        j++;
    }

    if (i != j) {
        *changed |= NM_NDISC_CONFIG_ROUTES;
        g_array_set_size(rdata->routes, j);
    }
}

static void
clean_dns_servers(NMNDisc *ndisc, gint64 now_msec, NMNDiscConfigMap *changed, gint64 *next_msec)
{
    NMNDiscDataInternal *rdata = &NM_NDISC_GET_PRIVATE(ndisc)->rdata;
    NMNDiscDNSServer *   arr;
    guint                i;
    guint                j;

    if (rdata->dns_servers->len == 0)
        return;

    arr = &g_array_index(rdata->dns_servers, NMNDiscDNSServer, 0);

    for (i = 0, j = 0; i < rdata->dns_servers->len; i++) {
        if (!expiry_next(now_msec, arr[i].expiry_msec, next_msec))
            continue;
        if (i != j)
            arr[j] = arr[i];
        j++;
    }

    if (i != j) {
        *changed |= NM_NDISC_CONFIG_DNS_SERVERS;
        g_array_set_size(rdata->dns_servers, j);
    }
}

static void
clean_dns_domains(NMNDisc *ndisc, gint64 now_msec, NMNDiscConfigMap *changed, gint64 *next_msec)
{
    NMNDiscDataInternal *rdata = &NM_NDISC_GET_PRIVATE(ndisc)->rdata;
    NMNDiscDNSDomain *   arr;
    guint                i;
    guint                j;

    if (rdata->dns_domains->len == 0)
        return;

    arr = &g_array_index(rdata->dns_domains, NMNDiscDNSDomain, 0);

    for (i = 0, j = 0; i < rdata->dns_domains->len; i++) {
        if (!expiry_next(now_msec, arr[i].expiry_msec, next_msec))
            continue;

        if (i != j) {
            g_free(arr[j].domain);
            arr[j]        = arr[i];
            arr[i].domain = NULL;
        }

        j++;
    }

    if (i != 0) {
        *changed |= NM_NDISC_CONFIG_DNS_DOMAINS;
        g_array_set_size(rdata->dns_domains, j);
    }
}

static void
check_timestamps(NMNDisc *ndisc, gint64 now_msec, NMNDiscConfigMap changed)
{
    NMNDiscPrivate *priv      = NM_NDISC_GET_PRIVATE(ndisc);
    gint64          next_msec = G_MAXINT64;

    _LOGT("router-data: check for changed router advertisement data");

    clean_gateways(ndisc, now_msec, &changed, &next_msec);
    clean_addresses(ndisc, now_msec, &changed, &next_msec);
    clean_routes(ndisc, now_msec, &changed, &next_msec);
    clean_dns_servers(ndisc, now_msec, &changed, &next_msec);
    clean_dns_domains(ndisc, now_msec, &changed, &next_msec);

    nm_assert(next_msec > now_msec);

    nm_clear_g_source_inst(&priv->timeout_expire_source);

    if (next_msec == NM_NDISC_EXPIRY_INFINITY)
        _LOGD("router-data: next lifetime expiration will happen: never");
    else {
        const gint64 timeout_msec = NM_MIN(next_msec - now_msec, ((gint64) G_MAXINT32));
        const guint  TIMEOUT_APPROX_THRESHOLD_SEC = 10000;

        _LOGD("router-data: next lifetime expiration will happen: in %s%.3f seconds",
              (timeout_msec / 1000) >= TIMEOUT_APPROX_THRESHOLD_SEC ? " about" : "",
              ((double) timeout_msec) / 1000);

        priv->timeout_expire_source = nm_g_timeout_add_source_approx(timeout_msec,
                                                                     TIMEOUT_APPROX_THRESHOLD_SEC,
                                                                     timeout_expire_cb,
                                                                     ndisc);
    }

    if (changed != NM_NDISC_CONFIG_NONE)
        nm_ndisc_emit_config_change(ndisc, changed);
}

static gboolean
timeout_expire_cb(gpointer user_data)
{
    check_timestamps(user_data, nm_utils_get_monotonic_timestamp_msec(), NM_NDISC_CONFIG_NONE);
    return G_SOURCE_CONTINUE;
}

void
nm_ndisc_ra_received(NMNDisc *ndisc, gint64 now_msec, NMNDiscConfigMap changed)
{
    NMNDiscPrivate *priv = NM_NDISC_GET_PRIVATE(ndisc);

    nm_clear_g_source_inst(&priv->ra_timeout_source);
    nm_clear_g_source(&priv->send_rs_id);
    nm_clear_g_free(&priv->last_error);
    check_timestamps(ndisc, now_msec, changed);
}

void
nm_ndisc_rs_received(NMNDisc *ndisc)
{
    NMNDiscPrivate *priv = NM_NDISC_GET_PRIVATE(ndisc);

    nm_clear_g_free(&priv->last_error);
    announce_router_solicited(ndisc);
}

/*****************************************************************************/

static void
dns_domain_free(gpointer data)
{
    g_free(((NMNDiscDNSDomain *) (data))->domain);
}

static void
set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    NMNDisc *       self = NM_NDISC(object);
    NMNDiscPrivate *priv = NM_NDISC_GET_PRIVATE(self);

    switch (prop_id) {
    case PROP_PLATFORM:
        /* construct-only */
        priv->platform = g_value_get_object(value) ?: NM_PLATFORM_GET;
        if (!priv->platform)
            g_return_if_reached();

        g_object_ref(priv->platform);

        priv->netns = nm_platform_netns_get(priv->platform);
        if (priv->netns)
            g_object_ref(priv->netns);

        g_return_if_fail(!priv->netns || priv->netns == nmp_netns_get_current());
        break;
    case PROP_IFINDEX:
        /* construct-only */
        priv->ifindex = g_value_get_int(value);
        g_return_if_fail(priv->ifindex > 0);
        break;
    case PROP_IFNAME:
        /* construct-only */
        priv->ifname = g_value_dup_string(value);
        g_return_if_fail(priv->ifname && priv->ifname[0]);
        break;
    case PROP_STABLE_TYPE:
        /* construct-only */
        priv->stable_type = g_value_get_int(value);
        break;
    case PROP_NETWORK_ID:
        /* construct-only */
        priv->network_id = g_value_dup_string(value);
        g_return_if_fail(priv->network_id);
        break;
    case PROP_ADDR_GEN_MODE:
        /* construct-only */
        priv->addr_gen_mode = g_value_get_int(value);
        break;
    case PROP_MAX_ADDRESSES:
        /* construct-only */
        priv->max_addresses = g_value_get_int(value);
        break;
    case PROP_RA_TIMEOUT:
        /* construct-only */
        priv->ra_timeout = g_value_get_uint(value);
        nm_assert(priv->ra_timeout <= NM_RA_TIMEOUT_INFINITY);
        break;
    case PROP_ROUTER_SOLICITATIONS:
        /* construct-only */
        priv->router_solicitations = g_value_get_int(value);
        break;
    case PROP_ROUTER_SOLICITATION_INTERVAL:
        /* construct-only */
        priv->router_solicitation_interval = g_value_get_int(value);
        break;
    case PROP_NODE_TYPE:
        /* construct-only */
        priv->node_type = g_value_get_int(value);
        nm_assert(NM_IN_SET(priv->node_type, NM_NDISC_NODE_TYPE_HOST, NM_NDISC_NODE_TYPE_ROUTER));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
nm_ndisc_init(NMNDisc *ndisc)
{
    NMNDiscPrivate *     priv;
    NMNDiscDataInternal *rdata;

    priv         = G_TYPE_INSTANCE_GET_PRIVATE(ndisc, NM_TYPE_NDISC, NMNDiscPrivate);
    ndisc->_priv = priv;

    rdata = &priv->rdata;

    rdata->gateways    = g_array_new(FALSE, FALSE, sizeof(NMNDiscGateway));
    rdata->addresses   = g_array_new(FALSE, FALSE, sizeof(NMNDiscAddress));
    rdata->routes      = g_array_new(FALSE, FALSE, sizeof(NMNDiscRoute));
    rdata->dns_servers = g_array_new(FALSE, FALSE, sizeof(NMNDiscDNSServer));
    rdata->dns_domains = g_array_new(FALSE, FALSE, sizeof(NMNDiscDNSDomain));
    g_array_set_clear_func(rdata->dns_domains, dns_domain_free);
    priv->rdata.public.hop_limit = 64;

    /* Start at very low number so that last_rs - router_solicitation_interval
     * is much lower than nm_utils_get_monotonic_timestamp_sec() at startup.
     */
    priv->last_rs = G_MININT32;
}

static void
dispose(GObject *object)
{
    NMNDisc *       ndisc = NM_NDISC(object);
    NMNDiscPrivate *priv  = NM_NDISC_GET_PRIVATE(ndisc);

    nm_clear_g_source_inst(&priv->ra_timeout_source);
    nm_clear_g_source(&priv->send_rs_id);
    nm_clear_g_source(&priv->send_ra_id);
    nm_clear_g_free(&priv->last_error);

    nm_clear_g_source_inst(&priv->timeout_expire_source);

    G_OBJECT_CLASS(nm_ndisc_parent_class)->dispose(object);
}

static void
finalize(GObject *object)
{
    NMNDisc *            ndisc = NM_NDISC(object);
    NMNDiscPrivate *     priv  = NM_NDISC_GET_PRIVATE(ndisc);
    NMNDiscDataInternal *rdata = &priv->rdata;

    g_free(priv->ifname);
    g_free(priv->network_id);

    g_array_unref(rdata->gateways);
    g_array_unref(rdata->addresses);
    g_array_unref(rdata->routes);
    g_array_unref(rdata->dns_servers);
    g_array_unref(rdata->dns_domains);

    g_clear_object(&priv->netns);
    g_clear_object(&priv->platform);

    G_OBJECT_CLASS(nm_ndisc_parent_class)->finalize(object);
}

static void
nm_ndisc_class_init(NMNDiscClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    g_type_class_add_private(klass, sizeof(NMNDiscPrivate));

    object_class->set_property = set_property;
    object_class->dispose      = dispose;
    object_class->finalize     = finalize;

    obj_properties[PROP_PLATFORM] =
        g_param_spec_object(NM_NDISC_PLATFORM,
                            "",
                            "",
                            NM_TYPE_PLATFORM,
                            G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    obj_properties[PROP_IFINDEX] =
        g_param_spec_int(NM_NDISC_IFINDEX,
                         "",
                         "",
                         0,
                         G_MAXINT,
                         0,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    obj_properties[PROP_IFNAME] =
        g_param_spec_string(NM_NDISC_IFNAME,
                            "",
                            "",
                            NULL,
                            G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    obj_properties[PROP_STABLE_TYPE] =
        g_param_spec_int(NM_NDISC_STABLE_TYPE,
                         "",
                         "",
                         NM_UTILS_STABLE_TYPE_UUID,
                         NM_UTILS_STABLE_TYPE_RANDOM,
                         NM_UTILS_STABLE_TYPE_UUID,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    obj_properties[PROP_NETWORK_ID] =
        g_param_spec_string(NM_NDISC_NETWORK_ID,
                            "",
                            "",
                            NULL,
                            G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    obj_properties[PROP_ADDR_GEN_MODE] =
        g_param_spec_int(NM_NDISC_ADDR_GEN_MODE,
                         "",
                         "",
                         NM_SETTING_IP6_CONFIG_ADDR_GEN_MODE_EUI64,
                         NM_SETTING_IP6_CONFIG_ADDR_GEN_MODE_STABLE_PRIVACY,
                         NM_SETTING_IP6_CONFIG_ADDR_GEN_MODE_EUI64,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    obj_properties[PROP_MAX_ADDRESSES] =
        g_param_spec_int(NM_NDISC_MAX_ADDRESSES,
                         "",
                         "",
                         0,
                         G_MAXINT32,
                         NM_NDISC_MAX_ADDRESSES_DEFAULT,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    G_STATIC_ASSERT_EXPR(G_MAXINT32 == NM_RA_TIMEOUT_INFINITY);
    obj_properties[PROP_RA_TIMEOUT] =
        g_param_spec_uint(NM_NDISC_RA_TIMEOUT,
                          "",
                          "",
                          0,
                          G_MAXINT32,
                          0,
                          G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    obj_properties[PROP_ROUTER_SOLICITATIONS] =
        g_param_spec_int(NM_NDISC_ROUTER_SOLICITATIONS,
                         "",
                         "",
                         1,
                         G_MAXINT32,
                         NM_NDISC_ROUTER_SOLICITATIONS_DEFAULT,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    obj_properties[PROP_ROUTER_SOLICITATION_INTERVAL] =
        g_param_spec_int(NM_NDISC_ROUTER_SOLICITATION_INTERVAL,
                         "",
                         "",
                         1,
                         G_MAXINT32,
                         NM_NDISC_ROUTER_SOLICITATION_INTERVAL_DEFAULT,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    obj_properties[PROP_NODE_TYPE] =
        g_param_spec_int(NM_NDISC_NODE_TYPE,
                         "",
                         "",
                         NM_NDISC_NODE_TYPE_INVALID,
                         NM_NDISC_NODE_TYPE_ROUTER,
                         NM_NDISC_NODE_TYPE_INVALID,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties(object_class, _PROPERTY_ENUMS_LAST, obj_properties);

    signals[CONFIG_RECEIVED]   = g_signal_new(NM_NDISC_CONFIG_RECEIVED,
                                            G_OBJECT_CLASS_TYPE(klass),
                                            G_SIGNAL_RUN_FIRST,
                                            0,
                                            NULL,
                                            NULL,
                                            NULL,
                                            G_TYPE_NONE,
                                            2,
                                            G_TYPE_POINTER,
                                            G_TYPE_UINT);
    signals[RA_TIMEOUT_SIGNAL] = g_signal_new(NM_NDISC_RA_TIMEOUT_SIGNAL,
                                              G_OBJECT_CLASS_TYPE(klass),
                                              G_SIGNAL_RUN_FIRST,
                                              0,
                                              NULL,
                                              NULL,
                                              NULL,
                                              G_TYPE_NONE,
                                              0);
}
