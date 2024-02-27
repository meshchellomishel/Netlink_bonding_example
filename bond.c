#include <netlink/route/link/bonding.h>
#include <netlink/route/link.h>
#include <netlink/route/link/bridge.h>
#include <netlink/netlink.h>
#include <netlink/handlers.h>
#include <netlink/cache.h>
#include <netlink/errno.h>
#include <netlink/types.h>
#include <netlink/msg.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <linux/types.h>
#include <errno.h>
#include <time.h>


#define LAG_MODE_LOADBALANCE 3
#define LAG_MODE_LACP 4
#define LAG_DEFAULT_MIIMON 100
#define BOND_TYPE "bond"

#define BOND_MAX_FULL_STATUS 21
#define BOND_MAX_ORIGINAL_STATUS 10

#define NLMSG_TAIL(nmsg) \
	((struct rtattr *) (((void *) (nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

enum rx_state {
	AD_RX_DUMMY,
	AD_RX_INITIALIZE,	/* rx Machine */
	AD_RX_PORT_DISABLED,	/* rx Machine */
	AD_RX_LACP_DISABLED,	/* rx Machine */
	AD_RX_EXPIRED,		/* rx Machine */
	AD_RX_DEFAULTED,	/* rx Machine */
	AD_RX_CURRENT		/* rx Machine */
};

struct bond {
	struct nl_sock *sock;
	struct nl_cache *cache;
	struct nl_sock *evcb;
};


struct bond_req {
	struct nlmsghdr n;
	struct ifinfomsg i;
};


static struct bond *bond_alloc(void)
{
	struct bond *bond = NULL;

	bond = malloc(sizeof(struct bond));
	if (!bond)
		return NULL;

	bond->sock = NULL;
	bond->cache = NULL;

	return bond;
}

const char *state2str(enum rx_state state)
{
	switch (state) {
	case AD_RX_DUMMY:
		return "dummy";
	case AD_RX_INITIALIZE:
		return "initialize";
	case AD_RX_PORT_DISABLED:
		return "port disabled";
	case AD_RX_LACP_DISABLED:
		return "lacp disabled";
	case AD_RX_EXPIRED:
		return "expired";
	case AD_RX_DEFAULTED:
		return "default";
	case AD_RX_CURRENT:
		return "current";
	}

	return "unknown";
}

void printAddr(unsigned char *addr)
{
	for (int i = 0; i < 5; i++)
		printf("%02x:", addr[i]);
	printf("%02x\n", addr[5]);
}

void parse_slave(struct nlattr *linkinfo)
{
	int ret;
	struct nlattr *slave[IFLA_BOND_SLAVE_MAX+1];

	if (!linkinfo)
		return;

	ret = nla_parse_nested(&slave, IFLA_BOND_SLAVE_MAX, linkinfo, NULL);
	if (ret < 0) {
		printf("Failed to parse linkinfo\n");
		return;
	}

	if (slave[IFLA_BOND_SLAVE_IFNAME])
		printf("[%s]\n", nla_get_string(slave[IFLA_BOND_SLAVE_IFNAME]));

	printf("\t[ACTOR]\n");

	if (slave[IFLA_BOND_SLAVE_AD_ACTOR_OPER_PORT_STATE])
		printf("\t\tActor port state: %d\n", nla_get_u16(slave[IFLA_BOND_SLAVE_AD_ACTOR_OPER_PORT_STATE]));

	if (IFLA_BOND_SLAVE_MAX+1 == BOND_MAX_FULL_STATUS) {
		if (slave[IFLA_BOND_SLAVE_AD_INFO_ACTOR_KEY])
			printf("\t\tActor key: %d\n", nla_get_u16(slave[IFLA_BOND_SLAVE_AD_INFO_ACTOR_KEY]));

		if (slave[IFLA_BOND_SLAVE_AD_ACTOR_PORT_NUM])
			printf("\t\tActor port num: %d\n", nla_get_u16(slave[IFLA_BOND_SLAVE_AD_ACTOR_PORT_NUM]));

		if (slave[IFLA_BOND_SLAVE_AD_RX_PORT_STATE])
			printf("\t\tActor rx_state: %s\n", state2str(nla_get_u16(slave[IFLA_BOND_SLAVE_AD_RX_PORT_STATE])));

		printf("\t[PARTNER]\n");

		if (slave[IFLA_BOND_SLAVE_AD_PARTNER_OPER_SYSTEM_ID]) {
			printf("\t\tPartner system id: ");
			printAddr(RTA_DATA(slave[IFLA_BOND_SLAVE_AD_PARTNER_OPER_SYSTEM_ID]));
		}

		if (slave[IFLA_BOND_SLAVE_AD_PATNER_OPER_KEY])
			printf("\t\tPartner key: %d\n", nla_get_u16(slave[IFLA_BOND_SLAVE_AD_PATNER_OPER_KEY]));

		if (slave[ILFA_BOND_SLAVE_AD_PARTNER_OPER_SYSTEM_PRIO])
			printf("\t\tPartner system priority: %d\n", nla_get_u16(slave[ILFA_BOND_SLAVE_AD_PARTNER_OPER_SYSTEM_PRIO]));

		if (slave[IFLA_BOND_SLAVE_PARTNER_PORT_NUM])
			printf("\t\tPartner port num: %d\n", nla_get_u16(slave[IFLA_BOND_SLAVE_PARTNER_PORT_NUM]));

		if (slave[IFLA_BOND_SLAVE_PARTNER_OPER_PORT_PRIO])
			printf("\t\tPartner port prio: %d\n", nla_get_u16(slave[IFLA_BOND_SLAVE_PARTNER_OPER_PORT_PRIO]));

	} else
		printf("Full status not supported\n");

	if (slave[IFLA_BOND_SLAVE_AD_PARTNER_OPER_PORT_STATE])
		printf("\t\tPartner port state: %d\n", nla_get_u16(slave[IFLA_BOND_SLAVE_AD_PARTNER_OPER_PORT_STATE]));
}

void parse_attrs(struct nl_msg *msg)
{
	int ret, i;
	struct nlmsghdr *h = nlmsg_hdr(msg);
	struct nlattr *tb[IFLA_MAX + 1], *linkinfo[IFLA_INFO_MAX + 1], *bond[IFLA_BOND_MAX + 1], *port;

	ret = nlmsg_parse(h, sizeof(struct ifinfomsg), &tb, IFLA_MAX, NULL);
	if (ret < 0) {
		printf("Failed to parse args\n");
		return;
	}

	if (!tb[IFLA_LINKINFO])
		return;

	if (tb[IFLA_IFNAME])
		printf("\n\nName: %s\n", nla_get_string(tb[IFLA_IFNAME]));
	if (tb[IFLA_CARRIER])
		printf("Oper-Status: %s\n", nla_get_u8(tb[IFLA_CARRIER]) ? "Up" : "Down");
	// printf("\tActor system id: ");
	// if (tb[IFLA_ADDRESS]) {
	// 	unsigned char *mac;

	// 	mac = RTA_DATA(tb[IFLA_ADDRESS]);
	// 	printAddr(mac);
	// } else
	// 	printf("no info\n");

	ret = nla_parse_nested(&linkinfo, IFLA_INFO_MAX, tb[IFLA_LINKINFO], NULL);
	if (ret < 0) {
		printf("Failed to parse linkinfo\n");
		return;
	}

	/*
	IFLA_BOND_SLAVE_AD_INFO_ACTOR_KEY,
	ILFA_BOND_SLAVE_AD_PARTNER_OPER_SYSTEM_PRIO,
	IFLA_BOND_SLAVE_AD_PARTNER_OPER_SYSTEM_ID,
	IFLA_BOND_SLAVE_AD_PATNER_OPER_KEY,
	IFLA_BOND_SLAVE_AD_ACTOR_PORT_NUM,
	IFLA_BOND_SLAVE_PARTNER_PORT_NUM,
	IFLA_BOND_SLAVE_PARTNER_OPER_PORT_PRIO,
	IFLA_BOND_SLAVE_AD_RX_PORT_STATE,
	*/

	if (linkinfo[IFLA_INFO_KIND]) {
		if (linkinfo[IFLA_INFO_DATA]) {
			ret = nla_parse_nested(&bond, IFLA_BOND_MAX, linkinfo[IFLA_INFO_DATA], NULL);
			if (ret < 0) {
				printf("Failed to parse linkinfo\n");
				return;
			}

			if (bond[IFLA_BOND_SLAVE_LIST]) {
				nla_for_each_nested(port, bond[IFLA_BOND_SLAVE_LIST], i) {
					parse_slave(port);
				}
			} else
				printf("No port list(\n");

		} else
			printf("NO INFO bond\n");
	}
}

int bond_modify_cb(struct nl_msg *msg, void *arg)
{
	parse_attrs(msg);

	return NL_SKIP;
}

static int set_socket(struct bond *bond)
{
	int ret;

	bond->sock = nl_socket_alloc();
	if (!bond->sock) {
		printf("ERROR: Failed to create a nl_socket\n");
		return -1;
	}

	bond->evcb = nl_socket_alloc();
	if (!bond->evcb) {
		printf("ERROR: Failed to create event socket\n");
		return -1;
	}

	nl_socket_set_peer_groups(bond->sock, RTMGRP_LINK);
	ret = nl_socket_modify_cb(bond->sock, NL_CB_VALID, NL_CB_CUSTOM, bond_modify_cb, bond);
	if (ret < 0) {
		printf("ERROR: Failed to set callback function to socket\n");
		return ret;
	}

	ret = nl_connect(bond->evcb, NETLINK_ROUTE);
	if (ret < 0) {
		printf("ERROR: Failed to connect to event socket\n");
		return ret;
	}

	ret = nl_connect(bond->sock, NETLINK_ROUTE);
	if (ret < 0) {
		printf("ERROR: Failed to connect to nl socket\n");
		return ret;
	}

	ret = nl_socket_set_nonblocking(bond->sock);
	if (ret < 0) {
		printf("ERROR: Failed to set nl socket into non blocking\n");
		return ret;
	}

	return 0;
}


static int set_cahce(struct bond *bond)
{
	int ret;

	ret = rtnl_link_alloc_cache_flags(bond->sock, AF_UNSPEC, &bond->cache, NL_CACHE_AF_ITER);
	if (ret < 0) {
		printf("ERROR: Failed to alloc link cache\n");
		return ret;
	}

	return 0;
}

int bond_init(struct bond *bond)
{
	int ret;

	ret = set_socket(bond);
	if (ret < 0) {
		printf("ERROR: failed to set socket\n");
		return ret;
	}

	return set_cahce(bond);
}

void bond_destroy(struct bond *bond)
{
	if (bond) {
		if (bond->sock)
			nl_socket_free(bond->sock);
		if (bond->cache)
			nl_cache_free(bond->cache);
		if (bond->evcb)
			nl_socket_free(bond->evcb);
		free(bond);
	}
}

struct rtnl_link *build_bond_by_ifname(const char *ifname)
{
	struct rtnl_link *link = NULL;

	link = rtnl_link_bond_alloc();
	if (!link) {
		printf("ERROR: failed to allocate link\n");
		rtnl_link_put(link);
		return NULL;
	}
	rtnl_link_set_name(link, ifname);

	return link;
}

struct nl_msg *build_msg(int nlmsg_type, int flags, struct rtnl_link *link)
{
	int ret;
	int ifindex = 0;

	if (link)
		ifindex = rtnl_link_get_ifindex(link);
	printf("ifindex: %d\n", ifindex);
	struct nl_msg *msg = NULL;
	struct ifinfomsg ifi = {
		.ifi_family = AF_UNSPEC,
		.ifi_index = ifindex,
	};

	msg = nlmsg_alloc_simple(nlmsg_type, flags);
	if (!msg)
		return NULL;

	ret = nlmsg_append(msg, &ifi, sizeof(ifi), NLMSG_ALIGNTO);
	if (ret < 0)
		return NULL;

	return msg;
}

int _fill_default_info(struct nl_msg *msg, const char *ifname)
{
	int ret;
	int iflatype;
	struct rtattr *linkinfo, *data;
	char *type = BOND_TYPE;

	nla_put_string(msg, IFLA_IFNAME, ifname);

	linkinfo = nla_nest_start(msg, IFLA_LINKINFO);
	{
		nla_put_string(msg, IFLA_INFO_KIND, type);

		data = nla_nest_start(msg, IFLA_INFO_DATA);
		{
			nla_put_u8(msg, IFLA_BOND_MODE, LAG_MODE_LACP);
			nla_put_u32(msg, IFLA_BOND_MIIMON, LAG_DEFAULT_MIIMON);
			nla_put_u8(msg, IFLA_BOND_AD_SELECT, 3);
		}
		nla_nest_end(msg, data);
	}
	nla_nest_end(msg, linkinfo);

	return 0;
}

int _fill_default_slave_info(struct nl_msg *msg, const char *ifname)
{
	int ret;
	int iflatype;
	struct rtattr *linkinfo, *data;
	char *type = BOND_TYPE;

	linkinfo = nla_nest_start(msg, IFLA_LINKINFO);
	{
		nla_put_string(msg, IFLA_INFO_KIND, "bond");

		data = nla_nest_start(msg, IFLA_INFO_SLAVE_DATA);
		{
			nla_put_u16(msg, IFLA_BOND_SLAVE_AD_ACTOR_PORT_PRIO, 64464);
		}
		nla_nest_end(msg, data);
	}
	nla_nest_end(msg, linkinfo);

	return 0;
}

int _enslave_iface(struct bond *bond, struct rtnl_link *slave, int master)
{
	int ret;
	struct rtnl_link *changes = NULL;
	int slave_index = rtnl_link_get_ifindex(slave);
	unsigned int flags = rtnl_link_get_flags(slave);

	changes = rtnl_link_alloc();
	if (!changes) {
		printf("ERROR: Failed to allocate change link\n");
		return -1;
	}

	if (flags & IFF_UP)
		rtnl_link_unset_flags(changes, IFF_UP);
	ret = rtnl_link_change(bond->sock, slave, changes, 0);
	if (ret < 0) {
		printf("ERROR: Failed to apply changes on iface\n");
		return ret;
	}

	printf("Enslaving %d to %d...\n", slave_index, master);
	return rtnl_link_bond_enslave_ifindex(bond->sock, master, slave_index);
}

int enslave_iface(struct bond *bond, const char *master_name, const char *slave_name)
{
	int ret;
	int master = -1;
	struct rtnl_link *slave = NULL;

	master = rtnl_link_name2i(bond->cache, master_name);
	if (master < 0) {
		printf("ERROR: Failed to get master`s link by name\n");
		return -1;
	}
	slave = rtnl_link_get_by_name(bond->cache, slave_name);
	if (!slave) {
		printf("ERROR: Failed to get slave`s link by name\n");
		return -1;
	}

	ret = _enslave_iface(bond, slave, master);
	if (ret < 0)
		printf("ERROR: Failed to enslave iface\n");

	rtnl_link_put(slave);
	return ret;
}

int nl_talk(struct bond *bond, struct nl_msg *msg)
{
	int ret;

	ret = nl_send_auto(bond->sock, msg);
	if (ret < 0) {
		printf("ERROR: Failed to send msg(%s)\n", nl_geterror(ret));
		return ret;
	}

	ret = nl_recvmsgs_default(bond->sock);
	if (ret < 0) {
		printf("ERROR: Failed to recv NL mesgs: %s\n", nl_geterror(ret));
		return ret;
	}

	return 0;
}

int set_port_prio(struct bond *bond, const char *iface_name, int port_prio)
{
	int ret;
	struct nl_cache *cache = NULL;
	struct link *link = NULL;
	struct nl_msg *msg = NULL;

	link = rtnl_link_get_by_name(bond->cache, iface_name);
	if (!link) {
		printf("ERROR: Failed to get iface link\n");
		ret = -1;
		goto link_put;
	}

	msg = build_msg(RTM_NEWLINK, NLM_F_REQUEST, link);
	if (!msg) {
		printf("ERROR: Failed to build msg\n");
		ret = -1;
		goto msg_free;
	}

	ret = _fill_default_slave_info(msg, iface_name);
	if (ret < 0) {
		printf("ERROR: Failed to fill slave info\n");
		ret = -1;
		goto msg_free;
	}

	ret = nl_talk(bond, msg);

msg_free:
	nlmsg_free(msg);
link_put:
	rtnl_link_put(link);

	return ret;
}

int create_bond(struct bond *bond, const char *ifname)
{
	int ret;
	struct nl_msg *msg;
	struct rtnl_link *link = NULL;

	link = build_bond_by_ifname(ifname);
	if (!link) {
		printf("ERROR: Failed to create bond iface\n");
		return -1;
	}
	msg = build_msg(RTM_NEWLINK, NLM_F_REQUEST | NLM_F_CREATE, link);
	if (!msg) {
		printf("ERROR: Failed to create msg\n");
		return -1;
	}
	ret = _fill_default_info(msg, ifname);
	if (ret < 0) {
		printf("ERROR: Failed to fill default info\n");
		return ret;
	}
	ret = nl_talk(bond, msg);

	rtnl_link_put(link);
	return ret;
}

struct nl_msg *get_bond(struct bond *bond, const char *ifname)
{
	struct nl_msg *msg;
	struct rtnl_link *link = NULL;

	link = rtnl_link_get_by_name(bond->cache, ifname);
	if (!link) {
		printf("ERROR: Failed to found iface\n");
		return NULL;
	}

	msg = build_msg(RTM_GETLINK, NLM_F_REQUEST, link);
	if (!msg) {
		printf("ERROR: Failed to create msg\n");
		return NULL;
	}

	rtnl_link_put(link);
	return msg;
}

int delete_bond(struct bond *bond, const char *ifname)
{
	int ret;
	struct nl_msg *msg = NULL;
	struct rtnl_link *link = NULL;

	ret = set_cahce(bond);
	if (ret < 0) {
		printf("[ERROR]: Failed to set cache\n");
		return -1;
	}

	link = rtnl_link_get_by_name(bond->cache, ifname);
	if (!link) {
		printf("ERROR: Failed to found iface\n");
		return 0;
	}
	msg = build_msg(RTM_DELLINK, NLM_F_REQUEST, link);
	ret = nl_talk(bond, msg);

	rtnl_link_put(link);
	return ret;
}

void get_state(struct bond *bond, const char *ifname)
{
	ssize_t status;
	struct nl_cb *cb;
	struct nl_msg *msg = NULL;

	msg = get_bond(bond, ifname);
	if (!msg) {
		printf("[ERROR]: Failed to get msg\n");
		return;
	}

	status = nl_send_auto(bond->sock, msg);
	if (status < 0) {
		printf("[ERROR]: Failed to send get request (%s)\n", strerror(-status));
		return;
	}

	cb = nl_socket_get_cb(bond->sock);
	do {
		status = nl_recvmsgs_report(bond->sock, cb);
	} while (status > 0);
	nl_cb_put(cb);

	nlmsg_free(msg);
}

int main(void)
{
	int ret;
	struct bond *bond = NULL;

	bond = bond_alloc();
	if (!bond) {
		printf("[ERROR]: Failed to allocate bond\n");
		return -1;
	}

	ret = bond_init(bond);
	if (ret < 0) {
		printf("[ERROR]: Failed to init bond\n");
		goto on_error;
	}

	ret = create_bond(bond, "bond0");
	if (ret < 0)
		printf("ERROR: Failed to create bond\n");

	ret = set_port_prio(bond, "eth21", 254);
	if (ret < 0)
		printf("ERROR: Failed to set port prio\n");

	// читаем и разбираем сообщения из сокета
	/*while (1) {
		clock_t t, t0;

		sleep(3);
		t0 = clock();

		get_state(bond, "bond0");

		t = clock();
		printf("get info from one port: %f\n", (double)(t - t0)/CLOCKS_PER_SEC);
	}*/

on_error:
	bond_destroy(bond);
	return ret;
}
