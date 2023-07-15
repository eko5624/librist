#include "network.h"
#include "socket-shim.h"
#include <stddef.h>
#include <stdint.h>
#include <memory.h>
#ifndef _WIN32
#include <ifaddrs.h>
#ifdef __linux__
#include <netpacket/packet.h>
#else
#include <net/if_dl.h>
#endif
#else
#include <iphlpapi.h>
#endif

int _librist_network_get_macaddr(uint8_t mac[]) {
  char mac_null[6] = {0};
#ifndef _WIN32
  struct ifaddrs *ifaddr = NULL;
  struct ifaddrs *ifa = NULL;
  if (getifaddrs(&ifaddr) == -1) {
    return -1;
  } else {
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
#ifdef __linux__

      if ((ifa->ifa_addr) && (ifa->ifa_addr->sa_family == AF_PACKET)) {
        struct sockaddr_ll *s = (struct sockaddr_ll *)ifa->ifa_addr;
        if (memcmp(mac_null, s->sll_addr, 6)) {
          memcpy(mac, s->sll_addr, 6);
          break;
        }
      }
#else
      if ((ifa->ifa_addr) && (ifa->ifa_addr->sa_family == AF_LINK)) {
        struct sockaddr_dl *s = (struct sockaddr_dl *)ifa->ifa_addr;
        if (memcmp(mac_null, s->sdl_data + s->sdl_nlen, 6)) {
          memcpy(mac, s->sdl_data + s->sdl_nlen, 6);
          break;
        }
      }
#endif
    }
    freeifaddrs(ifaddr);
  }
#else
  IP_ADAPTER_INFO adaptors[16];
  DWORD adaptors_size = sizeof(adaptors);
  DWORD ret = GetAdaptersInfo(adaptors, &adaptors_size);
  if (ret != ERROR_SUCCESS)
    return -1;
  for (PIP_ADAPTER_INFO adaptor = adaptors; adaptor != NULL;
       adaptor = adaptor->Next) {
    if (memcmp(mac_null, adaptor->Address, 6)) {
      memcpy(mac, adaptor->Address, 6);
      break;
    }
  }
#endif
  return 0;
}
