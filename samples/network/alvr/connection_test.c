/* Copyright (C) 2026 The RetroArch team. SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../../network/alvr_connection.h"
#include <net/net_socket.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t accepted_20[] = {
   0x00, 0x00, 0x00, 0x00, 0x67, 0x5d, 0x6d, 0xcf, 0xaf, 0x7e, 0x66, 0x17,
   0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x51, 0x75, 0x65, 0x73,
   0x74, 0x00, 0x00, 0x00, 0x00, 0x7f, 0x00, 0x00, 0x01, 0x01, 0x00, 0x08,
   0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
   0x00, 0x00, 0x00, 0x00, 0x90, 0x42, 0x00, 0x00, 0xb4, 0x42, 0x00, 0x00,
   0xf6, 0xc2, 0x00, 0x00, 0xfa, 0xc2, 0x80, 0xbb, 0x00, 0x00,
};

static const uint8_t accepted[] = {
   0, 253, 0x70, 0x09, 0x77, 0xaf, 0xb3, 0xca, 0x13, 0xb5,
   5, 'Q', 'u', 'e', 's', 't',
   0, 127, 0, 0, 1, 1,
   251, 0, 8, 251, 0, 8,
   251, 0, 16, 251, 0, 16,
   2, 0, 0, 0x90, 0x42, 0, 0, 0xb4, 0x42,
   251, 0x80, 0xbb,
   1, 1, 1, 1, 0, 0, 0, 0x80, 0x3f, 0,
   2, '{', '}'
};
static unsigned deliveries;
static bool legacy_mode;
static uint8_t tracking[3000];

static void receive_tracking(void *context, unsigned stream, void *data,
      size_t size, bool legacy)
{
   assert(legacy == legacy_mode);
   (void)context;
   assert(stream == 0);
   assert(size == sizeof(tracking));
   assert(!memcmp(data, tracking, size));
   deliveries++;
}

static int listener(unsigned port, bool udp)
{
   struct sockaddr_in address;
   int fd = socket_create("alvr_test", SOCKET_DOMAIN_INET,
         udp ? SOCKET_TYPE_DATAGRAM : SOCKET_TYPE_STREAM,
         udp ? SOCKET_PROTOCOL_UDP : SOCKET_PROTOCOL_TCP);
   int reuse = 1;
   assert(fd >= 0);
   assert(socket_nonblock(fd));
   setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
   memset(&address, 0, sizeof(address));
   address.sin_family = AF_INET;
   address.sin_port = htons(port);
   assert(inet_pton(AF_INET, "127.0.0.2", &address.sin_addr) == 1);
   assert(bind(fd, (struct sockaddr*)&address, sizeof(address)) == 0);
   if (!udp)
      assert(listen(fd, 1) == 0);
   return fd;
}

static int accept_connection(int listener_fd, alvr_connection_t *c)
{
   unsigned i;
   for (i = 0; i < 100000; i++)
   {
      int fd;
      assert(alvr_connection_poll(c, 1000) != ALVR_CONNECTION_ERROR);
      fd = accept(listener_fd, NULL, NULL);
      if (fd >= 0)
      {
         assert(socket_nonblock(fd));
         return fd;
      }

      assert(isagain(fd));
   }
   assert(!"accept timeout");
   return -1;
}

static void send_control(int fd, alvr_connection_t *c,
      const uint8_t *payload, size_t size)
{
   uint8_t length[4];
   size_t i;
   if (legacy_mode)
      alvr_wire_store_be32(length, (uint32_t)size);
   else
      alvr_wire_store32(length, (uint32_t)size);
   /* Split every header and body byte across separate polls. */
   for (i = 0; i < 4 + size; i++)
   {
      const uint8_t *byte = i < 4 ? length + i : payload + i - 4;
      assert(send(fd, (const char*)byte, 1, 0) == 1);
      assert(alvr_connection_poll(c, 1000) != ALVR_CONNECTION_ERROR);
   }
}

static size_t read_control(int fd, alvr_connection_t *c,
      uint8_t *output, size_t capacity)
{
   uint8_t header[4];
   size_t pos = 0, needed = 4;
   unsigned i;
   bool body = false;
   for (i = 0; i < 100000; i++)
   {
      int got;
      uint8_t *target = body ? output : header;
      assert(alvr_connection_poll(c, 1000) != ALVR_CONNECTION_ERROR);
      got = recv(fd, (char*)target + pos, (int)(needed - pos), 0);
      if (got < 0)
      {
         assert(isagain(got));
         continue;
      }
      assert(got > 0);
      pos += got;
      if (pos != needed)
         continue;
      if (body)
         return needed;

      needed = legacy_mode ? alvr_wire_be32(header) : alvr_wire_u32(header);
      assert(needed && needed <= capacity);
      body = true;
      pos = 0;
   }
   assert(!"control timeout");
   return 0;
}

static void connection(void)
{
   alvr_connection_t *c;
   struct sockaddr_in server;
   uint8_t packet[ALVR_PACKET_SIZE], control[1024];
   uint8_t ready[4] = {3, 0, 0, 0};
   static uint8_t media[3000];
   size_t length;
   unsigned i, shards = 0;
   unsigned header = legacy_mode ? 18 : 14;
   int tcp, udp, peer;
   assert(network_init());
   tcp = listener(ALVR_CONTROL_PORT, false);
   udp = listener(ALVR_STREAM_PORT, true);
   c = alvr_connection_new("127.0.0.2", 512, 512, 48000,
         1000, receive_tracking, NULL);
   assert(c);
   peer = accept_connection(tcp, c);
   send_control(peer, c, legacy_mode ? accepted_20 : accepted,
         legacy_mode ? sizeof(accepted_20) : sizeof(accepted));
   length = read_control(peer, c, control, sizeof(control));
   assert(length > 20);
   length = read_control(peer, c, control, sizeof(control));
   assert(length == (legacy_mode ? 4 : 1) && control[0] == 0);
   send_control(peer, c, ready, legacy_mode ? 4 : 1);
   for (i = 0; i < 1000000; i++)
      if (alvr_connection_poll(c, 1000) == ALVR_CONNECTION_STREAMING)
         break;
   assert(i < 1000000);
   assert(!alvr_connection_send(c, 3, NULL, 1));
   assert(alvr_connection_decoder(c, "abc", 3, 0));
   length = read_control(peer, c, control, sizeof(control));
   if (length == (legacy_mode ? 4 : 1) && control[0] == 3)
      length = read_control(peer, c, control, sizeof(control));
   if (legacy_mode)
   {
      assert(length == 19 && alvr_wire_u32(control) == 1);
      assert(alvr_wire_u32(control + 4) == 0);
      assert(alvr_wire_u32(control + 8) == 3);
      assert(!memcmp(control + 16, "abc", 3));
   }
   else
   {
      assert(length == 9 && control[0] == 1 && control[1] == 0);
      assert(control[2] == 3 && !memcmp(control + 3, "abc", 3));
   }
   assert(!alvr_connection_decoder(c, "abc", 3, 2));
   assert(alvr_connection_decoder(c, "abc", 3, 1));
   length = read_control(peer, c, control, sizeof(control));
   if (length == (legacy_mode ? 4 : 1) && control[0] == 3)
      length = read_control(peer, c, control, sizeof(control));
   if (legacy_mode)
      assert(length == 19 && alvr_wire_u32(control + 4) == 1);
   else
      assert(length == 9 && control[1] == 1);
   for (i = 0; i < sizeof(media); i++)
      tracking[i] = media[i] = (uint8_t)i;
   assert(alvr_connection_send(c, 3, media, sizeof(media)));
   assert(!alvr_connection_send(c, 3, media, sizeof(media)));
   for (i = 0; i < 100000 && shards < 3; i++)
   {
      int got;
      assert(alvr_connection_poll(c, 1000) == ALVR_CONNECTION_STREAMING);
      got = recv(udp, (char*)packet, sizeof(packet), 0);
      if (got < 0)
      {
         assert(isagain(got));
         continue;
      }
      assert(got > (int)header);
      if (legacy_mode)
      {
         assert(alvr_wire_be32(packet) == (unsigned)got - 4);
         assert(packet[4] == 0 && packet[5] == 3);
         assert(alvr_wire_be32(packet + 6) == 0);
         assert(alvr_wire_be32(packet + 10) == 3);
         assert(alvr_wire_be32(packet + 14) == shards);
      }
      else
      {
         assert(packet[0] == 3 && packet[1] == 0);
         assert(alvr_wire_u32(packet + 2) == 0);
         assert(alvr_wire_u32(packet + 6) == 3);
         assert(alvr_wire_u32(packet + 10) == shards);
      }
      assert(!memcmp(packet + header, media + shards * (ALVR_PACKET_SIZE - header),
               (size_t)got - header));
      shards++;
   }
   assert(shards == 3);
   memset(&server, 0, sizeof(server));
   server.sin_family = AF_INET;
   server.sin_port = htons(ALVR_STREAM_PORT);
   assert(inet_pton(AF_INET, "127.0.0.1", &server.sin_addr) == 1);
   /* Reverse order, then duplicates: deliver one complete tracking packet. */
   for (i = 0; i < 6; i++)
   {
      length = alvr_protocol_shard(packet, sizeof(packet), 0, UINT32_MAX,
            tracking, sizeof(tracking), 2 - i % 3, legacy_mode);
      assert(sendto(udp, (const char*)packet, (int)length, 0,
               (struct sockaddr*)&server, sizeof(server)) == (int)length);
   }
   for (i = 0; i < 100000 && !deliveries; i++)
      assert(alvr_connection_poll(c, 1000) == ALVR_CONNECTION_STREAMING);
   assert(deliveries == 1);
   assert(alvr_connection_poll(c, 2001000) == ALVR_CONNECTION_ERROR);
   alvr_connection_free(c);
   socket_close(peer);
   socket_close(tcp);
   socket_close(udp);
}

int main(void)
{
   connection();
   legacy_mode = true;
   deliveries = 0;
   connection();
   puts("ALVR connection tests passed (20 and 21)");
   return 0;
}
