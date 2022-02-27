/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "networkd-address.h"
#include "networkd-address-label.h"
#include "networkd-bridge-fdb.h"
#include "networkd-bridge-mdb.h"
#include "networkd-dhcp-server.h"
#include "networkd-dhcp4.h"
#include "networkd-dhcp6.h"
#include "networkd-ipv6-proxy-ndp.h"
#include "networkd-manager.h"
#include "networkd-ndisc.h"
#include "networkd-neighbor.h"
#include "networkd-nexthop.h"
#include "networkd-route.h"
#include "networkd-routing-policy-rule.h"
#include "networkd-queue.h"
#include "networkd-setlink.h"
#include "qdisc.h"
#include "tclass.h"

static void request_free_object(RequestType type, void *object) {
        switch (type) {
        case REQUEST_TYPE_ACTIVATE_LINK:
                break;
        case REQUEST_TYPE_ADDRESS:
                address_free(object);
                break;
        case REQUEST_TYPE_ADDRESS_LABEL:
                address_label_free(object);
                break;
        case REQUEST_TYPE_BRIDGE_FDB:
                bridge_fdb_free(object);
                break;
        case REQUEST_TYPE_BRIDGE_MDB:
                bridge_mdb_free(object);
                break;
        case REQUEST_TYPE_DHCP_SERVER:
        case REQUEST_TYPE_DHCP4_CLIENT:
        case REQUEST_TYPE_DHCP6_CLIENT:
                break;
        case REQUEST_TYPE_IPV6_PROXY_NDP:
                free(object);
                break;
        case REQUEST_TYPE_NDISC:
                break;
        case REQUEST_TYPE_NEIGHBOR:
                neighbor_free(object);
                break;
        case REQUEST_TYPE_NETDEV_INDEPENDENT:
        case REQUEST_TYPE_NETDEV_STACKED:
                netdev_unref(object);
                break;
        case REQUEST_TYPE_NEXTHOP:
                nexthop_free(object);
                break;
        case REQUEST_TYPE_RADV:
                break;
        case REQUEST_TYPE_ROUTE:
                route_free(object);
                break;
        case REQUEST_TYPE_ROUTING_POLICY_RULE:
                routing_policy_rule_free(object);
                break;
        case REQUEST_TYPE_SET_LINK:
                break;
        case REQUEST_TYPE_TC_QDISC:
                qdisc_free(object);
                break;
        case REQUEST_TYPE_TC_CLASS:
                tclass_free(object);
                break;
        case REQUEST_TYPE_UP_DOWN:
                break;
        default:
                assert_not_reached();
        }
}

static Request *request_free(Request *req) {
        if (!req)
                return NULL;

        if (req->link && req->link->manager)
                /* To prevent from triggering assertions in hash functions, remove this request before
                 * freeing object below. */
                ordered_set_remove(req->link->manager->request_queue, req);
        if (req->consume_object)
                request_free_object(req->type, req->object);
        link_unref(req->link);

        return mfree(req);
}

DEFINE_TRIVIAL_REF_UNREF_FUNC(Request, request, request_free);

void request_drop(Request *req) {
        if (!req)
                return;

        if (req->message_counter)
                (*req->message_counter)--;

        request_unref(req);
}

static void request_hash_func(const Request *req, struct siphash *state) {
        assert(req);
        assert(state);

        siphash24_compress_boolean(req->link, state);
        if (req->link)
                siphash24_compress(&req->link->ifindex, sizeof(req->link->ifindex), state);

        siphash24_compress(&req->type, sizeof(req->type), state);

        switch (req->type) {
        case REQUEST_TYPE_ACTIVATE_LINK:
                break;
        case REQUEST_TYPE_ADDRESS:
                address_hash_func(req->address, state);
                break;
        case REQUEST_TYPE_ADDRESS_LABEL:
        case REQUEST_TYPE_BRIDGE_FDB:
        case REQUEST_TYPE_BRIDGE_MDB:
        case REQUEST_TYPE_NETDEV_INDEPENDENT:
        case REQUEST_TYPE_NETDEV_STACKED:
                /* TODO: Currently, these types do not have any specific hash and compare functions.
                 * Fortunately, all these objects are 'static', thus we can use the trivial functions. */
                trivial_hash_func(req->object, state);
                break;
        case REQUEST_TYPE_DHCP_SERVER:
        case REQUEST_TYPE_DHCP4_CLIENT:
        case REQUEST_TYPE_DHCP6_CLIENT:
                /* These types do not have an object. */
                break;
        case REQUEST_TYPE_IPV6_PROXY_NDP:
                in6_addr_hash_func(req->ipv6_proxy_ndp, state);
                break;
        case REQUEST_TYPE_NDISC:
                /* This type does not have an object. */
                break;
        case REQUEST_TYPE_NEIGHBOR:
                neighbor_hash_func(req->neighbor, state);
                break;
        case REQUEST_TYPE_NEXTHOP:
                nexthop_hash_func(req->nexthop, state);
                break;
        case REQUEST_TYPE_RADV:
                /* This type does not have an object. */
                break;
        case REQUEST_TYPE_ROUTE:
                route_hash_func(req->route, state);
                break;
        case REQUEST_TYPE_ROUTING_POLICY_RULE:
                routing_policy_rule_hash_func(req->rule, state);
                break;
        case REQUEST_TYPE_SET_LINK:
                trivial_hash_func(req->set_link_operation_ptr, state);
                break;
        case REQUEST_TYPE_TC_QDISC:
                qdisc_hash_func(req->qdisc, state);
                break;
        case REQUEST_TYPE_TC_CLASS:
                tclass_hash_func(req->tclass, state);
                break;
        case REQUEST_TYPE_UP_DOWN:
                break;
        default:
                assert_not_reached();
        }
}

static int request_compare_func(const struct Request *a, const struct Request *b) {
        int r;

        assert(a);
        assert(b);

        r = CMP(!!a->link, !!b->link);
        if (r != 0)
                return r;

        if (a->link) {
                r = CMP(a->link->ifindex, b->link->ifindex);
                if (r != 0)
                        return r;
        }

        r = CMP(a->type, b->type);
        if (r != 0)
                return r;

        switch (a->type) {
        case REQUEST_TYPE_ACTIVATE_LINK:
                return 0;
        case REQUEST_TYPE_ADDRESS:
                return address_compare_func(a->address, b->address);
        case REQUEST_TYPE_ADDRESS_LABEL:
        case REQUEST_TYPE_BRIDGE_FDB:
        case REQUEST_TYPE_BRIDGE_MDB:
        case REQUEST_TYPE_NETDEV_INDEPENDENT:
        case REQUEST_TYPE_NETDEV_STACKED:
                return trivial_compare_func(a->object, b->object);
        case REQUEST_TYPE_DHCP_SERVER:
        case REQUEST_TYPE_DHCP4_CLIENT:
        case REQUEST_TYPE_DHCP6_CLIENT:
                return 0;
        case REQUEST_TYPE_IPV6_PROXY_NDP:
                return in6_addr_compare_func(a->ipv6_proxy_ndp, b->ipv6_proxy_ndp);
        case REQUEST_TYPE_NDISC:
                return 0;
        case REQUEST_TYPE_NEIGHBOR:
                return neighbor_compare_func(a->neighbor, b->neighbor);
        case REQUEST_TYPE_NEXTHOP:
                return nexthop_compare_func(a->nexthop, b->nexthop);
        case REQUEST_TYPE_ROUTE:
                return route_compare_func(a->route, b->route);
        case REQUEST_TYPE_RADV:
                return 0;
        case REQUEST_TYPE_ROUTING_POLICY_RULE:
                return routing_policy_rule_compare_func(a->rule, b->rule);
        case REQUEST_TYPE_SET_LINK:
                return trivial_compare_func(a->set_link_operation_ptr, b->set_link_operation_ptr);
        case REQUEST_TYPE_TC_QDISC:
                return qdisc_compare_func(a->qdisc, b->qdisc);
        case REQUEST_TYPE_TC_CLASS:
                return tclass_compare_func(a->tclass, b->tclass);
        case REQUEST_TYPE_UP_DOWN:
                return 0;
        default:
                assert_not_reached();
        }
}

DEFINE_PRIVATE_HASH_OPS_WITH_KEY_DESTRUCTOR(
                request_hash_ops,
                Request,
                request_hash_func,
                request_compare_func,
                request_unref);

int netdev_queue_request(
                NetDev *netdev,
                Request **ret) {

        _cleanup_(request_unrefp) Request *req = NULL;
        Request *existing;
        int r;

        assert(netdev);
        assert(netdev->manager);

        req = new(Request, 1);
        if (!req)
                return -ENOMEM;

        *req = (Request) {
                .n_ref = 1,
                .netdev = netdev_ref(netdev),
                .type = REQUEST_TYPE_NETDEV_INDEPENDENT,
                .consume_object = true,
        };

        existing = ordered_set_get(netdev->manager->request_queue, req);
        if (existing) {
                /* To prevent from removing the existing request. */
                req->netdev = netdev_unref(req->netdev);

                if (ret)
                        *ret = existing;
                return 0;
        }

        r = ordered_set_ensure_put(&netdev->manager->request_queue, &request_hash_ops, req);
        if (r < 0)
                return r;

        if (ret)
                *ret = req;

        TAKE_PTR(req);
        return 1;
}

int link_queue_request(
                Link *link,
                RequestType type,
                void *object,
                bool consume_object,
                unsigned *message_counter,
                link_netlink_message_handler_t netlink_handler,
                Request **ret) {

        _cleanup_(request_unrefp) Request *req = NULL;
        Request *existing;
        int r;

        assert(link);
        assert(link->manager);
        assert(type >= 0 && type < _REQUEST_TYPE_MAX);
        assert(IN_SET(type,
                      REQUEST_TYPE_ACTIVATE_LINK,
                      REQUEST_TYPE_DHCP_SERVER,
                      REQUEST_TYPE_DHCP4_CLIENT,
                      REQUEST_TYPE_DHCP6_CLIENT,
                      REQUEST_TYPE_NDISC,
                      REQUEST_TYPE_RADV,
                      REQUEST_TYPE_SET_LINK,
                      REQUEST_TYPE_UP_DOWN) ||
               object);
        assert(IN_SET(type,
                      REQUEST_TYPE_DHCP_SERVER,
                      REQUEST_TYPE_DHCP4_CLIENT,
                      REQUEST_TYPE_DHCP6_CLIENT,
                      REQUEST_TYPE_NDISC,
                      REQUEST_TYPE_RADV) ||
               netlink_handler);

        req = new(Request, 1);
        if (!req) {
                if (consume_object)
                        request_free_object(type, object);
                return -ENOMEM;
        }

        *req = (Request) {
                .n_ref = 1,
                .link = link_ref(link),
                .type = type,
                .object = object,
                .consume_object = consume_object,
                .message_counter = message_counter,
                .netlink_handler = netlink_handler,
        };

        existing = ordered_set_get(link->manager->request_queue, req);
        if (existing) {
                /* To prevent from removing the existing request. */
                req->link = link_unref(req->link);

                if (ret)
                        *ret = existing;
                return 0;
        }

        r = ordered_set_ensure_put(&link->manager->request_queue, &request_hash_ops, req);
        if (r < 0)
                return r;

        if (req->message_counter)
                (*req->message_counter)++;

        if (ret)
                *ret = req;

        TAKE_PTR(req);
        return 1;
}

int manager_process_requests(sd_event_source *s, void *userdata) {
        Manager *manager = userdata;
        int r;

        assert(manager);

        for (;;) {
                bool processed = false;
                Request *req;

                ORDERED_SET_FOREACH(req, manager->request_queue) {
                        switch (req->type) {
                        case REQUEST_TYPE_ACTIVATE_LINK:
                                r = link_process_activation(req, req->link, req->userdata);
                                break;
                        case REQUEST_TYPE_ADDRESS:
                                r = address_process_request(req, req->link, req->address);
                                break;
                        case REQUEST_TYPE_ADDRESS_LABEL:
                                r = address_label_process_request(req, req->link, req->label);
                                break;
                        case REQUEST_TYPE_BRIDGE_FDB:
                                r = bridge_fdb_process_request(req, req->link, req->fdb);
                                break;
                        case REQUEST_TYPE_BRIDGE_MDB:
                                r = bridge_mdb_process_request(req, req->link, req->mdb);
                                break;
                        case REQUEST_TYPE_DHCP_SERVER:
                                r = dhcp_server_process_request(req, req->link, NULL);
                                break;
                        case REQUEST_TYPE_DHCP4_CLIENT:
                                r = dhcp4_process_request(req, req->link, NULL);
                                break;
                        case REQUEST_TYPE_DHCP6_CLIENT:
                                r = dhcp6_process_request(req, req->link, NULL);
                                break;
                        case REQUEST_TYPE_IPV6_PROXY_NDP:
                                r = ipv6_proxy_ndp_address_process_request(req, req->link, req->ipv6_proxy_ndp);
                                break;
                        case REQUEST_TYPE_NDISC:
                                r = ndisc_process_request(req, req->link, NULL);
                                break;
                        case REQUEST_TYPE_NEIGHBOR:
                                r = neighbor_process_request(req, req->link, req->neighbor);
                                break;
                        case REQUEST_TYPE_NETDEV_INDEPENDENT:
                                r = independent_netdev_process_request(req, req->link, req->netdev);
                                break;
                        case REQUEST_TYPE_NETDEV_STACKED:
                                r = stacked_netdev_process_request(req, req->link, req->netdev);
                                break;
                        case REQUEST_TYPE_NEXTHOP:
                                r = nexthop_process_request(req, req->link, req->nexthop);
                                break;
                        case REQUEST_TYPE_RADV:
                                r = radv_process_request(req, req->link, NULL);
                                break;
                        case REQUEST_TYPE_ROUTE:
                                r = route_process_request(req, req->link, req->route);
                                break;
                        case REQUEST_TYPE_ROUTING_POLICY_RULE:
                                r = routing_policy_rule_process_request(req, req->link, req->rule);
                                break;
                        case REQUEST_TYPE_SET_LINK:
                                r = request_process_set_link(req);
                                break;
                        case REQUEST_TYPE_TC_QDISC:
                                r = qdisc_process_request(req, req->link, NULL);
                                break;
                        case REQUEST_TYPE_TC_CLASS:
                                r = tclass_process_request(req, req->link, NULL);
                                break;
                        case REQUEST_TYPE_UP_DOWN:
                                r = link_process_up_or_down(req, req->link, req->userdata);
                                break;
                        default:
                                return -EINVAL;
                        }
                        if (r < 0) {
                                if (req->link)
                                        link_enter_failed(req->link);
                        } else if (r > 0) {
                                ordered_set_remove(manager->request_queue, req);
                                request_unref(req);
                                processed = true;
                        }
                }

                if (!processed)
                        break;
        }

        return 0;
}
