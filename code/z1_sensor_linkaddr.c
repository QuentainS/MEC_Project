#include "contiki.h"
#include "net/rime/rime.h"
#include "random.h"
#include "lib/list.h"
#include "lib/memb.h"
#include "lib/random.h"

#include <stdio.h>

#define MAX_RETRANSMISSIONS 4

int power(int a, int b)
{
  int res = 1;
  size_t i;
  for(i = 0 ; i < b ; i++)
  {
    res = res * a;
  }
  return res;
}

/* This structure holds information about children. */
struct children {
  /* The ->next pointer is needed since we are placing these on a
     Contiki list. */
  struct children *next;

  /* The ->addr field holds the Rime address of the child to contact. */
  int original_sender;

  /* The ->addr field holds the Rime address of the node to fwd the message to. */
  linkaddr_t addr_fwd;

};

#define MAX_CHILDREN 10

/* This MEMB() definition defines a memory pool from which we allocate
   neighbor entries. */
MEMB(children_memb, struct children, MAX_CHILDREN);

/* The neighbors_list is a Contiki list that holds the neighbors we
   have seen thus far. */
LIST(children_list);

/*---------------------------------------------------------------------------*/
PROCESS(network_setup, "Network Setup");
PROCESS(send_sensor_data, "Send Sensor Data");
PROCESS(forwarding_messages, "Forwarding SRV & COM");
AUTOSTART_PROCESSES(&network_setup, &send_sensor_data, &forwarding_messages);

//MEMB(linkaddr_memb, linkaddr_t, 1);
static linkaddr_t *parent_node;
static int not_connected = 1;
static int parent_signal = -9999;

/*---------------------------------------------------------------------------*/
static void
recv_bdcst(struct broadcast_conn *c, const linkaddr_t *from)
{
  char message[100];
  strcpy(message, (char *)packetbuf_dataptr());


  // If announce from a new node
  if (message[0] == 'N' && message[1] == 'D' && message[2] == 'A')
  {
    // If this node is connected to the server
    printf("[SETUP THREAD] Announce received : %s\n", message);
    if (!not_connected)
    {
      // Respond to the child
      sprintf(message, "NDR%d", from->u8[0]);
      packetbuf_copyfrom(message, strlen(message));
      broadcast_send(c);
      printf("[SETUP THREAD] Reponse (NDR) sent : %s\n", message);
    }
    
  }

  // If response to an announce
  else if (message[0] == 'N' && message[1] == 'D' && message[2] == 'R')
  {
    size_t size, i;
    int recipient = 0;
    size = strlen(message) - 3;

    // Compute the recipient from the radio message 
    for(i = 0 ; i < size ; i++)
    {
      recipient = recipient + ((message[i+3]-48) * power(10,size-i-1));
    }

    // If the message is for this node (avoid broadcast loop)
    if(recipient == linkaddr_node_addr.u8[0])
    {
      printf("[SETUP THREAD] Parent response received from %d with signal %d\n", from->u8[0], packetbuf_attr(PACKETBUF_ATTR_RSSI));

      if (packetbuf_attr(PACKETBUF_ATTR_RSSI) > parent_signal)
      {
        printf("[SETUP THREAD] This parent is better than %d\n", parent_signal);
        parent_signal = packetbuf_attr(PACKETBUF_ATTR_RSSI);
        //from = memb_alloc(&linkaddr_memb);
        linkaddr_copy(parent_node, from);
        not_connected = 0;
      }
    }
    
  }
  
}

static const struct broadcast_callbacks broadcast_call = {recv_bdcst};
static struct broadcast_conn broadcast;

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(network_setup, ev, data)
{
  static struct etimer et;
  char message[100];
  PROCESS_EXITHANDLER(broadcast_close(&broadcast);)

  PROCESS_BEGIN();

  broadcast_open(&broadcast, 129, &broadcast_call);

  printf("[SENSOR] I'm %d\n", linkaddr_node_addr.u8[0]);

  while(1) {

    // If the node is not connected to the network, try to connect
    if (not_connected)
    {
      sprintf(message, "NDA");
      packetbuf_copyfrom(message, strlen(message));
      broadcast_send(&broadcast);
      printf("[SETUP THREAD] Announce (NDA) sent : %s\n", message);
    }

    /* Delay 2-4 seconds */
    etimer_set(&et, CLOCK_SECOND * 4 + random_rand() % (CLOCK_SECOND * 4));
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

    
  }

  PROCESS_END();
}

/*---------------------------------------------------------------------------*/

/* This function is called for every incoming unicast packet. */
static void
recv_ruc(struct runicast_conn *c, const linkaddr_t *from, uint8_t seqno)
{
  char message[100];
  strcpy(message, (char *)packetbuf_dataptr());
  int original_sender = 0;
  size_t size, i;

  // If SRV message, need to forward it to the parent_node
  if (message[0] == 'S' && message[1] == 'R' && message[2] == 'V')
  {
    // Get the air quality
    //int air_quality = (message[3]-48) * 10 + (message[4]-48);

    // Get the size of the address (e.g. "1" or "78" or "676")
    size = strlen(message) - 5;

    // Get the address of the original sender
    for(i = 0 ; i < size ; i++) //TODO for (i=5;i<size;i++)
    {
      original_sender = original_sender + ((message[i+5]-48) * power(10,size-i-1));
    }

    struct children *child;

    /* Check if we already know this children. */
    for(child = list_head(children_list); child != NULL; child = list_item_next(child)) 
    {

      /* We break out of the loop if the address of the neighbor matches
         the address of the original sender from which we received this
         unicast message. */
      if(&child->original_sender == original_sender) 
      {
        if(linkaddr_cmp(&child->addr_fwd, from)) 
        {
          printf("[FORWARDING THREAD] POSSIBLE ERROR fwd_information from %d to %d was not removed\n", original_sender, from->u8[0]);
          break;
        }
      }

    }

    /* If n is NULL, this neighbor was not found in our list, and we
       allocate a new struct neighbor from the neighbors_memb memory
       pool. */
    if(child == NULL) 
    {
      child = memb_alloc(&children_memb);

      /* If we could not allocate a new neighbor entry, we give up. We
         could have reused an old neighbor entry, but we do not do this
         for now. */
      if(child == NULL) 
      {
        return;
      }

      /* Initialize the fields. */
      linkaddr_copy(&child->addr_fwd, from);
      child->original_sender = original_sender;

      /* Place the neighbor on the neighbor list. */
      list_add(children_list, child);
    }

    //printf("[DATA THREAD] Unicast received from %d : Sensor %d - Quality = %d\n", 
    //  from->u8[0], original_sender, air_quality);

    // Forward the message to the parent
    packetbuf_copyfrom(message, strlen(message));
    runicast_send(c, parent_node, MAX_RETRANSMISSIONS);

    printf("[FORWARDING THREAD] Forwarding from %d to %d (%s)\n", from->u8[0], parent_node->u8[0], message);

  }
  else
  {
    // DEBUG PURPOSE
    printf("[FORWARDING THREAD] Weird message received from %d.%d\n", from->u8[0], from->u8[1]);
  }

}

static void
sent_runicast(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions)
{
  //printf("runicast message sent to %d.%d, retransmissions %d\n",
  // to->u8[0], to->u8[1], retransmissions);
}
static void
timedout_runicast(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions)
{
  // If connecion timeout, re-run the network setup to find a new parent
  printf("[FORWARDING THREAD] Impossible to send data, disconnected from network.");
  not_connected = 1;
  parent_signal = -9999;
}

/*---------------------------------------------------------------------------*/

static const struct runicast_callbacks runicast_callbacks = {recv_ruc, sent_runicast, timedout_runicast};
static struct runicast_conn runicast;

/*---------------------------------------------------------------------------*/

// Sending data thread
PROCESS_THREAD(send_sensor_data, ev, data)
{
  char message[100];
  int air_quality;
  static struct etimer before_start;

  PROCESS_EXITHANDLER(runicast_close(&runicast);)
    
  PROCESS_BEGIN();

  runicast_open(&runicast, 144, &runicast_callbacks);

  /* Wait random seconds to simulate the different time
  of the node installation (0-59min) */
  printf("[DATA THREAD] Waiting before start ...\n");
  etimer_set(&before_start, random_rand()%60 * CLOCK_SECOND);
  PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&before_start));
  printf("[DATA THREAD] Starting ...\n");

  while(1) {
    static struct etimer et;
    
    // Send the data to the parent
    if(!not_connected && !runicast_is_transmitting(&runicast)) {

      // Generate random sensor data
      air_quality = random_rand() % 99 + 1;
      
      sprintf(message, "SRV%02d%d", air_quality, linkaddr_node_addr.u8[0]);
      packetbuf_copyfrom(message, strlen(message));

      runicast_send(&runicast, parent_node, MAX_RETRANSMISSIONS);

      printf("[DATA THREAD] Sending data (%d) to the server\n", air_quality);
    }

    /* Delay 1 minute */
    etimer_set(&et, 60*CLOCK_SECOND);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

  }

  PROCESS_END();
}

/*---------------------------------------------------------------------------*/

// Only forwarding thread
PROCESS_THREAD(forwarding_messages, ev, data)
{
  PROCESS_EXITHANDLER(runicast_close(&runicast);)
    
  PROCESS_BEGIN();

  runicast_open(&runicast, 144, &runicast_callbacks);

  while(1) {

    struct children *child;
    
    /* Check if we already know this children. */
    for(child = list_head(children_list); child != NULL; child = list_item_next(child)) 
    {
      linkaddr_t forward_address = child->addr_fwd;
      //linkaddr_copy(forward_address, child->addr_fwd);
      printf("[MESSAGE THREAD] fwd_information from %d to DOWN was successfuly stored\n", child->original_sender);
      printf("[MESSAGE THREAD] fwd_information from UP to %d was successfuly stored\n", forward_address->u8[0]);
      list_remove(children_list, child);
    }

    // Wait for SRV / CMD to forward
    PROCESS_WAIT_EVENT();
  
  }

  PROCESS_END();
}
