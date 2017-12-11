/*******************************************************************************
 * Copyright (c) 2017, Rockwell Automation, Inc.
 * All rights reserved.
 *
 ******************************************************************************/

#include "cipconnectionobject.h"

#include "opener_user_conf.h"
#include "endianconv.h"
#include "trace.h"

#define CIP_CONNECTION_OBJECT_STATE_NON_EXISTENT 0U
#define CIP_CONNECTION_OBJECT_STATE_CONFIGURING 1U
#define CIP_CONNECTION_OBJECT_STATE_WAITING_FOR_CONNECTION_ID 2U
#define CIP_CONNECTION_OBJECT_STATE_ESTABLISHED 3U
#define CIP_CONNECTION_OBJECT_STATE_TIMEOUT 4U
#define CIP_CONNECTION_OBJECT_STATE_DEFERRED_DELETE 5U
#define CIP_CONNECTION_OBJECT_STATE_CLOSING 6U

#define CIP_CONNECTION_OBJECT_INSTANCE_TYPE_EXPLICIT_MESSAGING 0
#define CIP_CONNECTION_OBJECT_INSTANCE_TYPE_IO 1
#define CIP_CONNECTION_OBJECT_INSTANCE_TYPE_CIP_BRIDGED 2

#define CIP_CONNECTION_OBJECT_TRANSPORT_CLASS_TRIGGER_PRODUCTION_TRIGGER_CYCLIC ( \
    0 << 4)
#define \
  CIP_CONNECTION_OBJECT_TRANSPORT_CLASS_TRIGGER_PRODUCTION_TRIGGER_CHANGE_OF_STATE ( \
    1 << 4)
#define \
  CIP_CONNECTION_OBJECT_TRANSPORT_CLASS_TRIGGER_PRODUCTION_TRIGGER_APPLICATION_OBJECT ( \
    2 << 4)

#define CIP_CONNECTION_OBJECT_TRANSPORT_CLASS_TRIGGER_TRANSPORT_CLASS_0 0
#define CIP_CONNECTION_OBJECT_TRANSPORT_CLASS_TRIGGER_TRANSPORT_CLASS_1 1
#define CIP_CONNECTION_OBJECT_TRANSPORT_CLASS_TRIGGER_TRANSPORT_CLASS_2 2
#define CIP_CONNECTION_OBJECT_TRANSPORT_CLASS_TRIGGER_TRANSPORT_CLASS_3 3

#define CIP_CONNECTION_OBJECT_WATCHDOG_TIMEOUT_ACTION_TRANSITION_TO_TIMED_OUT 0
#define CIP_CONNECTION_OBJECT_WATCHDOG_TIMEOUT_ACTION_AUTO_DELETE 1
#define CIP_CONNECTION_OBJECT_WATCHDOG_TIMEOUT_ACTION_AUTO_RESET 2
#define CIP_CONNECTION_OBJECT_WATCHDOG_TIMEOUT_ACTION_DEFERRED_DELETE 3

#define CIP_CONNECTION_OBJECT_CONNECTION_TYPE_NULL 0
#define CIP_CONNECTION_OBJECT_CONNECTION_TYPE_MULTICAST (1 << 13)
#define CIP_CONNECTION_OBJECT_CONNECTION_TYPE_POINT_TO_POINT (1 << 14)

#define CIP_CONNECTION_OBJECT_PRIORITY_LOW 0
#define CIP_CONNECTION_OBJECT_PRIORITY_HIGH (1 << 13)
#define CIP_CONNECTION_OBJECT_PRIORITY_SCHEDULED (1 << 14)
#define CIP_CONNECTION_OBJECT_PRIORITY_URGENT (3 << 13)

static CipConnectionObject explicit_connection_object_pool[
  OPENER_CIP_NUM_EXPLICIT_CONNS];
static CipConnectionObject input_only_connection_object_pool[
  OPENER_CIP_NUM_INPUT_ONLY_CONNS];
static CipConnectionObject exclusive_owner_connection_object_pool[
  OPENER_CIP_NUM_EXLUSIVE_OWNER_CONNS];
static CipConnectionObject listen_only_connection_object_pool[
  OPENER_CIP_NUM_LISTEN_ONLY_CONNS];

/* Private methods declaration */
uint64_t ConnectionObjectCalculateRegularInactivityWatchdogTimerValue(
  const CipConnectionObject *connection_object);
/* End private methods declaration */

void ConnectionObjectInitializeEmpty(
  CipConnectionObject *const connection_object) {
  memset( connection_object, 0, sizeof(*connection_object) );
}

CipConnectionObject *CipConnectionObjectCreate(const CipOctet *message) {

}

void ConnectionObjectInitializeFromMessage(
  const CipOctet *message,
  CipConnectionObject *const
  connection_object) {
  /* For unconnected send - can be ignored by targets, and is ignored here */
  CipByte priority_timetick = GetSintFromMessage(&message);
  CipUsint timeout_ticks = GetSintFromMessage(&message);

  /* O_to_T Conn ID */
  ConnectionObjectSetCipConsumedConnectionID(connection_object, GetDintFromMessage(&message));
  /* T_to_O Conn ID */
  ConnectionObjectSetCipProducedConnectionID(connection_object, GetDintFromMessage(&message));

  ConnectionObjectSetConnectionSerialNumber(connection_object, GetIntFromMessage(&message));
  ConnectionObjectSetOriginatorVendorId(connection_object, GetIntFromMessage(&message));
  ConnectionObjectSetOriginatorSerialNumber(connection_object, GetDintFromMessage(&message));

  /* keep it to none existent till the setup is done this eases error handling and
   * the state changes within the forward open request can not be detected from
   * the application or from outside (reason we are single threaded)
   * */
  ConnectionObjectSetState(connection_object, kConnectionObjectStateNonExistent);
  connection_object->sequence_count_producing = 0; /* set the sequence count to zero */

  ConnectionObjectSetConnectionTimeoutMultiplier(connection_object, GetSintFromMessage(
    &message));

  MoveMessageNOctets(3, &message); /* 3 bytes reserved */

  /* the requested packet interval parameter needs to be a multiple of TIMERTICK from the header file */
  OPENER_TRACE_INFO(
    "ForwardOpen: ConConnID %" PRIu32 ", ProdConnID %" PRIu32
    ", ConnSerNo %u\n",
    connection_object->cip_consumed_connection_id,
    connection_object->cip_produced_connection_id,
    connection_object->connection_serial_number);

  ConnectionObjectSetOToTRequestedPacketInterval(connection_object, GetDintFromMessage(
    &message));

  ConnectionObjectResetInactivityWatchdogTimerValue(connection_object);

  //TODO: introduce setter function
  connection_object->o_to_t_network_connection_parameters = GetIntFromMessage(&message);

  connection_object->t_to_o_requested_packet_interval = GetDintFromMessage(&message);

  ConnectionObjectSetExpectedPacketRate(connection_object,
      connection_object->t_to_o_requested_packet_interval);

  connection_object->t_to_o_network_connection_parameters = GetIntFromMessage(&message);

  connection_object->transport_class_trigger = GetSintFromMessage(&message);
}

ConnectionObjectState ConnectionObjectGetState(
  const CipConnectionObject *const connection_object) {
  switch (connection_object->state) {
    case CIP_CONNECTION_OBJECT_STATE_NON_EXISTENT:
      return kConnectionObjectStateNonExistent;
      break;
    case CIP_CONNECTION_OBJECT_STATE_CONFIGURING:
      return kConnectionObjectStateConfiguring;
      break;
    case CIP_CONNECTION_OBJECT_STATE_WAITING_FOR_CONNECTION_ID:
      return kConnectionObjectStateWaitingForConnectionID;
      break;
    case CIP_CONNECTION_OBJECT_STATE_ESTABLISHED:
      return kConnectionObjectStateEstablished;
      break;
    case CIP_CONNECTION_OBJECT_STATE_TIMEOUT:
      return kConnectionObjectStateTimedOut;
      break;
    case CIP_CONNECTION_OBJECT_STATE_DEFERRED_DELETE:
      return kConnectionObjectStateDeferredDelete;
      break;
    case CIP_CONNECTION_OBJECT_STATE_CLOSING:
      return kConnectionObjectStateClosing;
      break;
    default:
      return kConnectionObjectStateInvalid;
  }
}

void ConnectionObjectSetState(CipConnectionObject *const connection_object,
                              const ConnectionObjectState state) {
  switch (state) {
    case kConnectionObjectStateNonExistent:
      connection_object->state =
        CIP_CONNECTION_OBJECT_STATE_NON_EXISTENT;
      break;
    case kConnectionObjectStateConfiguring:
      connection_object->state =
        CIP_CONNECTION_OBJECT_STATE_CONFIGURING;
      break;
    case kConnectionObjectStateWaitingForConnectionID:
      connection_object->state =
        CIP_CONNECTION_OBJECT_STATE_WAITING_FOR_CONNECTION_ID;
      break;
    case kConnectionObjectStateEstablished:
      connection_object->state =
        CIP_CONNECTION_OBJECT_STATE_ESTABLISHED;
      break;
    case kConnectionObjectStateTimedOut:
      connection_object->state =
        CIP_CONNECTION_OBJECT_STATE_TIMEOUT;
      break;
    case kConnectionObjectStateDeferredDelete:
      connection_object->state =
        CIP_CONNECTION_OBJECT_STATE_DEFERRED_DELETE;
      break;
    case kConnectionObjectStateClosing:
      connection_object->state =
        CIP_CONNECTION_OBJECT_STATE_CLOSING;
      break;
    default:
      OPENER_ASSERT(false); /* Never get here */
  }
}

ConnectionObjectInstanceType ConnectionObjectGetInstanceType(
  const CipConnectionObject *const connection_object) {
  switch (connection_object->instance_type) {
    case CIP_CONNECTION_OBJECT_INSTANCE_TYPE_EXPLICIT_MESSAGING:
      return kConnectionObjectInstanceTypeExplicitMessaging;
      break;
    case CIP_CONNECTION_OBJECT_INSTANCE_TYPE_IO:
      return kConnectionObjectInstanceTypeIO;
      break;
    case CIP_CONNECTION_OBJECT_INSTANCE_TYPE_CIP_BRIDGED:
      return kConnectionObjectInstanceTypeCipBridged;
      break;
    default:
      return kConnectionObjectInstanceTypeInvalid;
  }
}

ConnectionObjectTransportClassTriggerDirection
ConnectionObjectGetTransportClassTriggerDirection(
  const CipConnectionObject *const connection_object) {
  const CipByte TransportClassTriggerDirectionMask = 0x80;
  return (connection_object->transport_class_trigger &
          TransportClassTriggerDirectionMask) ==
         TransportClassTriggerDirectionMask ?
         kConnectionObjectTransportClassTriggerDirectionServer
         : kConnectionObjectTransportClassTriggerDirectionClient;
}

ConnectionObjectTransportClassTriggerProductionTrigger
ConnectionObjectGetTransportClassTriggerProductionTrigger(
  const CipConnectionObject *const connection_object) {
  const CipByte kTransportClassTriggerProductionTriggerMask = 0x70;
  switch ( (connection_object->transport_class_trigger) &
           kTransportClassTriggerProductionTriggerMask ) {
    case
      CIP_CONNECTION_OBJECT_TRANSPORT_CLASS_TRIGGER_PRODUCTION_TRIGGER_CYCLIC:
      return kConnectionObjectTransportClassTriggerProductionTriggerCyclic;
      break;
    case
      CIP_CONNECTION_OBJECT_TRANSPORT_CLASS_TRIGGER_PRODUCTION_TRIGGER_CHANGE_OF_STATE
      :
      return
        kConnectionObjectTransportClassTriggerProductionTriggerChangeOfState;
      break;
    case
      CIP_CONNECTION_OBJECT_TRANSPORT_CLASS_TRIGGER_PRODUCTION_TRIGGER_APPLICATION_OBJECT
      :
      return
        kConnectionObjectTransportClassTriggerProductionTriggerApplicationObject;
      break;
    default:
      return kConnectionObjectTransportClassTriggerProductionTriggerInvalid;
  }
}

ConnectionObjectTransportClassTriggerTransportClass
ConnectionObjectGetTransportClassTriggerTransportClass(
  const CipConnectionObject *const connection_object) {
  const CipByte kTransportClassTriggerTransportClassMask = 0x0F;
  switch ( (connection_object->transport_class_trigger) &
           kTransportClassTriggerTransportClassMask ) {
    case CIP_CONNECTION_OBJECT_TRANSPORT_CLASS_TRIGGER_TRANSPORT_CLASS_0:
      return kConnectionObjectTransportClassTriggerTransportClass0;
      break;
    case CIP_CONNECTION_OBJECT_TRANSPORT_CLASS_TRIGGER_TRANSPORT_CLASS_1:
      return kConnectionObjectTransportClassTriggerTransportClass1;
      break;
    case CIP_CONNECTION_OBJECT_TRANSPORT_CLASS_TRIGGER_TRANSPORT_CLASS_2:
      return kConnectionObjectTransportClassTriggerTransportClass2;
      break;
    case CIP_CONNECTION_OBJECT_TRANSPORT_CLASS_TRIGGER_TRANSPORT_CLASS_3:
      return kConnectionObjectTransportClassTriggerTransportClass3;
      break;
    default:
      return kConnectionObjectTransportClassTriggerTransportClassInvalid;
  }
}

CipUint ConnectionObjectGetProducedConnectionSize(
  const CipConnectionObject *const connection_object) {
  return connection_object->produced_connection_size;
}

void ConnectionObjectSetProducedConnectionSize(
  CipConnectionObject *const connection_object,
  const CipUint
  produced_connection_size) {
  connection_object->produced_connection_size = produced_connection_size;
}

CipUint ConnectionObjectGetConsumedConnectionSize(
  const CipConnectionObject *const connection_object) {
  return connection_object->consumed_connection_size;
}

void ConnectionObjectSetConsumedConnectionSize(
  CipConnectionObject *const connection_object,
  const CipUint
  consumed_connection_size) {
  connection_object->consumed_connection_size = consumed_connection_size;
}

CipUint ConnectionObjectGetExpectedPacketRate(
  const CipConnectionObject *const connection_object) {
  return connection_object->expected_packet_rate;
}

void ConnectionObjectSetExpectedPacketRate(
  CipConnectionObject *const connection_object,
  CipUint expected_packet_rate) {
  if( (expected_packet_rate % kOpenerTimerTickInMilliSeconds) == 0 ) {
    connection_object->expected_packet_rate = expected_packet_rate;
  }
  else{
    connection_object->expected_packet_rate = expected_packet_rate +
                                              (kOpenerTimerTickInMilliSeconds -
                                               expected_packet_rate %
                                               kOpenerTimerTickInMilliSeconds);
  }
}

CipUdint ConnectionObjectGetCipProducedConnectionID(
  const CipConnectionObject *const connection_object) {
  return connection_object->cip_produced_connection_id;
}

void ConnectionObjectSetCipProducedConnectionID(
  CipConnectionObject *const connection_object,
  const CipUdint
  cip_produced_connection_id) {
  connection_object->cip_produced_connection_id = cip_produced_connection_id;
}

CipUdint ConnectionObjectGetCipConsumedConnectionID(
  const CipConnectionObject *const connection_object) {
  return connection_object->cip_consumed_connection_id;
}

void ConnectionObjectSetCipConsumedConnectionID(
  CipConnectionObject *const connection_object,
  const CipUdint
  cip_consumed_connection_id) {
  connection_object->cip_consumed_connection_id = cip_consumed_connection_id;
}

ConnectionObjectWatchdogTimeoutAction ConnectionObjectGetWatchdogTimeoutAction(
  const CipConnectionObject *const connection_object) {
  switch (connection_object->watchdog_timeout_action) {
    case CIP_CONNECTION_OBJECT_WATCHDOG_TIMEOUT_ACTION_TRANSITION_TO_TIMED_OUT:
      return kConnectionObjectWatchdogTimeoutActionTransitionToTimedOut;
      break;
    case CIP_CONNECTION_OBJECT_WATCHDOG_TIMEOUT_ACTION_AUTO_DELETE:
      return kConnectionObjectWatchdogTimeoutActionAutoDelete;
      break;
    case CIP_CONNECTION_OBJECT_WATCHDOG_TIMEOUT_ACTION_AUTO_RESET:
      return kConnectionObjectWatchdogTimeoutActionAutoReset;
      break;
    case CIP_CONNECTION_OBJECT_WATCHDOG_TIMEOUT_ACTION_DEFERRED_DELETE:
      return kConnectionObjectWatchdogTimeoutActionDeferredDelete;
      break;
    default:
      return kConnectionObjectWatchdogTimeoutActionInvalid;
      break;
  }
}

void ConnectionObjectSetWatchdogTimeoutAction(
  CipConnectionObject *const connection_object,
  const CipUsint
  watchdog_timeout_action) {
  switch (watchdog_timeout_action) {
    case CIP_CONNECTION_OBJECT_WATCHDOG_TIMEOUT_ACTION_TRANSITION_TO_TIMED_OUT:
      connection_object->watchdog_timeout_action =
        kConnectionObjectWatchdogTimeoutActionTransitionToTimedOut;
      break;
    case CIP_CONNECTION_OBJECT_WATCHDOG_TIMEOUT_ACTION_AUTO_DELETE:
      connection_object->watchdog_timeout_action =
        kConnectionObjectWatchdogTimeoutActionAutoDelete;
      break;
    case CIP_CONNECTION_OBJECT_WATCHDOG_TIMEOUT_ACTION_AUTO_RESET:
      connection_object->watchdog_timeout_action =
        kConnectionObjectWatchdogTimeoutActionAutoReset;
      break;
    case CIP_CONNECTION_OBJECT_WATCHDOG_TIMEOUT_ACTION_DEFERRED_DELETE:
      connection_object->watchdog_timeout_action =
        kConnectionObjectWatchdogTimeoutActionDeferredDelete;
      break;
    default:
      connection_object->watchdog_timeout_action =
        kConnectionObjectWatchdogTimeoutActionInvalid;
      break;
  }
}

CipUint ConnectionObjectGetProducedConnectionPathLength(
  const CipConnectionObject *const connection_object) {
  return connection_object->produced_connection_path_length;
}

void ConnectionObjectSetProducedConnectionPathLength(
  CipConnectionObject *const connection_object,
  const CipUint
  produced_connection_path_length) {
  connection_object->produced_connection_path_length =
    produced_connection_path_length;
}

CipUint ConnectionObjectGetConsumedConnectionPathLength(
  const CipConnectionObject *const connection_object) {
  return connection_object->consumed_connection_path_length;
}

void ConnectionObjectSetConsumedConnectionPathLength(
  CipConnectionObject *const connection_object,
  const CipUint
  consumed_connection_path_length) {
  connection_object->consumed_connection_path_length =
    consumed_connection_path_length;
}

void ConnectionObjectSetInitialInactivityWatchdogTimerValue(
  CipConnectionObject *connection_object) {
  connection_object->inactivity_watchdog_timer =
    (ConnectionObjectCalculateRegularInactivityWatchdogTimerValue(
       connection_object) >
     10000) ? ConnectionObjectCalculateRegularInactivityWatchdogTimerValue(
      connection_object)
    :
    10000;
}

void ConnectionObjectResetInactivityWatchdogTimerValue(
  CipConnectionObject *connection_object) {
  connection_object->inactivity_watchdog_timer =
    ConnectionObjectCalculateRegularInactivityWatchdogTimerValue(
      connection_object);
}

uint64_t ConnectionObjectCalculateRegularInactivityWatchdogTimerValue(
  const CipConnectionObject *connection_object) {
  return ( ( (connection_object->o_to_t_requested_packet_interval) /
             1000 ) << (2 + connection_object->connection_timeout_multiplier) );
}

CipUint ConnectionObjectGetConnectionSerialNumber(
    const CipConnectionObject *const connection_object) {
  return connection_object->connection_serial_number;
}

void ConnectionObjectSetConnectionSerialNumber(
    CipConnectionObject *connection_object, const CipUint connection_serial_number) {
  connection_object->connection_serial_number = connection_serial_number;
}

CipUint ConnectionObjectGetOriginatorVendorId(
    const CipConnectionObject *const connection_object) {
  return connection_object->originator_vendor_id;
}

void ConnectionObjectSetOriginatorVendorId(
    CipConnectionObject *connection_object, const CipUint vendor_id){
  connection_object->originator_vendor_id = vendor_id;
}

CipUdint ConnectionObjectGetOriginatorSerialNumber(
    const CipConnectionObject *const connection_object) {
  return connection_object->originator_serial_number;
}

void ConnectionObjectSetOriginatorSerialNumber(
    CipConnectionObject *connection_object, CipUdint originator_serial_number) {
  connection_object->originator_serial_number = originator_serial_number;
}

CipUsint ConnectionObjectGetConnectionTimeoutMultiplier(
    const CipConnectionObject *const connection_object) {
  return connection_object->connection_timeout_multiplier;
}

void ConnectionObjectSetConnectionTimeoutMultiplier(
    CipConnectionObject *connection_object, CipUsint connection_timeout_multiplier) {
  connection_object->connection_timeout_multiplier = connection_timeout_multiplier;
}

CipUdint ConnectionObjectGetOToTRequestedPacketInterval(const CipConnectionObject *const connection_object) {
  return connection_object->o_to_t_requested_packet_interval;
}

void ConnectionObjectSetOToTRequestedPacketInterval(CipConnectionObject *connection_object, const CipUdint requested_packet_interval) {
  connection_object->o_to_t_requested_packet_interval = requested_packet_interval;
}

CipUdint ConnectionObjectGetTToORequestedPacketInterval(const CipConnectionObject *const connection_object) {
  return connection_object->t_to_o_requested_packet_interval;
}

void ConnectionObjectSetTToORequestedPacketInterval(CipConnectionObject *connection_object, const CipUdint requested_packet_interval) {
  connection_object->t_to_o_requested_packet_interval = requested_packet_interval;
}

bool ConnectionObjectIsOToTRedundantOwner(const CipConnectionObject *const connection_object) {
  const CipWord kOwnerMask = 0x80;
  return kOwnerMask & connection_object->o_to_t_network_connection_parameters;
}

bool ConnectionObjectIsTToORedundantOwner(const CipConnectionObject *const connection_object) {
  const CipWord kOwnerMask = 0x80;
  return kOwnerMask & connection_object->t_to_o_network_connection_parameters;
}

ConnectionObjectConnectionType ConnectionObjectGetConnectionType(const CipWord connection_parameters) {
  const CipWord kConnectionTypeMask = 3 << 13;
  switch(connection_parameters & kConnectionTypeMask) {
    case CIP_CONNECTION_OBJECT_CONNECTION_TYPE_NULL: return kConnectionObjectConnectionTypeNull;
    case CIP_CONNECTION_OBJECT_CONNECTION_TYPE_MULTICAST: return kConnectionObjectConnectionTypeMulticast;
    case CIP_CONNECTION_OBJECT_CONNECTION_TYPE_POINT_TO_POINT: return kConnectionObjectConnectionTypePointToPoint;
    default: return kConnectionObjectConnectionTypeInvalid;
  }
}

ConnectionObjectConnectionType ConnectionObjectGetOToTConnectionType(const CipConnectionObject *const connection_object) {
  ConnectionObjectGetConnectionType(connection_object->o_to_t_network_connection_parameters);
}

ConnectionObjectConnectionType ConnectionObjectGetTToOConnectionType(const CipConnectionObject *const connection_object) {
  ConnectionObjectGetConnectionType(connection_object->t_to_o_network_connection_parameters);
}

ConnectionObjectPriority ConnectionObjectGetPriority(const CipWord connection_parameters) {
  const CipWord kPriorityMask = 3 << 10;
  switch(connection_parameters & kPriorityMask) {
    case CIP_CONNECTION_OBJECT_PRIORITY_LOW: return kConnectionObjectPriorityLow;
    case CIP_CONNECTION_OBJECT_PRIORITY_HIGH: return kConnectionObjectPriorityHigh;
    case CIP_CONNECTION_OBJECT_PRIORITY_SCHEDULED: return kConnectionObjectPriorityScheduled;
    case CIP_CONNECTION_OBJECT_PRIORITY_URGENT: return kConnectionObjectPriorityUrgent;
    default: OPENER_ASSERT(false); //Not possible to get here!
  }
}

ConnectionObjectPriority ConnectionObjectGetOToTPriority(const CipConnectionObject *const connection_object) {
  return ConnectionObjectGetPriority(connection_object->o_to_t_network_connection_parameters);
}

ConnectionObjectPriority ConnectionObjectGetTToOPriority(const CipConnectionObject *const connection_object) {
  return ConnectionObjectGetPriority(connection_object->t_to_o_network_connection_parameters);
}


ConnectionObjectConnectionSizeType ConnectionObjectGetConnectionSizeType(const CipWord connection_parameters) {
  const CipWord kConnectionSizeTypeMask = 1 << 9;
  if(connection_parameters & kConnectionSizeTypeMask) {
    return kConnectionObjectConnectionSizeTypeVariable;
  } else {
    return kConnectionObjectConnectionSizeTypeFixed;
  }
}

ConnectionObjectConnectionSizeType ConnectionObjectGetOToTConnectionSizeType(const CipConnectionObject *const connection_object) {
  return ConnectionObjectGetConnectionSizeType(connection_object->o_to_t_network_connection_parameters);
}

ConnectionObjectConnectionSizeType ConnectionObjectGetTToOConnectionSizeType(const CipConnectionObject *const connection_object) {
  return ConnectionObjectGetConnectionSizeType(connection_object->t_to_o_network_connection_parameters);
}

size_t ConnectionObjectGetConnectionSize(const CipWord connection_parameters) {
  const CipWord kConnectionSizeMask = 0x01FF;
  return connection_parameters & kConnectionSizeMask;
}

size_t ConnectionObjectGetOToTConnectionSize(const CipConnectionObject *const connection_object) {
  return ConnectionObjectGetConnectionSize(connection_object->o_to_t_network_connection_parameters);
}

size_t ConnectionObjectGetTToOConnectionSize(const CipConnectionObject *const connection_object) {
  return ConnectionObjectGetConnectionSize(connection_object->t_to_o_network_connection_parameters);
}
