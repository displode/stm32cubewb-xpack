/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_ble.c
  * @author  MCD Application Team
  * @brief   BLE Application
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2019-2021 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"

#include "app_common.h"

#include "dbg_trace.h"
#include "ble.h"
#include "tl.h"
#include "app_ble.h"

#include "cmsis_os.h"
#include "shci.h"
#include "stm32_lpm.h"
#include "otp.h"
#include "dis_app.h"
#include "hrs_app.h"
#include "ancs_client_app.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/

/**
 * security parameters structure
 */
typedef struct _tSecurityParams
{
  /**
   * IO capability of the device
   */
  uint8_t ioCapability;

  /**
   * Authentication requirement of the device
   * Man In the Middle protection required?
   */
  uint8_t mitm_mode;

  /**
   * bonding mode of the device
   */
  uint8_t bonding_mode;

  /**
   * Flag to tell whether OOB data has
   * to be used during the pairing process
   */
  uint8_t OOB_Data_Present; 

  /**
   * OOB data to be used in the pairing process if
   * OOB_Data_Present is set to TRUE
   */
  uint8_t OOB_Data[16]; 

  /**
   * this variable indicates whether to use a fixed pin
   * during the pairing process or a passkey has to be
   * requested to the application during the pairing process
   * 0 implies use fixed pin and 1 implies request for passkey
   */
  uint8_t Use_Fixed_Pin;

  /**
   * minimum encryption key size requirement
   */
  uint8_t encryptionKeySizeMin;

  /**
   * maximum encryption key size requirement
   */
  uint8_t encryptionKeySizeMax;

  /**
   * fixed pin to be used in the pairing process if
   * Use_Fixed_Pin is set to 1
   */
  uint32_t Fixed_Pin;

  /**
   * this flag indicates whether the host has to initiate
   * the security, wait for pairing or does not have any security
   * requirements.\n
   * 0x00 : no security required
   * 0x01 : host should initiate security by sending the slave security
   *        request command
   * 0x02 : host need not send the clave security request but it
   * has to wait for paiirng to complete before doing any other
   * processing
   */
  uint8_t initiateSecurity;
}tSecurityParams;

/**
 * global context
 * contains the variables common to all
 * services
 */
typedef struct _tBLEProfileGlobalContext
{

  /**
   * security requirements of the host
   */
  tSecurityParams bleSecurityParam;

  /**
   * gap service handle
   */
  uint16_t gapServiceHandle;

  /**
   * device name characteristic handle
   */
  uint16_t devNameCharHandle;

  /**
   * appearance characteristic handle
   */
  uint16_t appearanceCharHandle;

  /**
   * connection handle of the current active connection
   * When not in connection, the handle is set to 0xFFFF
   */
  uint16_t connectionHandle;

  /**
   * length of the UUID list to be used while advertising
   */
  uint8_t advtServUUIDlen;

  /**
   * the UUID list to be used while advertising
   */
  uint8_t advtServUUID[100];

}BleGlobalContext_t;

typedef struct
{
  BleGlobalContext_t BleApplicationContext_legacy;
  APP_BLE_ConnStatus_t Device_Connection_Status;
  uint16_t connection_handle;
  uint8_t Peer_Bonded;
  uint8_t Peer_Address_Type;
  uint8_t Peer_Address[6];
  uint8_t Security_Request;
  uint8_t Security_Mode;
  uint8_t Security_Level;

  /**
   * ID of the Advertising Timeout
   */
  uint8_t Advertising_mgr_timer_Id;

}BleApplicationContext_t;
/* USER CODE BEGIN PTD */
typedef enum
{
  GAP_PROC_TERMINATE_CONNECTION,
  GAP_PROC_SLAVE_SECURITY_REQ,
  GAP_PROC_PASS_KEY_RESPONSE,
  GAP_PROC_NUMERIC_COMPARISON_VALUE_CONFIRM,
  GAP_PROC_ALLOW_REBOND,
} GapProcId_t;

/* USER CODE END PTD */

/* Private defines -----------------------------------------------------------*/
#define APPBLE_GAP_DEVICE_NAME_LENGTH 7
#define FAST_ADV_TIMEOUT               (30*1000*1000/CFG_TS_TICK_VAL) /**< 30s */
#define INITIAL_ADV_TIMEOUT            (60*1000*1000/CFG_TS_TICK_VAL) /**< 60s */

#define BD_ADDR_SIZE_LOCAL    6

/* USER CODE BEGIN PD */
#define LED_ON_TIMEOUT                 (0.005*1000*1000/CFG_TS_TICK_VAL) /**< 5ms */

/**
 * The default GAP command timeout is set to 30s
 */
#define GAP_DEFAULT_TIMEOUT (30000)
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
PLACE_IN_SECTION("MB_MEM1") ALIGN(4) static TL_CmdPacket_t BleCmdBuffer;

static const uint8_t M_bd_addr[BD_ADDR_SIZE_LOCAL] =
    {
        (uint8_t)((CFG_ADV_BD_ADDRESS & 0x0000000000FF)),
        (uint8_t)((CFG_ADV_BD_ADDRESS & 0x00000000FF00) >> 8),
        (uint8_t)((CFG_ADV_BD_ADDRESS & 0x000000FF0000) >> 16),
        (uint8_t)((CFG_ADV_BD_ADDRESS & 0x0000FF000000) >> 24),
        (uint8_t)((CFG_ADV_BD_ADDRESS & 0x00FF00000000) >> 32),
        (uint8_t)((CFG_ADV_BD_ADDRESS & 0xFF0000000000) >> 40)
    };

static uint8_t bd_addr_udn[BD_ADDR_SIZE_LOCAL];

/**
*   Identity root key used to derive LTK and CSRK
*/
static const uint8_t BLE_CFG_IR_VALUE[16] = CFG_BLE_IRK;

/**
* Encryption root key used to derive LTK and CSRK
*/
static const uint8_t BLE_CFG_ER_VALUE[16] = CFG_BLE_ERK;

/**
 * These are the two tags used to manage a power failure during OTA
 * The MagicKeywordAdress shall be mapped @0x140 from start of the binary image
 * The MagicKeywordvalue is checked in the ble_ota application
 */
PLACE_IN_SECTION("TAG_OTA_END") const uint32_t MagicKeywordValue = 0x94448A29 ;
PLACE_IN_SECTION("TAG_OTA_START") const uint32_t MagicKeywordAddress = (uint32_t)&MagicKeywordValue;

static BleApplicationContext_t BleApplicationContext;
static uint16_t AdvIntervalMin, AdvIntervalMax;

static const char local_name[] = { AD_TYPE_COMPLETE_LOCAL_NAME ,'H','R','a','n','c'};
uint8_t  manuf_data[14] = {
    sizeof(manuf_data)-1, AD_TYPE_MANUFACTURER_SPECIFIC_DATA,
    0x01/*SKD version */,
    0x00 /* Generic*/,
    0x00 /* GROUP A Feature  */,
    0x00 /* GROUP A Feature */,
    0x00 /* GROUP B Feature */,
    0x00 /* GROUP B Feature */,
    0x00, /* BLE MAC start -MSB */
    0x00,
    0x00,
    0x00,
    0x00,
    0x00, /* BLE MAC stop */

};

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Global variables ----------------------------------------------------------*/
osMutexId_t MtxHciId;
osSemaphoreId_t SemHciId;
osSemaphoreId_t SemGapId;
osThreadId_t AdvUpdateProcessId;
osThreadId_t HciUserEvtProcessId;

const osThreadAttr_t AdvUpdateProcess_attr = {
    .name = CFG_ADV_UPDATE_PROCESS_NAME,
    .attr_bits = CFG_ADV_UPDATE_PROCESS_ATTR_BITS,
    .cb_mem = CFG_ADV_UPDATE_PROCESS_CB_MEM,
    .cb_size = CFG_ADV_UPDATE_PROCESS_CB_SIZE,
    .stack_mem = CFG_ADV_UPDATE_PROCESS_STACK_MEM,
    .priority = CFG_ADV_UPDATE_PROCESS_PRIORITY,
    .stack_size = CFG_ADV_UPDATE_PROCESS_STACK_SIZE
};

const osThreadAttr_t HciUserEvtProcess_attr = {
    .name = CFG_HCI_USER_EVT_PROCESS_NAME,
    .attr_bits = CFG_HCI_USER_EVT_PROCESS_ATTR_BITS,
    .cb_mem = CFG_HCI_USER_EVT_PROCESS_CB_MEM,
    .cb_size = CFG_HCI_USER_EVT_PROCESS_CB_SIZE,
    .stack_mem = CFG_HCI_USER_EVT_PROCESS_STACK_MEM,
    .priority = CFG_HCI_USER_EVT_PROCESS_PRIORITY,
    .stack_size = CFG_HCI_USER_EVT_PROCESS_STACK_SIZE
};

/* Private function prototypes -----------------------------------------------*/
static void HciUserEvtProcess(void *argument);
static void BLE_UserEvtRx( void * pPayload );
static void BLE_StatusNot( HCI_TL_CmdStatus_t status );
static void Ble_Tl_Init( void );
static void Ble_Hci_Gap_Gatt_Init(void);
static const uint8_t* BleGetBdAddress( void );
static void Adv_Request( APP_BLE_ConnStatus_t New_Status );
static void Add_Advertisment_Service_UUID( uint16_t servUUID );
static void Adv_Mgr( void );
static void AdvUpdateProcess(void *argument);
static void Adv_Update( void );

/* USER CODE BEGIN PFP */
static void gap_cmd_resp_wait(uint32_t timeout);
static void gap_cmd_resp_release(uint32_t flag);
static void GapProcReq(GapProcId_t GapProcId);
/* USER CODE END PFP */

/* Functions Definition ------------------------------------------------------*/
void APP_BLE_Init( void )
{
  SHCI_CmdStatus_t status;
/* USER CODE BEGIN APP_BLE_Init_1 */

/* USER CODE END APP_BLE_Init_1 */
  SHCI_C2_Ble_Init_Cmd_Packet_t ble_init_cmd_packet =
  {
    {{0,0,0}},                          /**< Header unused */
    {0,                                 /** pBleBufferAddress not used */
    0,                                  /** BleBufferSize not used */
    CFG_BLE_NUM_GATT_ATTRIBUTES,
    CFG_BLE_NUM_GATT_SERVICES,
    CFG_BLE_ATT_VALUE_ARRAY_SIZE,
    CFG_BLE_NUM_LINK,
    CFG_BLE_DATA_LENGTH_EXTENSION,
    CFG_BLE_PREPARE_WRITE_LIST_SIZE,
    CFG_BLE_MBLOCK_COUNT,
    CFG_BLE_MAX_ATT_MTU,
    CFG_BLE_SLAVE_SCA,
    CFG_BLE_MASTER_SCA,
    CFG_BLE_LS_SOURCE,
    CFG_BLE_MAX_CONN_EVENT_LENGTH,
    CFG_BLE_HSE_STARTUP_TIME,
    CFG_BLE_VITERBI_MODE,
    CFG_BLE_OPTIONS,
    0,
    CFG_BLE_MAX_COC_INITIATOR_NBR,
    CFG_BLE_MIN_TX_POWER,
    CFG_BLE_MAX_TX_POWER,
    CFG_BLE_RX_MODEL_CONFIG,
     CFG_BLE_MAX_ADV_SET_NBR, 
     CFG_BLE_MAX_ADV_DATA_LEN,
     CFG_BLE_TX_PATH_COMPENS,
     CFG_BLE_RX_PATH_COMPENS,
     CFG_BLE_CORE_VERSION
    }
  };

  /**
   * Initialize Ble Transport Layer
   */
  Ble_Tl_Init( );

  /**
   * Do not allow standby in the application
   */
  UTIL_LPM_SetOffMode(1 << CFG_LPM_APP_BLE, UTIL_LPM_DISABLE);

  /**
   * Register the hci transport layer to handle BLE User Asynchronous Events
   */
  HciUserEvtProcessId = osThreadNew(HciUserEvtProcess, NULL, &HciUserEvtProcess_attr);

  /**
   * Starts the BLE Stack on CPU2
   */
  status = SHCI_C2_BLE_Init(&ble_init_cmd_packet);
  if (status != SHCI_Success)
  {
    APP_DBG_MSG("  Fail   : SHCI_C2_BLE_Init command, result: 0x%02x\n\r", status);
    /* if you are here, maybe CPU2 doesn't contain STM32WB_Copro_Wireless_Binaries, see Release_Notes.html */
    Error_Handler();
  }
  else
  {
    APP_DBG_MSG("  Success: SHCI_C2_BLE_Init command\n\r");
  }

  /**
   * Initialization of HCI & GATT & GAP layer
   */
  Ble_Hci_Gap_Gatt_Init();

  /**
   * Initialization of the BLE Services
   */
  SVCCTL_Init();

  /**
   * Initialization of the BLE App Context
   */
  BleApplicationContext.Device_Connection_Status = APP_BLE_IDLE;
  BleApplicationContext.BleApplicationContext_legacy.connectionHandle = 0xFFFF;
  /**
   * From here, all initialization are BLE application specific
   */
  AdvUpdateProcessId = osThreadNew(AdvUpdateProcess, NULL, &AdvUpdateProcess_attr);

  /**
   * Initialization of ADV - Ad Manufacturer Element - Support OTA Bit Mask
   */
#if(BLE_CFG_OTA_REBOOT_CHAR != 0)
  manuf_data[sizeof(manuf_data)-8] = CFG_FEATURE_OTA_REBOOT;
#endif
  /**
   * Initialize DIS Application
   */
  DISAPP_Init();

  /**
   * Initialize HRS Application
   */
  HRSAPP_Init();
  
  /**
  * Initialize ANCS Application
  */  
  ANCS_Client_App_Init();

  /**
   * Create timer to handle the connection state machine
   */

  HW_TS_Create(CFG_TIM_PROC_ID_ISR, &(BleApplicationContext.Advertising_mgr_timer_Id), hw_ts_SingleShot, Adv_Mgr);

  /**
   * Make device discoverable
   */
  BleApplicationContext.BleApplicationContext_legacy.advtServUUID[0] = AD_TYPE_16_BIT_SERV_UUID;
  BleApplicationContext.BleApplicationContext_legacy.advtServUUIDlen = 1;
  Add_Advertisment_Service_UUID(HEART_RATE_SERVICE_UUID);
  /* Initialize intervals for reconnexion without intervals update */
  AdvIntervalMin = CFG_FAST_CONN_ADV_INTERVAL_MIN;
  AdvIntervalMax = CFG_FAST_CONN_ADV_INTERVAL_MAX;

  /**
  * Start to Advertise to be connected by Collector
   */
   Adv_Request(APP_BLE_FAST_ADV);

/* USER CODE BEGIN APP_BLE_Init_2 */

/* USER CODE END APP_BLE_Init_2 */
  return;
}

SVCCTL_UserEvtFlowStatus_t SVCCTL_App_Notification( void *pckt )
{
  hci_event_pckt *event_pckt;
  evt_le_meta_event *meta_evt;
  evt_blecore_aci *blecore_evt;
  hci_le_phy_update_complete_event_rp0 *evt_le_phy_update_complete;
  uint8_t TX_PHY, RX_PHY;
  tBleStatus ret = BLE_STATUS_INVALID_PARAMS;
  Connection_Context_t Notification;

  event_pckt = (hci_event_pckt*) ((hci_uart_pckt *) pckt)->data;

  /* USER CODE BEGIN SVCCTL_App_Notification */

  /* USER CODE END SVCCTL_App_Notification */

  switch (event_pckt->evt)
  {
    case HCI_DISCONNECTION_COMPLETE_EVT_CODE:
    {
      hci_disconnection_complete_event_rp0 *disconnection_complete_event;
      disconnection_complete_event = (hci_disconnection_complete_event_rp0 *) event_pckt->data;

      if (disconnection_complete_event->Connection_Handle == BleApplicationContext.BleApplicationContext_legacy.connectionHandle)
      {
        BleApplicationContext.BleApplicationContext_legacy.connectionHandle = 0;
        BleApplicationContext.Device_Connection_Status = APP_BLE_IDLE;

        if(disconnection_complete_event->Reason == ERR_CMD_SUCCESS){
          APP_DBG_MSG("\r\n\r** DISCONNECTION EVENT WITH CLIENT disconnection Reason=0x%02X success \n\r",disconnection_complete_event->Reason);
        }else if(disconnection_complete_event->Reason == HCI_CONNECTION_TERMINATED_BY_LOCAL_HOST_ERR_CODE){
          APP_DBG_MSG("\r\n\r** DISCONNECTION EVENT WITH CLIENT disconnection Reason=0x%02X Connection terminated by local host \n\r",disconnection_complete_event->Reason);
        }else if(disconnection_complete_event->Reason == HCI_CONNECTION_TERMINATED_DUE_TO_MIC_FAILURE_ERR_CODE){
          APP_DBG_MSG("\r\n\r** DISCONNECTION EVENT WITH CLIENT disconnection Reason=0x%02X Connection terminated due to MIC failure \n\r",disconnection_complete_event->Reason);
          APP_BLE_Remove_Bonding_Info();
        }else{
          APP_DBG_MSG("\r\n\r** DISCONNECTION EVENT WITH CLIENT disconnection Reason=0x%02X \n\r",disconnection_complete_event->Reason);
        }
        	
        Notification.Evt_Opcode = ANCS_DISCONN_COMPLETE;
        Notification.connection_handle = disconnection_complete_event->Connection_Handle;
        ANCS_App_Notification(&Notification);
        gap_cmd_resp_release(0);
      }

      /* restart advertising */
      Adv_Request(APP_BLE_FAST_ADV);

      /* USER CODE BEGIN EVT_DISCONN_COMPLETE */

      /* USER CODE END EVT_DISCONN_COMPLETE */
    }

    break; /* HCI_DISCONNECTION_COMPLETE_EVT_CODE */

    case HCI_LE_META_EVT_CODE:
    {
      meta_evt = (evt_le_meta_event*) event_pckt->data;
      /* USER CODE BEGIN EVT_LE_META_EVENT */

      /* USER CODE END EVT_LE_META_EVENT */
      switch (meta_evt->subevent)
      {
        case HCI_LE_CONNECTION_UPDATE_COMPLETE_SUBEVT_CODE:
        {
          APP_DBG_MSG("\r\n\r** CONNECTION UPDATE EVENT WITH CLIENT \n");

          /* USER CODE BEGIN EVT_LE_CONN_UPDATE_COMPLETE */
          hci_le_connection_update_complete_event_rp0 *connection_update_complete = (hci_le_connection_update_complete_event_rp0 *) meta_evt->data;
          APP_DBG_MSG("HCI_LE_CONNECTION_UPDATE_COMPLETE_SUBEVT_CODE Status=0x%02X Connection_Handle=0x%04X Conn_Interval=0x%04X Conn_Latency=0x%04X Supervision_Timeout=0x%04X \n\r",
          connection_update_complete->Status,connection_update_complete->Connection_Handle,connection_update_complete->Conn_Interval,connection_update_complete->Conn_Latency,connection_update_complete->Supervision_Timeout);
          /* USER CODE END EVT_LE_CONN_UPDATE_COMPLETE */
        }
          break;
        case HCI_LE_PHY_UPDATE_COMPLETE_SUBEVT_CODE:
          APP_DBG_MSG("EVT_UPDATE_PHY_COMPLETE \n");
          evt_le_phy_update_complete = (hci_le_phy_update_complete_event_rp0*)meta_evt->data;
          if (evt_le_phy_update_complete->Status == 0)
          {
            APP_DBG_MSG("EVT_UPDATE_PHY_COMPLETE, status ok \n");
          }
          else
          {
            APP_DBG_MSG("EVT_UPDATE_PHY_COMPLETE, status nok \n");
          }

          ret = hci_le_read_phy(BleApplicationContext.BleApplicationContext_legacy.connectionHandle,&TX_PHY,&RX_PHY);
          if (ret == BLE_STATUS_SUCCESS)
          {
            APP_DBG_MSG("Read_PHY success \n");

            if ((TX_PHY == TX_2M) && (RX_PHY == RX_2M))
            {
              APP_DBG_MSG("PHY Param  TX= %d, RX= %d \n", TX_PHY, RX_PHY);
            }
            else
            {
              APP_DBG_MSG("PHY Param  TX= %d, RX= %d \n", TX_PHY, RX_PHY);
            }
          }
          else
          {
            APP_DBG_MSG("Read conf not success \n");
          }
          /* USER CODE BEGIN EVT_LE_PHY_UPDATE_COMPLETE */

          /* USER CODE END EVT_LE_PHY_UPDATE_COMPLETE */
          break;
        case HCI_LE_CONNECTION_COMPLETE_SUBEVT_CODE:
        {
          hci_le_connection_complete_event_rp0 *connection_complete_event;

          /**
           * The connection is done, there is no need anymore to schedule the LP ADV
           */
          connection_complete_event = (hci_le_connection_complete_event_rp0 *) meta_evt->data;

          HW_TS_Stop(BleApplicationContext.Advertising_mgr_timer_Id);

          APP_DBG_MSG("HCI_LE_CONNECTION_COMPLETE_SUBEVT_CODE for connection handle 0x%x\n", connection_complete_event->Connection_Handle);
          if (BleApplicationContext.Device_Connection_Status == APP_BLE_LP_CONNECTING)
          {
            /* Connection as client */
            BleApplicationContext.Device_Connection_Status = APP_BLE_CONNECTED_CLIENT;
          }
          else
          {
            /* Connection as server */
            BleApplicationContext.Device_Connection_Status = APP_BLE_CONNECTED_SERVER;
          }
          BleApplicationContext.BleApplicationContext_legacy.connectionHandle = connection_complete_event->Connection_Handle;
          /* USER CODE BEGIN HCI_EVT_LE_CONN_COMPLETE */
	   APP_DBG_MSG("HCI_LE_CONNECTION_COMPLETE_SUBEVT_CODE Connection_Handle=0x%04X Role=%d Peer_Address_Type=%d Peer_Address:%02X %02X %02X %02X %02X %02X \n\r",
		  	connection_complete_event->Connection_Handle,
		  	connection_complete_event->Role,
		  	connection_complete_event->Peer_Address_Type,
		  	connection_complete_event->Peer_Address[5],
		  	connection_complete_event->Peer_Address[4],
		  	connection_complete_event->Peer_Address[3],
		  	connection_complete_event->Peer_Address[2],
		  	connection_complete_event->Peer_Address[1],
		  	connection_complete_event->Peer_Address[0]);

          BleApplicationContext.connection_handle = connection_complete_event->Connection_Handle;
		  
          BleApplicationContext.Peer_Address_Type = connection_complete_event->Peer_Address_Type;
          BleApplicationContext.Peer_Address[5] = connection_complete_event->Peer_Address[5];
          BleApplicationContext.Peer_Address[4] = connection_complete_event->Peer_Address[4];
          BleApplicationContext.Peer_Address[3] = connection_complete_event->Peer_Address[3];
          BleApplicationContext.Peer_Address[2] = connection_complete_event->Peer_Address[2];
          BleApplicationContext.Peer_Address[1] = connection_complete_event->Peer_Address[1];
          BleApplicationContext.Peer_Address[0] = connection_complete_event->Peer_Address[0];
          APP_BLE_Peer_Bonded_Check();
		  
          Notification.Evt_Opcode = ANCS_CONNECTED;
          Notification.connection_handle = connection_complete_event->Connection_Handle;
	   ANCS_App_Notification(&Notification);
          /* USER CODE END HCI_EVT_LE_CONN_COMPLETE */
        }
        break; /* HCI_LE_CONNECTION_COMPLETE_SUBEVT_CODE */

        /* USER CODE BEGIN META_EVT */

        /* USER CODE END META_EVT */

        default:
          /* USER CODE BEGIN SUBEVENT_DEFAULT */

          /* USER CODE END SUBEVENT_DEFAULT */
          break;
      }
    }
    break; /* HCI_LE_META_EVT_CODE */

    case HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE:
      blecore_evt = (evt_blecore_aci*) event_pckt->data;
      /* USER CODE BEGIN EVT_VENDOR */

      /* USER CODE END EVT_VENDOR */
      switch (blecore_evt->ecode)
      {
      /* USER CODE BEGIN ecode */
        aci_gap_pairing_complete_event_rp0 *pairing_complete;

      case ACI_GAP_LIMITED_DISCOVERABLE_VSEVT_CODE: 
        APP_DBG_MSG("\r\n\r** ACI_GAP_LIMITED_DISCOVERABLE_VSEVT_CODE \n");
          break; /* ACI_GAP_LIMITED_DISCOVERABLE_VSEVT_CODE */
          
      case ACI_GAP_PASS_KEY_REQ_VSEVT_CODE:  
      {
        aci_gap_pass_key_req_event_rp0 *gap_evt_pass_key_req = (aci_gap_pass_key_req_event_rp0*) blecore_evt->data;
        APP_DBG_MSG("ACI_GAP_PASS_KEY_REQ_VSEVT_CODE ==> GAP_PROC_PASS_KEY_RESPONSE Connection_Handle=0x%04X \n\r",gap_evt_pass_key_req->Connection_Handle);
        GapProcReq(GAP_PROC_PASS_KEY_RESPONSE);
      }
          break; /* ACI_GAP_PASS_KEY_REQ_VSEVT_CODE */

      case ACI_GAP_AUTHORIZATION_REQ_VSEVT_CODE:    
        APP_DBG_MSG("\r\n\r** ACI_GAP_AUTHORIZATION_REQ_VSEVT_CODE \n");
          break; /* ACI_GAP_AUTHORIZATION_REQ_VSEVT_CODE */

      case ACI_GAP_SLAVE_SECURITY_INITIATED_VSEVT_CODE: 
        APP_DBG_MSG("ACI_GAP_SLAVE_SECURITY_INITIATED_VSEVT_CODE slave security request is successfully sent to the master \n");
        //gap_cmd_resp_release(0);
          break; /* ACI_GAP_SLAVE_SECURITY_INITIATED_VSEVT_CODE */

      case ACI_GAP_BOND_LOST_VSEVT_CODE:    

        APP_DBG_MSG("ACI_GAP_BOND_LOST_VSEVT_CODE ==> GAP_PROC_ALLOW_REBOND \n\r");
        GapProcReq(GAP_PROC_ALLOW_REBOND);

          break; /* ACI_GAP_BOND_LOST_VSEVT_CODE */

      case ACI_GAP_ADDR_NOT_RESOLVED_VSEVT_CODE:
         APP_DBG_MSG("\r\n\r** ACI_GAP_ADDR_NOT_RESOLVED_VSEVT_CODE \n");
          break; /* ACI_GAP_ADDR_NOT_RESOLVED_VSEVT_CODE */
      
      case (ACI_GAP_KEYPRESS_NOTIFICATION_VSEVT_CODE):
         APP_DBG_MSG("\r\n\r** ACI_GAP_KEYPRESS_NOTIFICATION_VSEVT_CODE\n");
          break; /* ACI_GAP_KEYPRESS_NOTIFICATION_VSEVT_CODE */    

      case (ACI_GAP_NUMERIC_COMPARISON_VALUE_VSEVT_CODE):
        APP_DBG_MSG(" numeric_value = %ld\n", ((aci_gap_numeric_comparison_value_event_rp0 *)(blecore_evt->data))->Numeric_Value);
        APP_DBG_MSG(" Hex_value = %lx\n", ((aci_gap_numeric_comparison_value_event_rp0 *)(blecore_evt->data))->Numeric_Value);
        APP_DBG_MSG("ACI_GAP_NUMERIC_COMPARISON_VALUE_VSEVT_CODE ==> GAP_PROC_NUMERIC_COMPARISON_VALUE_CONFIRM \n\r");
        GapProcReq(GAP_PROC_NUMERIC_COMPARISON_VALUE_CONFIRM);
          break;

       case (ACI_GAP_PAIRING_COMPLETE_VSEVT_CODE):
          {
            pairing_complete = (aci_gap_pairing_complete_event_rp0*)blecore_evt->data;

            APP_DBG_MSG("ACI_GAP_PAIRING_COMPLETE_VSEVT_CODE, Connection_Handle=0x%04X Status=%d Reason=0x%02x \n",pairing_complete->Connection_Handle,pairing_complete->Status,pairing_complete->Reason);
            if(pairing_complete->Status == SM_PAIRING_TIMEOUT){
              APP_DBG_MSG(" ** Pairing Timeout Status=%d Reason=0x%02x , \n\r !!! Please ignore this BLE Device on the iOS/Android Device Setting=>Bluetooth=>My Device or Paired Device !!! \n\r",pairing_complete->Status,pairing_complete->Reason);
              APP_BLE_Remove_Bonding_Info();
            }else if(pairing_complete->Status == SM_PAIRING_FAILED){
              APP_DBG_MSG(" ** Pairing KO Status=%d Reason=0x%02x , \n\r !!! Please ignore this BLE Device on the iOS/Android Device Setting=>Bluetooth=>My Device or Paired Device !!! \n\r",pairing_complete->Status,pairing_complete->Reason);
              APP_BLE_Remove_Bonding_Info();
            }else if(pairing_complete->Status == SM_PAIRING_SUCCESS){
              uint8_t Peer_Bonded,Security_Mode, Security_Level;
              Peer_Bonded = BleApplicationContext.Peer_Bonded;
              Security_Mode = BleApplicationContext.Security_Mode;
              Security_Level = BleApplicationContext.Security_Level;
        
              APP_BLE_Peer_Bonded_Check();
              APP_DBG_MSG("ACI_GAP_PAIRING_COMPLETE_VSEVT_CODE  Peer_Bonded =%d => %d  Security_Mode=%d => %d Security_Level=%d => %d \n\r",
              Peer_Bonded,BleApplicationContext.Peer_Bonded,Security_Mode,BleApplicationContext.Security_Mode,Security_Level,BleApplicationContext.Security_Level);

              if (Peer_Bonded == 0x00)/* only for the first paring complete*/
              {
                APP_DBG_MSG("Term Connection for the first pairing complete to save bonding information !!! \n\r");

                for (int loop=0;loop<10;loop++) /* */
                {
                  /* hci_disconnection_complete_event event will be generated when the link is disconnected. 
                  It is important to leave an 100 ms blank window before sending any new command (including system hardware reset), 
                  since immediately after @ref hci_disconnection_complete_event event, system could save important information in non volatile memory. */
                  int cnt = 1000000;
                  while(cnt--);
                  printf(".\n\r");
                }

                ret = aci_gap_terminate(BleApplicationContext.connection_handle, 0x13);
                if (ret == BLE_STATUS_SUCCESS)
                {
                  Notification.Evt_Opcode = ANCS_DISCONNECTING;
                  ANCS_App_Notification(&Notification);
                }		 
              }
            } /* SM_PAIRING_SUCCESS */

	     if(BleApplicationContext.Security_Request == 0x01)
             gap_cmd_resp_release(0);

          }
           break; /* ACI_GAP_PAIRING_COMPLETE_VSEVT_CODE */

        case ACI_GATT_PROC_COMPLETE_VSEVT_CODE:
        APP_DBG_MSG(" ** ACI_GATT_PROC_COMPLETE_VSEVT_CODE \n");
        /* USER CODE BEGIN ACI_GATT_PROC_COMPLETE_VSEVT_CODE */

        /* USER CODE END ACI_GATT_PROC_COMPLETE_VSEVT_CODE */
          break; /* ACI_GATT_PROC_COMPLETE_VSEVT_CODE */
      /* USER CODE END ecode */
        case ACI_GAP_PROC_COMPLETE_VSEVT_CODE:
        APP_DBG_MSG("\r\n\r** ACI_GAP_PROC_COMPLETE_VSEVT_CODE \n");
        /* USER CODE BEGIN EVT_BLUE_GAP_PROCEDURE_COMPLETE */

        /* USER CODE END EVT_BLUE_GAP_PROCEDURE_COMPLETE */
          break; /* ACI_GAP_PROC_COMPLETE_VSEVT_CODE */

      /* USER CODE BEGIN BLUE_EVT */

      /* USER CODE END BLUE_EVT */
      }
      break; /* HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE */

      /* USER CODE BEGIN EVENT_PCKT */

      /* USER CODE END EVENT_PCKT */

      default:
      /* USER CODE BEGIN ECODE_DEFAULT*/

      /* USER CODE END ECODE_DEFAULT*/
      break;
  }

  return (SVCCTL_UserEvtFlowEnable);
}

APP_BLE_ConnStatus_t APP_BLE_Get_Server_Connection_Status(void)
{
    return BleApplicationContext.Device_Connection_Status;
}

/* USER CODE BEGIN FD*/
void APP_BLE_Key_Button1_Action(void)
{
  //ANCS_App_KeyButton1Action();
  APP_DBG_MSG("\n\r ** Term CONNECTION **  \n\r");
  aci_gap_terminate(BleApplicationContext.connection_handle, 0x13);
}

void APP_BLE_Key_Button2_Action(void)
{
  ANCS_App_KeyButton2Action();
}
  
void APP_BLE_Key_Button3_Action(void)
{
  //ANCS_App_KeyButton3Action();
  APP_DBG_MSG(" aci_gap_clear_security_db & aci_gap_remove_bonded_device & aci_gap_terminate \n\r");
  aci_gap_remove_bonded_device(BleApplicationContext.Peer_Address_Type,BleApplicationContext.Peer_Address);
  aci_gap_clear_security_db();
  aci_gap_terminate(BleApplicationContext.connection_handle, 0x13);
}

/* USER CODE END FD*/
/*************************************************************
 *
 * LOCAL FUNCTIONS
 *
 *************************************************************/
static void Ble_Tl_Init( void )
{
  HCI_TL_HciInitConf_t Hci_Tl_Init_Conf;

  MtxHciId = osMutexNew( NULL );
  SemHciId = osSemaphoreNew( 1, 0, NULL ); /*< Create the semaphore and make it busy at initialization */
  SemGapId = osSemaphoreNew( 1, 0, NULL ); /*< Create the semaphore and make it busy at initialization */

  Hci_Tl_Init_Conf.p_cmdbuffer = (uint8_t*)&BleCmdBuffer;
  Hci_Tl_Init_Conf.StatusNotCallBack = BLE_StatusNot;
  hci_init(BLE_UserEvtRx, (void*) &Hci_Tl_Init_Conf);

  return;
}

static void Ble_Hci_Gap_Gatt_Init(void){

  uint8_t role;
  uint8_t index;
  uint16_t gap_service_handle, gap_dev_name_char_handle, gap_appearance_char_handle;
  const uint8_t *bd_addr;
  uint32_t srd_bd_addr[2];
  uint16_t appearance[1] = { BLE_CFG_GAP_APPEARANCE };

  /**
   * Initialize HCI layer
   */
  /*HCI Reset to synchronise BLE Stack*/
  hci_reset();

  /**
   * Write the BD Address
   */

  bd_addr = BleGetBdAddress();
  aci_hal_write_config_data(CONFIG_DATA_PUBADDR_OFFSET,
                            CONFIG_DATA_PUBADDR_LEN,
                            (uint8_t*) bd_addr);

  /* BLE MAC in ADV Packet */
  manuf_data[ sizeof(manuf_data)-6] = bd_addr[5];
  manuf_data[ sizeof(manuf_data)-5] = bd_addr[4];
  manuf_data[ sizeof(manuf_data)-4] = bd_addr[3];
  manuf_data[ sizeof(manuf_data)-3] = bd_addr[2];
  manuf_data[ sizeof(manuf_data)-2] = bd_addr[1];
  manuf_data[ sizeof(manuf_data)-1] = bd_addr[0];

  /**
   * Write Identity root key used to derive LTK and CSRK
   */
    aci_hal_write_config_data(CONFIG_DATA_IR_OFFSET,
    CONFIG_DATA_IR_LEN,
                            (uint8_t*) BLE_CFG_IR_VALUE);

   /**
   * Write Encryption root key used to derive LTK and CSRK
   */
    aci_hal_write_config_data(CONFIG_DATA_ER_OFFSET,
    CONFIG_DATA_ER_LEN,
                            (uint8_t*) BLE_CFG_ER_VALUE);

   /**
   * Write random bd_address
   */
   /* random_bd_address = R_bd_address;
    aci_hal_write_config_data(CONFIG_DATA_RANDOM_ADDRESS_WR,
    CONFIG_DATA_RANDOM_ADDRESS_LEN,
                            (uint8_t*) random_bd_address);
  */

  /**
   * Static random Address
   * The two upper bits shall be set to 1
   * The lowest 32bits is read from the UDN to differentiate between devices
   * The RNG may be used to provide a random number on each power on
   */
  srd_bd_addr[1] =  0x0000ED6E;
  srd_bd_addr[0] =  LL_FLASH_GetUDN( );
  aci_hal_write_config_data( CONFIG_DATA_RANDOM_ADDRESS_OFFSET, CONFIG_DATA_RANDOM_ADDRESS_LEN, (uint8_t*)srd_bd_addr );

  /**
   * Write Identity root key used to derive LTK and CSRK
   */
    aci_hal_write_config_data( CONFIG_DATA_IR_OFFSET, CONFIG_DATA_IR_LEN, (uint8_t*)BLE_CFG_IR_VALUE );

   /**
   * Write Encryption root key used to derive LTK and CSRK
   */
    aci_hal_write_config_data( CONFIG_DATA_ER_OFFSET, CONFIG_DATA_ER_LEN, (uint8_t*)BLE_CFG_ER_VALUE );

  /**
   * Set TX Power to 0dBm.
   */
  aci_hal_set_tx_power_level(1, CFG_TX_POWER);

  /**
   * Initialize GATT interface
   */
  aci_gatt_init();

  /**
   * Initialize GAP interface
   */
  role = 0;

#if (BLE_CFG_PERIPHERAL == 1)
  role |= GAP_PERIPHERAL_ROLE;
#endif

#if (BLE_CFG_CENTRAL == 1)
  role |= GAP_CENTRAL_ROLE;
#endif /* BLE_CFG_CENTRAL == 1 */

/* USER CODE BEGIN Role_Mngt*/

/* USER CODE END Role_Mngt */

  if (role > 0)
  {
    const char *name = "HRanc";
    aci_gap_init(role, 0,
                 APPBLE_GAP_DEVICE_NAME_LENGTH,
                 &gap_service_handle, &gap_dev_name_char_handle, &gap_appearance_char_handle);

    if (aci_gatt_update_char_value(gap_service_handle, gap_dev_name_char_handle, 0, strlen(name), (uint8_t *) name))
    {
      BLE_DBG_SVCCTL_MSG("Device Name aci_gatt_update_char_value failed.\n");
    }
  }

  if(aci_gatt_update_char_value(gap_service_handle,
                                gap_appearance_char_handle,
                                0,
                                2,
                                (uint8_t *)&appearance))
  {
    BLE_DBG_SVCCTL_MSG("Appearance aci_gatt_update_char_value failed.\n");
  }
  /**
   * Initialize Default PHY
   */
  hci_le_set_default_phy(ALL_PHYS_PREFERENCE,TX_2M_PREFERRED,RX_2M_PREFERRED);

  /**
   * Initialize IO capability
   */
  BleApplicationContext.BleApplicationContext_legacy.bleSecurityParam.ioCapability = CFG_IO_CAPABILITY;
  aci_gap_set_io_capability(BleApplicationContext.BleApplicationContext_legacy.bleSecurityParam.ioCapability);

  /**
   * Initialize authentication
   */
  BleApplicationContext.BleApplicationContext_legacy.bleSecurityParam.mitm_mode = CFG_MITM_PROTECTION;
  BleApplicationContext.BleApplicationContext_legacy.bleSecurityParam.OOB_Data_Present = CFG_OOB_DATA_PRESENT;
  BleApplicationContext.BleApplicationContext_legacy.bleSecurityParam.encryptionKeySizeMin = CFG_ENCRYPTION_KEY_SIZE_MIN;
  BleApplicationContext.BleApplicationContext_legacy.bleSecurityParam.encryptionKeySizeMax = CFG_ENCRYPTION_KEY_SIZE_MAX;
  BleApplicationContext.BleApplicationContext_legacy.bleSecurityParam.Use_Fixed_Pin = CFG_USED_FIXED_PIN;
  BleApplicationContext.BleApplicationContext_legacy.bleSecurityParam.Fixed_Pin = CFG_FIXED_PIN;
  BleApplicationContext.BleApplicationContext_legacy.bleSecurityParam.bonding_mode = CFG_BONDING_MODE;
  for (index = 0; index < 16; index++)
  {
    BleApplicationContext.BleApplicationContext_legacy.bleSecurityParam.OOB_Data[index] = (uint8_t) index;
  }

  aci_gap_set_authentication_requirement(BleApplicationContext.BleApplicationContext_legacy.bleSecurityParam.bonding_mode,
                                         BleApplicationContext.BleApplicationContext_legacy.bleSecurityParam.mitm_mode,
                                         CFG_SC_SUPPORT,
                                         CFG_KEYPRESS_NOTIFICATION_SUPPORT,
                                         BleApplicationContext.BleApplicationContext_legacy.bleSecurityParam.encryptionKeySizeMin,
                                         BleApplicationContext.BleApplicationContext_legacy.bleSecurityParam.encryptionKeySizeMax,
                                         BleApplicationContext.BleApplicationContext_legacy.bleSecurityParam.Use_Fixed_Pin,
                                         BleApplicationContext.BleApplicationContext_legacy.bleSecurityParam.Fixed_Pin,
                                         CFG_IDENTITY_ADDRESS_TYPE
                                         );

  /**
   * Initialize whitelist
   */
   if (BleApplicationContext.BleApplicationContext_legacy.bleSecurityParam.bonding_mode)
   {
     aci_gap_configure_whitelist();
   }
}

static void Adv_Request(APP_BLE_ConnStatus_t New_Status)
{
  tBleStatus ret = BLE_STATUS_INVALID_PARAMS;
  uint16_t Min_Inter, Max_Inter;

  if (New_Status == APP_BLE_FAST_ADV)
  {
    Min_Inter = AdvIntervalMin;
    Max_Inter = AdvIntervalMax;
  }
  else
  {
    Min_Inter = CFG_LP_CONN_ADV_INTERVAL_MIN;
    Max_Inter = CFG_LP_CONN_ADV_INTERVAL_MAX;
  }

    /**
     * Stop the timer, it will be restarted for a new shot
     * It does not hurt if the timer was not running
     */
    HW_TS_Stop(BleApplicationContext.Advertising_mgr_timer_Id);

    APP_DBG_MSG("First index in %d state \n", BleApplicationContext.Device_Connection_Status);

    if ((New_Status == APP_BLE_LP_ADV)
        && ((BleApplicationContext.Device_Connection_Status == APP_BLE_FAST_ADV)
            || (BleApplicationContext.Device_Connection_Status == APP_BLE_LP_ADV)))
    {
      /* Connection in ADVERTISE mode have to stop the current advertising */
      ret = aci_gap_set_non_discoverable();
      if (ret == BLE_STATUS_SUCCESS)
      {
        APP_DBG_MSG("Successfully Stopped Advertising \n");
      }
      else
      {
        APP_DBG_MSG("Stop Advertising Failed , result: %d \n", ret);
      }
    }

    BleApplicationContext.Device_Connection_Status = New_Status;
    /* Start Fast or Low Power Advertising */
    ret = aci_gap_set_discoverable(
        ADV_IND,
        Min_Inter,
        Max_Inter,
        PUBLIC_ADDR,
        NO_WHITE_LIST_USE, /* use white list */
        sizeof(local_name),
        (uint8_t*) &local_name,
        BleApplicationContext.BleApplicationContext_legacy.advtServUUIDlen,
        BleApplicationContext.BleApplicationContext_legacy.advtServUUID,
        0,
        0);

    /* Update Advertising data */
    ret = aci_gap_update_adv_data(sizeof(manuf_data), (uint8_t*) manuf_data);
    if (ret == BLE_STATUS_SUCCESS)
    {
      if (New_Status == APP_BLE_FAST_ADV)
      {
        APP_DBG_MSG("Successfully Start Fast Advertising \n" );
        /* Start Timer to STOP ADV - TIMEOUT */
        HW_TS_Start(BleApplicationContext.Advertising_mgr_timer_Id, INITIAL_ADV_TIMEOUT);
      }
      else
      {
        APP_DBG_MSG("Successfully Start Low Power Advertising \n");
      }
    }
    else
    {
      if (New_Status == APP_BLE_FAST_ADV)
      {
        APP_DBG_MSG("Start Fast Advertising Failed , result: %d \n", ret);
      }
      else
      {
        APP_DBG_MSG("Start Low Power Advertising Failed , result: %d \n", ret);
      }
    }

  return;
}

const uint8_t* BleGetBdAddress( void )
{
  uint8_t *otp_addr;
  const uint8_t *bd_addr;
  uint32_t udn;
  uint32_t company_id;
  uint32_t device_id;

  udn = LL_FLASH_GetUDN();

  if(udn != 0xFFFFFFFF)
  {
    company_id = LL_FLASH_GetSTCompanyID();
    device_id = LL_FLASH_GetDeviceID();

/**
 * Public Address with the ST company ID
 * bit[47:24] : 24bits (OUI) equal to the company ID
 * bit[23:16] : Device ID.
 * bit[15:0] : The last 16bits from the UDN
 * Note: In order to use the Public Address in a final product, a dedicated
 * 24bits company ID (OUI) shall be bought.
 */
    bd_addr_udn[0] = (uint8_t)(udn & 0x000000FF);
    bd_addr_udn[1] = (uint8_t)( (udn & 0x0000FF00) >> 8 );
    bd_addr_udn[2] = (uint8_t)device_id;
    bd_addr_udn[3] = (uint8_t)(company_id & 0x000000FF);
    bd_addr_udn[4] = (uint8_t)( (company_id & 0x0000FF00) >> 8 );
    bd_addr_udn[5] = (uint8_t)( (company_id & 0x00FF0000) >> 16 );

    bd_addr = (const uint8_t *)bd_addr_udn;
  }
  else
  {
    otp_addr = OTP_Read(0);
    if(otp_addr)
    {
      bd_addr = ((OTP_ID0_t*)otp_addr)->bd_address;
    }
    else
    {
      bd_addr = M_bd_addr;
    }
  }

  return bd_addr;
}

/* USER CODE BEGIN FD_LOCAL_FUNCTION */

/* USER CODE END FD_LOCAL_FUNCTION */

/*************************************************************
 *
 *SPECIFIC FUNCTIONS
 *
 *************************************************************/
static void Add_Advertisment_Service_UUID( uint16_t servUUID )
{
  BleApplicationContext.BleApplicationContext_legacy.advtServUUID[BleApplicationContext.BleApplicationContext_legacy.advtServUUIDlen] =
      (uint8_t) (servUUID & 0xFF);
  BleApplicationContext.BleApplicationContext_legacy.advtServUUIDlen++;
  BleApplicationContext.BleApplicationContext_legacy.advtServUUID[BleApplicationContext.BleApplicationContext_legacy.advtServUUIDlen] =
      (uint8_t) (servUUID >> 8) & 0xFF;
  BleApplicationContext.BleApplicationContext_legacy.advtServUUIDlen++;

  return;
}

static void Adv_Mgr( void )
{
  /**
   * The code shall be executed in the background as an aci command may be sent
   * The background is the only place where the application can make sure a new aci command
   * is not sent if there is a pending one
   */
  osThreadFlagsSet( AdvUpdateProcessId, 1 );

  return;
}

static void AdvUpdateProcess(void *argument)
{
  UNUSED(argument);

  for(;;)
  {
    osThreadFlagsWait( 1, osFlagsWaitAny, osWaitForever);
    Adv_Update( );
  }
}

static void Adv_Update( void )
{
  Adv_Request(APP_BLE_LP_ADV);

  return;
}

static void HciUserEvtProcess(void *argument)
{
  UNUSED(argument);

  for(;;)
  {
    osThreadFlagsWait( 1, osFlagsWaitAny, osWaitForever);
    hci_user_evt_proc( );
  }
}

/* USER CODE BEGIN FD_SPECIFIC_FUNCTIONS */

void APP_BLE_Peer_Bonded_Check(void)
{
  tBleStatus result = BLE_STATUS_SUCCESS;
  uint8_t Security_Mode, Security_Level;

  result = aci_gap_is_device_bonded(BleApplicationContext.Peer_Address_Type,BleApplicationContext.Peer_Address);
  if (result == BLE_STATUS_SUCCESS)
  {
    BleApplicationContext.Peer_Bonded = 0x01;
  }
  else 
  {
    BleApplicationContext.Peer_Bonded = 0x00;
  }
    
  result = aci_gap_get_security_level(BleApplicationContext.connection_handle,&Security_Mode,&Security_Level);
  BleApplicationContext.Security_Mode = Security_Mode;
  BleApplicationContext.Security_Level = Security_Level;
  if (result == BLE_STATUS_SUCCESS)
  {
  APP_DBG_MSG("Peer_Bonded=%d Security_Mode= %d, Security_Level= %d \n\r", BleApplicationContext.Peer_Bonded, Security_Mode, Security_Level);
  }

  return;	
}

void APP_BLE_Remove_Bonding_Info(void)
{
  APP_DBG_MSG(" aci_gap_clear_security_db & aci_gap_remove_bonded_device \n\r");
  aci_gap_remove_bonded_device(BleApplicationContext.Peer_Address_Type,BleApplicationContext.Peer_Address);
  aci_gap_clear_security_db();
  return;
}

void APP_BLE_Slave_Security_Request(void)
{
  APP_BLE_Peer_Bonded_Check();
  
  if( (BleApplicationContext.Peer_Bonded == 0x00) ||
      ( (BleApplicationContext.Peer_Bonded == 0x01) && (BleApplicationContext.Security_Mode == 0x01) && (BleApplicationContext.Security_Level == 0x01) )
    )
  {
    GapProcReq(GAP_PROC_SLAVE_SECURITY_REQ);
  }
  
  return;
}

static void GapProcReq(GapProcId_t GapProcId)
{
  tBleStatus status;

  switch(GapProcId)
  {
    case GAP_PROC_SLAVE_SECURITY_REQ:
    {
      BleApplicationContext.Security_Request = 0x01;
      status = aci_gap_slave_security_req(BleApplicationContext.connection_handle); 
      if (status != BLE_STATUS_SUCCESS)
      {
        APP_DBG_MSG("GAP_PROC_SLAVE_SECURITY_REQ aci_gap_slave_security_req  status=0x%02x \n\r",status);
      }
      
      //APP_DBG_MSG("GAP_PROC_SLAVE_SECURITY_REQ waiting for ACI_GAP_SLAVE_SECURITY_INITIATED_VSEVT_CODE \n\r");
      //gap_cmd_resp_wait(GAP_DEFAULT_TIMEOUT);/* waiting for ACI_GAP_SLAVE_SECURITY_INITIATED_VSEVT_CODE */
      //APP_DBG_MSG("GAP_PROC_SLAVE_SECURITY_REQ waited for ACI_GAP_SLAVE_SECURITY_INITIATED_VSEVT_CODE  \n\r");
      
      APP_DBG_MSG("waiting for  ACI_GAP_PAIRING_COMPLETE_VSEVT_CODE \n\r");
      gap_cmd_resp_wait(GAP_DEFAULT_TIMEOUT);/* waiting for ACI_GAP_PAIRING_COMPLETE_VSEVT_CODE */
      BleApplicationContext.Security_Request = 0x00;
      APP_DBG_MSG("waited for  ACI_GAP_PAIRING_COMPLETE_VSEVT_CODE \n\r");
    }
    break;

    case GAP_PROC_PASS_KEY_RESPONSE:
    {
      APP_DBG_MSG("GAP_PROC_PASS_KEY_RESPONSE \n\r");
      aci_gap_pass_key_resp(BleApplicationContext.connection_handle, CFG_FIXED_PIN);/* response for ACI_GAP_PASS_KEY_REQ_VSEVT_CODE */
    }
    break;

    case GAP_PROC_ALLOW_REBOND:
    {
      APP_DBG_MSG("GAP_PROC_ALLOW_REBOND aci_gap_allow_rebond(0x%04X)\n\r",BleApplicationContext.connection_handle);
      aci_gap_allow_rebond(BleApplicationContext.connection_handle);/* response for ACI_GAP_BOND_LOST_VSEVT_CODE */
    }
    break;

    case GAP_PROC_NUMERIC_COMPARISON_VALUE_CONFIRM:
    {
      aci_gap_numeric_comparison_value_confirm_yesno(BleApplicationContext.connection_handle, 1); /* CONFIRM_YES = 1 */
      
      APP_DBG_MSG("GAP_PROC_NUMERIC_COMPARISON_VALUE_CONFIRM ** aci_gap_numeric_comparison_value_confirm_yesno-->YES \n");
    }
    break;
		  
    case GAP_PROC_TERMINATE_CONNECTION:
    {
      APP_DBG_MSG("terminate connection \n");
      status = aci_gap_terminate(BleApplicationContext.connection_handle,0x13);
      if (status != BLE_STATUS_SUCCESS)
      {
        APP_DBG_MSG("Term Connection cmd failure: 0x%x\n", status);
      }
      gap_cmd_resp_wait(GAP_DEFAULT_TIMEOUT);
      
      APP_DBG_MSG("GAP_PROC_TERMINATE_CONNECTION complete event received\n");
    }
    break;
	  
    default:
      break;
  }
  return;
}

static void gap_cmd_resp_release(uint32_t flag)
{
  UNUSED(flag);
  osSemaphoreRelease( SemGapId );
  return;
}

static void gap_cmd_resp_wait(uint32_t timeout)
{
  UNUSED(timeout);
  osSemaphoreAcquire( SemGapId, osWaitForever );
  return;
}

/* USER CODE END FD_SPECIFIC_FUNCTIONS */
/*************************************************************
 *
 * WRAP FUNCTIONS
 *
 *************************************************************/
void hci_notify_asynch_evt(void* pdata)
{
  UNUSED(pdata);
  osThreadFlagsSet( HciUserEvtProcessId, 1 );
  return;
}

void hci_cmd_resp_release(uint32_t flag)
{
  UNUSED(flag);
  osSemaphoreRelease( SemHciId );
  return;
}

void hci_cmd_resp_wait(uint32_t timeout)
{
  UNUSED(timeout);
  osSemaphoreAcquire( SemHciId, osWaitForever );
  return;
}

static void BLE_UserEvtRx( void * pPayload )
{
  SVCCTL_UserEvtFlowStatus_t svctl_return_status;
  tHCI_UserEvtRxParam *pParam;

  pParam = (tHCI_UserEvtRxParam *)pPayload;

  svctl_return_status = SVCCTL_UserEvtRx((void *)&(pParam->pckt->evtserial));
  if (svctl_return_status != SVCCTL_UserEvtFlowDisable)
  {
    pParam->status = HCI_TL_UserEventFlow_Enable;
  }
  else
  {
    pParam->status = HCI_TL_UserEventFlow_Disable;
  }
}

static void BLE_StatusNot( HCI_TL_CmdStatus_t status )
{
  switch (status)
  {
    case HCI_TL_CmdBusy:
      osMutexAcquire( MtxHciId, osWaitForever );
      break;

    case HCI_TL_CmdAvailable:
      osMutexRelease( MtxHciId );
      break;

    default:
      break;
  }
  return;
}

void SVCCTL_ResumeUserEventFlow( void )
{
  hci_resume_flow();
  return;
}

/* USER CODE BEGIN FD_WRAP_FUNCTIONS */

/* USER CODE END FD_WRAP_FUNCTIONS */
