#include "contiki.h"
#include "net/rime/rime.h"
#include "random.h"
#include "lib/list.h"
#include "lib/memb.h"
#include "lib/random.h"

#include <stdio.h>
#include <stdlib.h>

#define MAX_RETRANSMISSIONS 4
#define MAX_ROUTES 10
#define INACTIVE_MESSAGE 20
#define NUMBER_OF_SAVED_VALUES 5 //TODO change to 30
#define MAX_CHILDREN 2 //TODO change to 5

// Utils function for computing the Rime ID
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


/* This structure holds information about the routes. */
struct routes {

  // The ->next pointer is needed for Contiki list
  struct routes *next;

  // The id that we want to reach
  int id;

  // Where to forward the message
  linkaddr_t addr_fwd;

  // int used to determine whether or not the node is still active
  int age;

  // If = 0 then corresponding child exisist, If = 1 then no corresponding child
  // If = 2 then collecting data before making child
  int is_child;

};

// Holding information about the children (the ones with data)
struct children {
  // The ->next pointer is needed for Contiki list
  struct children *next;

  // The child's id
  int id;

  // Values from sensors
  int last_values[NUMBER_OF_SAVED_VALUES];

  //number of values stored
  int nvalues;

};

// memory allocation for routes
MEMB(routes_memb, struct routes, MAX_ROUTES);
LIST(routes_list);

// memory allocation for children
MEMB(children_memb, struct children, MAX_CHILDREN);
LIST(children_list);

/*---------------------------------------------------------------------------*/
PROCESS(network_setup, "Network Setup");
PROCESS(forwarding_messages, "Forwarding SRV & COM");
AUTOSTART_PROCESSES(&network_setup, &forwarding_messages);

static linkaddr_t *parent_node;
static int not_connected = 1;
static int parent_signal = -9999;
static int number_of_children = 0;

// Used to get a children using RIME id. 
// When a new child is created, it might not be in the list already.
// This function creates any new child that does not yet exist.
struct children* get_children(int id)
{
  struct children *child;
  for(child = list_head(children_list); child != NULL; child = list_item_next(child))
  {
    if ( child->id == id )
    {
      break;
    }
  }
  // If the child does not exist, it needs to be created
  if ( child == NULL ) // From 2 to 0
  {
    printf("requested non-existing child %d\n", id);
  }
  return child;
}

// Used to remove a child
// It is called when a child stops communication by remove_old_routes()
// This function selects a new child to replace the removed one from the list of routes
void remove_child(int id)
{
  struct children *child;
  for(child = list_head(children_list); child != NULL; child = list_item_next(child))
  {
    if ( child->id == id )
    {
      printf("removed child \n");
      list_remove(children_list, child);
    }
  }
  // Selecting new child
  struct routes *route;

  for(route = list_head(routes_list); route != NULL; route = list_item_next(route)) 
  {
    if(route->is_child == 1) // Can't just be !route->is_child
    {
// As long as is_child = 2 (computation node does not have 30 data points), messages will be forwarded to the server
      route->is_child = 2; 
      struct children *new_child;
      new_child = memb_alloc(&children_memb);
      new_child->id = id;
      new_child->nvalues = 0;
      list_add(children_list, new_child);
      break; // only select one
    }
  }
}

// This function is used to remove a node that has stopped communicating for a while.
// It uses the INACTIVE_MESSAGE constant
void remove_old_routes()
{
  struct routes *route;

  for(route = list_head(routes_list); route != NULL; route = list_item_next(route)) 
  {
    if(INACTIVE_MESSAGE <= (int) route->age ) 
    {
      printf("removed route \n");
      if (route->is_child != 1) // Can't just be route->is_child
      {
        remove_child(route->id);
      }
      list_remove(routes_list, route);
    }
    else 
    {
      route->age += 1;
    }
  }
}

/*---------------------------------------------------------------------------*/
static void
recv_bdcst(struct broadcast_conn *c, const linkaddr_t *from)
{
  char message[10];
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
      //printf("[SETUP THREAD] Parent response received from %d with signal %d\n", from->u8[0], packetbuf_attr(PACKETBUF_ATTR_RSSI));

      if (packetbuf_attr(PACKETBUF_ATTR_RSSI) > parent_signal)
      {
        printf("[SETUP THREAD] This parent is better than %d\n", parent_signal);
        parent_signal = packetbuf_attr(PACKETBUF_ATTR_RSSI);
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
  char message[10];

  PROCESS_EXITHANDLER(broadcast_close(&broadcast);)

  PROCESS_BEGIN();

  broadcast_open(&broadcast, 129, &broadcast_call);

  printf("[COMPUTATION] I'm %d\n", linkaddr_node_addr.u8[0]);

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
  char message[10];
  strcpy(message, (char *)packetbuf_dataptr());
  int original_sender = 0;
  size_t size, i;
  
  printf("%c%c%c\n", message[0], message[1], message[2]);
  
  // If SRV message, need to forward it to the parent_node
  if (message[0] == 'S' && message[1] == 'R' && message[2] == 'V')
  {
    // Get the air quality
    //int air_quality = (message[3]-48) * 10 + (message[4]-48);

    // Get the size of the address (e.g. "1" or "78" or "676")
    size = strlen(message) - 5;

    // Get the address of the original sender
    for(i = 0 ; i < size ; i++)
    {
      original_sender = original_sender + ((message[i+5]-48) * power(10,size-i-1));
    }

    struct routes *new_route;

    /* Check if we already know this routes. */
    for(new_route = list_head(routes_list); new_route != NULL; new_route = list_item_next(new_route)) 
    {

      // We break out of the loop if the address in the list matches with from
      if((int)new_route->id == (int)original_sender) 
      {
          break;
      }

    }

    // If new_route is NULL, this node was not found in our list
    if(new_route == NULL) 
    {
      new_route = memb_alloc(&routes_memb);

      // If allocation failed, we give up.
      if(new_route == NULL) 
      {
        return;
      }

      // IF there is space left for a child
      if (number_of_children < MAX_CHILDREN)
      {
        //Creating the child
        new_route->is_child = 0;
        struct children *new_child;
        new_child = memb_alloc(&children_memb);
        if (new_child == NULL) // If there is an error / no memory left
        {
          new_route->is_child = 1;
          number_of_children = MAX_CHILDREN;
        }
        // Giving values to child attributes
        new_child->id = original_sender;
        new_child->nvalues = 0;
        list_add(children_list, new_child);
        number_of_children += 1;
      }
      else 
      { // If there are too many children
        new_route->is_child = 1;
      }
      printf("[ROUTING] New node\n");
    }
    new_route->age = 0; // used for deleting routes after they stop communicating
    //if (!linkaddr_cmp(&new_route->addr_fwd, from)) // If routing has changed
    //{
      // Initialize the new_route.
      linkaddr_copy(&new_route->addr_fwd, from);
      new_route->id = original_sender;

      // Add the route into the list
      printf("[ROUTING] New route\n");
      list_add(routes_list, new_route);
    //}
    
    if ( new_route->is_child != 1 )
    {
      //STORE THE SENSOR INFO
      int data = rand() % 10;   //TODO THIS SHOULD BE FROM MESSAGE
      struct children *this_child;
      this_child = get_children(original_sender);

      // When the array is not full, append data
      if (this_child->nvalues < NUMBER_OF_SAVED_VALUES){
        this_child->last_values[this_child->nvalues] = data;
        this_child->nvalues += 1;
      }
      // When the array is full, shift all values left and then append
      else
      {
        new_route->is_child = 0;
        int value_index = 1;
        for ( value_index; value_index < NUMBER_OF_SAVED_VALUES; value_index ++ )
        {
          this_child->last_values[value_index-1] = this_child->last_values[value_index];
        }
        this_child->last_values[this_child->nvalues-1] = data;
      }
    }
    if ( new_route->is_child != 0 )
    {  //TODO this does not seem to make it to the border node
      // Forward the message to the parent
      packetbuf_copyfrom(message, strlen(message));
      runicast_send(c, parent_node, MAX_RETRANSMISSIONS);

      printf("[FORWARDING THREAD] [TO SERVER] Forwarding from %d to %d (%s)\n", from->u8[0], parent_node->u8[0], message);
    }
    if ( new_route->is_child == 0 ) // AND 30 Values
    {  //TODO the maths
      // COMPUTE SLOPE
      // RESPOND TO MESSAGE
    }

  }
  else if (message[0] == 'C' && message[1] == 'O' && message[2] == 'M')
  {
    // Get the size of the address (e.g. "1" or "78" or "676")
    size = strlen(message) - 5;
    int recipient = 0;    
    int order = message[3] - '0';
    struct routes *route;

    // Get the address of the message recipient 
    for(i = 0 ; i < size ; i++)
    {
      recipient = recipient + ((message[i+5]-48) * power(10,size-i-1));
    }

    // gets the rigth route
    for(route = list_head(routes_list); route != NULL; route = list_item_next(route)) 
    {
      // We break out of the loop if the address in the list matches with from
      if((int)route->id == (int)recipient) 
      {
        break;
      }
    }

    // Forward the message to the parent
    packetbuf_copyfrom(message, strlen(message));
    runicast_send(c, &route->addr_fwd, MAX_RETRANSMISSIONS);
    
    printf("[FORWARDING THREAD] [TO NODE] Order: %d received from %d for %d (%s)\n", order, from->u8[0], recipient, message);
    
  }
  else
  {
    // DEBUG PURPOSE
    printf("[FORWARDING THREAD] Weird message received from %d.%d\n", from->u8[0], from->u8[1]);
  }

  /* ===== DEBUG ==== */
  struct routes *route;

  // Print the current routes
  for(route = list_head(routes_list); route != NULL; route = list_item_next(route)) 
  {
    printf("[ROUTING] To contact %d (child:%d), I have to send to %d\n", route->id, route->is_child, route->addr_fwd.u8[0]);
    //list_remove(routes_list, route);
  }
  /* ================ */
  remove_old_routes();

}

/*---------------------------------------------------------------------------*/

static const struct runicast_callbacks runicast_callbacks = {recv_ruc};
static struct runicast_conn runicast;

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(forwarding_messages, ev, data)
{
  PROCESS_EXITHANDLER(runicast_close(&runicast);)
    
  PROCESS_BEGIN();

  runicast_open(&runicast, 144, &runicast_callbacks);

  while(1) {

    // Wait for SRV / CMD to forward
    PROCESS_WAIT_EVENT();
  
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
