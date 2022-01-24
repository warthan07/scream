#include <stdio.h>
#include <errno.h>

#include "network.h"

static rctx_network_t rctx_network;

int init_network(enum receiver_type receiver_mode, in_addr_t interface, int port, char* multicast_group, long timeout_ms)
{
  rctx_network.sockfd = socket(AF_INET,SOCK_DGRAM,0);

  memset((void *)&(rctx_network.servaddr), 0, sizeof(rctx_network.servaddr));
  rctx_network.servaddr.sin_family = AF_INET;
  rctx_network.servaddr.sin_addr.s_addr = (receiver_mode == Unicast) ? interface : htonl(INADDR_ANY);
  rctx_network.servaddr.sin_port = htons(port);
  bind(rctx_network.sockfd, (struct sockaddr *)&rctx_network.servaddr, sizeof(rctx_network.servaddr));

  if (receiver_mode == Multicast) {
    rctx_network.imreq.imr_multiaddr.s_addr = inet_addr(multicast_group ? multicast_group : DEFAULT_MULTICAST_GROUP);
    rctx_network.imreq.imr_interface.s_addr = interface;

    setsockopt(rctx_network.sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
              (const void *)&rctx_network.imreq, sizeof(struct ip_mreq));
  }

  if (timeout_ms>0) {
    struct timeval tv;
    tv.tv_sec = timeout_ms/1000;
    tv.tv_usec = 1000*(timeout_ms%1000);
    if (setsockopt(rctx_network.sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
      perror("Error");
    }
  }

  return 0;
}

void rcv_network(receiver_data_t* receiver_data)
{
  ssize_t n = 0;
  receiver_data->prev_timed_out = receiver_data->timed_out;
  receiver_data->timed_out = 0;
  while (n<HEADER_SIZE) {
    errno = 0;
    n = recvfrom(rctx_network.sockfd, rctx_network.buf, MAX_SO_PACKETSIZE, 0, NULL, 0);
    if (errno==EAGAIN || errno==EWOULDBLOCK) {
      // There was a timeout, so we stop listening for now
      receiver_data->timed_out = 1;
      break;
    }
  }
  if (!receiver_data->timed_out) {
    receiver_data->format.sample_rate = rctx_network.buf[0];
    receiver_data->format.sample_size = rctx_network.buf[1];
    receiver_data->format.channels = rctx_network.buf[2];
    receiver_data->format.channel_map = (rctx_network.buf[4] << 8) | rctx_network.buf[3];
    receiver_data->audio_size = n - HEADER_SIZE;
    receiver_data->audio = &rctx_network.buf[5];
  }
}