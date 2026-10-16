#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"
#include "netwatch.h"

#define MOD "netwatch"

// <linux/if.h> has it but clashes with <net/if.h>; the kernel ABI value
#ifndef IFF_LOWER_UP
#define IFF_LOWER_UP (1 << 16)
#endif

static int nl_fd = -1;
static struct event *nl_ev;
static netwatch_cb watch_cb;
static void *watch_arg;

static void
addr_changed(const struct nlmsghdr *nh, bool removed)
{
  const struct ifaddrmsg *ifa = NLMSG_DATA(nh);
  int len = (int)IFA_PAYLOAD(nh);
  char addr[INET6_ADDRSTRLEN] = "";

  for (const struct rtattr *rta = IFA_RTA(ifa); RTA_OK(rta, len); rta = RTA_NEXT(rta, len))
    {
      // IFA_LOCAL is the address itself on point-to-point links, IFA_ADDRESS
      // everywhere else
      if (rta->rta_type != IFA_ADDRESS && rta->rta_type != IFA_LOCAL)
        continue;
      if (!inet_ntop(ifa->ifa_family, RTA_DATA(rta), addr, sizeof(addr)))
        continue;
      if (rta->rta_type == IFA_LOCAL)
        break;
    }
  if (!addr[0])
    return;

  log_debug(MOD, "Address %s %s interface %d", addr, removed ? "removed from" : "added to", ifa->ifa_index);
  watch_cb(removed ? NETWATCH_ADDR_GONE : NETWATCH_ADDR_NEW, addr, (int)ifa->ifa_index, watch_arg);
}

static void
link_changed(const struct nlmsghdr *nh)
{
  const struct ifinfomsg *ifi = NLMSG_DATA(nh);

  // Only a link that is down or has lost its carrier matters here
  if ((ifi->ifi_flags & IFF_UP) && (ifi->ifi_flags & IFF_LOWER_UP))
    return;

  log_debug(MOD, "Interface %d is down", ifi->ifi_index);
  watch_cb(NETWATCH_LINK_DOWN, NULL, ifi->ifi_index, watch_arg);
}

static void
nl_read_cb(evutil_socket_t fd, short what, void *arg)
{
  char buf[16384];
  ssize_t n;

  while ((n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0)
    {
      for (struct nlmsghdr *nh = (struct nlmsghdr *)buf; NLMSG_OK(nh, (unsigned)n); nh = NLMSG_NEXT(nh, n))
        {
          if (nh->nlmsg_type == RTM_DELADDR || nh->nlmsg_type == RTM_NEWADDR)
            addr_changed(nh, nh->nlmsg_type == RTM_DELADDR);
          else if (nh->nlmsg_type == RTM_NEWLINK || nh->nlmsg_type == RTM_DELLINK)
            link_changed(nh);
        }
    }
  if (n < 0 && errno == ENOBUFS)
    log_warn(MOD, "Missed network events (netlink buffer overrun)");
}

int
netwatch_start(struct event_base *evbase, netwatch_cb cb, void *arg)
{
  struct sockaddr_nl sa = {
    .nl_family = AF_NETLINK,
    .nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR,
  };

  watch_cb = cb;
  watch_arg = arg;

  nl_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
  if (nl_fd < 0 || bind(nl_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
      log_warn(MOD, "Cannot watch network changes: %s", strerror(errno));
      if (nl_fd >= 0)
        close(nl_fd);
      nl_fd = -1;
      return -1;
    }

  nl_ev = event_new(evbase, nl_fd, EV_READ | EV_PERSIST, nl_read_cb, NULL);
  event_add(nl_ev, NULL);
  return 0;
}

void
netwatch_stop(void)
{
  if (nl_ev)
    event_free(nl_ev);
  nl_ev = NULL;
  if (nl_fd >= 0)
    close(nl_fd);
  nl_fd = -1;
}

int
netwatch_ifindex_of(const char *addr)
{
  struct ifaddrs *list, *ifa;
  char host[INET6_ADDRSTRLEN + IF_NAMESIZE + 1];
  int ifindex = 0;

  if (!addr || getifaddrs(&list) < 0)
    return 0;
  for (ifa = list; ifa && !ifindex; ifa = ifa->ifa_next)
    {
      socklen_t len;

      if (!ifa->ifa_addr || (ifa->ifa_addr->sa_family != AF_INET && ifa->ifa_addr->sa_family != AF_INET6))
        continue;
      len = ifa->ifa_addr->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
      if (getnameinfo(ifa->ifa_addr, len, host, sizeof(host), NULL, 0, NI_NUMERICHOST) != 0)
        continue;
      host[strcspn(host, "%")] = '\0';
      if (strcmp(host, addr) == 0)
        ifindex = (int)if_nametoindex(ifa->ifa_name);
    }
  freeifaddrs(list);
  return ifindex;
}
