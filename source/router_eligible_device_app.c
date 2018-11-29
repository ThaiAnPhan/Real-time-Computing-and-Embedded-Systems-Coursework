/*==================================================================================================
Include Files
==================================================================================================*/

#include <string.h>
#include "EmbeddedTypes.h"

/* FSL Framework */
#include "shell.h"
#include "Keyboard.h"
#include "RNG_Interface.h"

/* Network */
#include "ip_if_management.h"
#include "event_manager.h"

/* Application */
#include "router_eligible_device_app.h"
#include "shell_ip.h"
#include "thread_utils.h"
#include "thread_meshcop.h"
#include "thread_network.h"
#include "thread_app_callbacks.h"
#include "app_init.h"
//#include "app_stack_config.h"
#include "app_thread_config.h"
#include "app_led.h"
#include "coap.h"
#include "app_socket_utils.h"
#if THR_ENABLE_EVENT_MONITORING
#include "app_event_monitoring.h"
#endif
#if THR_ENABLE_MGMT_DIAGNOSTICS
#include "thread_mgmt.h"
#include "thci.h"
#endif

#define APP_DEFAULT_DEST_ADDR                   in6addr_realmlocal_allthreadnodes

/*==================================================================================================
Private macros
==================================================================================================*/
#ifndef APP_MSG_QUEUE_SIZE
    #define APP_MSG_QUEUE_SIZE                  20
#endif

#if (THREAD_USE_SHELL == FALSE)
    #define shell_write(a)
    #define shell_refresh()
    #define shell_printf(a,...)
#endif

#define gThrDefaultInstanceId_c                 0
#if APP_AUTOSTART
#define gAppFactoryResetTimeoutMin_c            10000
#define gAppFactoryResetTimeoutMax_c            20000
#endif
#define gAppRestoreLeaderLedTimeout_c           60     /* seconds */

#define gAppJoinTimeout_c                       800    /* miliseconds */

#define APP_LED_URI_PATH                        "/led"
#define APP_TEXT_URI_PATH						"/text"
#define APP_ADDR_URI_PATH						"/addr"

#if LARGE_NETWORK
#define APP_RESET_TO_FACTORY_URI_PATH           "/reset"
#endif

#define APP_DEFAULT_DEST_ADDR                   in6addr_realmlocal_allthreadnodes

/*==================================================================================================
Private global variables declarations
==================================================================================================*/
static instanceId_t mThrInstanceId = gInvalidInstanceId_c;    /*!< Thread Instance ID */

static bool_t mFirstPushButtonPressed = FALSE;

static bool_t mJoiningIsAppInitiated = FALSE;

/*==================================================================================================
Private prototypes
==================================================================================================*/
static void App_HandleKeyboard(void *param);
static void App_UpdateStateLeds(appDeviceState_t deviceState);
static void APP_JoinEventsHandler(thrEvCode_t evCode);
static void APP_InitCoapDemo(void);
#if gKBD_KeysCount_c > 1
static void APP_SendLedRgbOn(void *pParam);
static void APP_SendLedRgbOff(void *pParam);
static void APP_SendLedFlash(void *pParam);
static void APP_SendLedColorWheel(void *pParam);
static void APP_SendIPv6Addr(void *pParam);


#endif
static void APP_ProcessLedCmd(uint8_t *pCommand, uint8_t dataLen);
static void APP_CoapGenericCallback(coapSessionStatus_t sessionStatus, void *pData, coapSession_t *pSession, uint32_t dataLen);
static void APP_CoapLedCb(coapSessionStatus_t sessionStatus, void *pData, coapSession_t *pSession, uint32_t dataLen);
static void APP_CoapTextCb(coapSessionStatus_t sessionStatus, void *pData, coapSession_t *pSession, uint32_t dataLen);
static void APP_CoapAddrCb(coapSessionStatus_t sessionStatus, void *pData, coapSession_t *pSession, uint32_t dataLen);

static void App_RestoreLeaderLed(void *param);
#if LARGE_NETWORK
static void APP_CoapResetToFactoryDefaultsCb(coapSessionStatus_t sessionStatus, void *pData, coapSession_t *pSession, uint32_t dataLen);
static void APP_SendResetToFactoryCommand(void *param);
#endif
#if APP_AUTOSTART
static void APP_AutoStart(void *param);
static void APP_AutoStartCb(void *param);
#endif

/*==================================================================================================
Public global variables declarations
==================================================================================================*/
const coapUriPath_t gAPP_LED_URI_PATH  = {SizeOfString(APP_LED_URI_PATH), (uint8_t *)APP_LED_URI_PATH};
const coapUriPath_t gAPP_TEXT_URI_PATH  = {SizeOfString(APP_TEXT_URI_PATH), (uint8_t *)APP_TEXT_URI_PATH};
const coapUriPath_t gAPP_ADDR_URI_PATH  = {SizeOfString(APP_ADDR_URI_PATH), (uint8_t *)APP_ADDR_URI_PATH};

#if LARGE_NETWORK
const coapUriPath_t gAPP_RESET_URI_PATH = {SizeOfString(APP_RESET_TO_FACTORY_URI_PATH), (uint8_t *)APP_RESET_TO_FACTORY_URI_PATH};
#endif

/* Application state/mode */
appDeviceState_t gAppDeviceState[THR_MAX_INSTANCES];
appDeviceMode_t gAppDeviceMode[THR_MAX_INSTANCES];

/* Flag used to stop the attaching retries */
bool_t gbRetryInterrupt = TRUE;

bool_t gbCreateNetwork = FALSE;

/* CoAP instance */
uint8_t mAppCoapInstId = THR_ALL_FFs8;
/* Destination address for CoAP commands */
ipAddr_t gCoapDestAddress;

/* Application timer Id */
tmrTimerID_t mAppTimerId = gTmrInvalidTimerID_c;

#if APP_AUTOSTART
tmrTimerID_t tmrStartApp = gTmrInvalidTimerID_c;
#endif

uint32_t leaderLedTimestamp = 0;

/* Pointer application task message queue */
taskMsgQueue_t *mpAppThreadMsgQueue = NULL;

extern bool_t gEnable802154TxLed;

/*==================================================================================================
Public functions
==================================================================================================*/
/*!*************************************************************************************************
\fn     void APP_Init(void)
\brief  This function is used to initialize application.
***************************************************************************************************/

void App_Init(void)
{
    /* Initialize pointer to application task message queue */
    mpAppThreadMsgQueue = &appThreadMsgQueue;

    /* Initialize main thread message queue */
    ListInit(&appThreadMsgQueue.msgQueue,APP_MSG_QUEUE_SIZE);

    /* Set default device mode/state */
    APP_SetState(gThrDefaultInstanceId_c, gDeviceState_FactoryDefault_c);
    APP_SetMode(gThrDefaultInstanceId_c, gDeviceMode_Configuration_c);

    /* Initialize keyboard handler */
    pfAppKeyboardHandler = App_HandleKeyboard;

    /* Use one instance ID for application */
    mThrInstanceId = gThrDefaultInstanceId_c;

#if THR_ENABLE_EVENT_MONITORING
    /* Initialize event monitoring */
    APP_InitEventMonitor(mThrInstanceId);
#endif

    if(gThrStatus_Success_c == THR_StartInstance(mThrInstanceId, pStackCfg[0]))
    {
        /* Initialize CoAP demo */
        APP_InitCoapDemo();
    }

}

/*!*************************************************************************************************
\fn     void App_EventHandler(void)
\brief  Application Handler. In this configuration is called on the task with the lowest priority
***************************************************************************************************/
void App_EventHandler(void)
{
    bool_t handleMsg = TRUE;

    while(handleMsg == TRUE)
    {
        handleMsg = NWKU_MsgHandler(&appThreadMsgQueue);
    }
}
/*!*************************************************************************************************
\private
\fn     static void APP_ThrNwkJoin(void *param)
\brief  Start the joining procedure.

\param  [in]    param    Not used
***************************************************************************************************/
static void APP_ThrNwkJoin
(
    void *param
)
{
    if(THR_NwkJoin(mThrInstanceId, THR_APP_JOIN_DISCOVERY_METHOD) != gThrStatus_Success_c)
    {
        /* User can treat join failure according to their application */
    }
}

/*!*************************************************************************************************
\private
\fn     static void App_JoinTimerCallback(void *param)
\brief  Join timer callback.

\param  [in]    param    Not used
***************************************************************************************************/
static void App_JoinTimerCallback
(
    void *param
)
{
    if(mFirstPushButtonPressed)
    {
        mJoiningIsAppInitiated = TRUE;
        TMR_FreeTimer(mAppTimerId);
        mAppTimerId = gTmrInvalidTimerID_c;
        (void)NWKU_SendMsg(APP_ThrNwkJoin, NULL, mpAppThreadMsgQueue);
    }
}


/*!*************************************************************************************************
\private
\fn     static void App_JoinNetwork(uint32_t keyEvent)
\brief  This is a handler for  KBD module - short press events. Device is in configuration mode.

\param  [in]    keyEvent    The keyboard module event
***************************************************************************************************/
static void App_JoinNetwork(uint32_t keyEvent)
{
	 (void)keyEvent;

	    if((APP_GetState(mThrInstanceId) == gDeviceState_FactoryDefault_c) ||
	       (APP_GetState(mThrInstanceId) == gDeviceState_NwkFailure_c) ||
	       (APP_GetState(mThrInstanceId) == gDeviceState_NwkOperationPending_c))
	    {
	        App_UpdateStateLeds(gDeviceState_JoiningOrAttaching_c);
	        mFirstPushButtonPressed = TRUE;

	        if(mAppTimerId == gTmrInvalidTimerID_c)
	        {
	            mAppTimerId = TMR_AllocateTimer();
	        }
	        //shell_printf("DMMM1\n");
	        /* Validate application timer Id */
	        if(mAppTimerId != gTmrInvalidTimerID_c)
	        {
		        //shell_printf("DMMM2\n");

	            /* Start the application timer. Wait gAppJoinTimeout_c
	               to start the joining procedure */
	            TMR_StartSingleShotTimer(mAppTimerId, gAppJoinTimeout_c, App_JoinTimerCallback, NULL);
	            //APP_SendIPv6Addr(NULL);

	        }
	        else
	        {
	            mJoiningIsAppInitiated = TRUE;
	            /* No timer available - try to join the network */
	            if(THR_NwkJoin(mThrInstanceId, THR_APP_JOIN_DISCOVERY_METHOD) != gThrStatus_Success_c)
	            {
	                /* User can treat join failure according to their application */
	            }
	        }
	    }

}


/*!*************************************************************************************************
\private
\fn     static void APP_ConfigModeHandleKeyboard(uint32_t keyEvent)
\brief  This is a handler for KBD module events. Device is in configuration mode.

\param  [in]    keyEvent   The keyboard module event
***************************************************************************************************/
static void APP_ConfigModeHandleKeyboard
(
    uint32_t keyEvent
)
{
    switch(keyEvent)
    {
        case gKBD_EventPB1_c:
#if gKBD_KeysCount_c > 1
        case gKBD_EventPB2_c:
        case gKBD_EventPB3_c:
        	App_JoinNetwork(keyEvent);
        	break;
        case gKBD_EventPB4_c:
#endif
        	App_JoinNetwork(keyEvent);
            break;
        case gKBD_EventLongPB1_c:
#if gKBD_KeysCount_c > 1
        case gKBD_EventLongPB2_c:
        case gKBD_EventLongPB3_c:
        case gKBD_EventLongPB4_c:
#endif
            break;
        case gKBD_EventVeryLongPB1_c:
#if gKBD_KeysCount_c > 1
        case gKBD_EventVeryLongPB2_c:
        case gKBD_EventVeryLongPB3_c:
        case gKBD_EventVeryLongPB4_c:
#endif
            /* Factory reset */
            THR_FactoryReset();
            break;
        default:
            break;
    }
}



/*!*************************************************************************************************
\private
\fn     static void APP_AppModeHandleKeyboard(uint32_t keyEvent)
\brief  This is a handler for KBD module events. Device is in application mode.

\param  [in]    keyEvent   The keyboard module event
***************************************************************************************************/
static void APP_AppModeHandleKeyboard
(
    uint32_t keyEvent
)
{
    switch(keyEvent)
    {
        case gKBD_EventPB1_c:
            break;
#if gKBD_KeysCount_c > 1
        case gKBD_EventPB2_c:
            /* Remote led RGB - on */
            (void)NWKU_SendMsg(APP_SendLedRgbOn, NULL, mpAppThreadMsgQueue);
            break;
        case gKBD_EventPB3_c:
            break;
        case gKBD_EventPB4_c:
            /* Remote led RGB - off */
            (void)NWKU_SendMsg(APP_SendLedRgbOff, NULL, mpAppThreadMsgQueue);
            break;
#endif
        case gKBD_EventLongPB1_c:
            break;
#if gKBD_KeysCount_c > 1
        case gKBD_EventLongPB2_c:
            break;
        case gKBD_EventLongPB3_c:
            /* Remote led flash */
            (void)NWKU_SendMsg(APP_SendLedFlash, NULL, mpAppThreadMsgQueue);
            break;
        case gKBD_EventLongPB4_c:
            /* Remote led - color wheel*/
            (void)NWKU_SendMsg(APP_SendLedColorWheel, NULL, mpAppThreadMsgQueue);
            break;
#endif
        case gKBD_EventVeryLongPB1_c:
#if gKBD_KeysCount_c > 1
        case gKBD_EventVeryLongPB4_c:
#if LARGE_NETWORK
            /* OTA factory reset */
            (void)NWKU_SendMsg(APP_SendResetToFactoryCommand, NULL, mpAppThreadMsgQueue);
            break;
#endif
        case gKBD_EventVeryLongPB3_c:
        case gKBD_EventVeryLongPB2_c:
#endif
            /* Factory reset */
            THR_FactoryReset();
            break;
        default:
            break;
   }
}


/*!*************************************************************************************************
\fn     void APP_NwkScanHandler(void *param)
\brief  This function is used to handle network scan results in asynchronous mode.

\param  [in]    param    Pointer to stack event
***************************************************************************************************/
void APP_NwkScanHandler
(
    void *param
)
{
    thrEvmParams_t *pEventParams = (thrEvmParams_t *)param;
    thrNwkScanResults_t *pScanResults = &pEventParams->pEventData->nwkScanCnf;

    /* Handle the network scan result here */
    if(pScanResults)
    {
#if THREAD_USE_SHELL
        SHELL_NwkScanPrint(pScanResults);
#endif
        MEM_BufferFree(pScanResults);
    }
    /* Free Event Buffer */
    MEM_BufferFree(pEventParams);
}


/*!*************************************************************************************************
\private
\fn     static void App_HandleKeyboard(void *param)
\brief  This is a handler for KBD module events.

\param  [in]    param    The keyboard module event

***************************************************************************************************/
static void App_HandleKeyboard
(
    void *param
)
{
    uint32_t events = (uint32_t)(param);

    if(APP_GetMode(mThrInstanceId) == gDeviceMode_Configuration_c)
    {
        /* Device is in configuration mode */
        APP_ConfigModeHandleKeyboard(events);
    }
    else
    {
        /* Device is in application mode */
        APP_AppModeHandleKeyboard(events);
    }
}

/*!*************************************************************************************************
\fn     void Stack_to_APP_Handler(void *param)
\brief  This function is used to handle stack events in asynchronous mode.

\param  [in]    param    Pointer to stack event
***************************************************************************************************/
void Stack_to_APP_Handler
(
    void *param
)
{
    thrEvmParams_t *pEventParams = (thrEvmParams_t *)param;

    switch(pEventParams->code)
    {
        case gThrEv_GeneralInd_ResetToFactoryDefault_c:
            App_UpdateStateLeds(gDeviceState_FactoryDefault_c);
            break;

        case gThrEv_GeneralInd_InstanceRestoreStarted_c:
        case gThrEv_GeneralInd_ConnectingStarted_c:
            APP_SetMode(mThrInstanceId, gDeviceMode_Configuration_c);
            App_UpdateStateLeds(gDeviceState_JoiningOrAttaching_c);
            gEnable802154TxLed = FALSE;
            break;

        case gThrEv_NwkJoinCnf_Success_c:
        case gThrEv_NwkJoinCnf_Failed_c:
            APP_JoinEventsHandler(pEventParams->code);
            break;

        case gThrEv_GeneralInd_Connected_c:
            App_UpdateStateLeds(gDeviceState_NwkConnected_c);
            /* Set application CoAP destination to all nodes on connected network */
            gCoapDestAddress = APP_DEFAULT_DEST_ADDR;
            APP_SetMode(mThrInstanceId, gDeviceMode_Application_c);
            mFirstPushButtonPressed  = FALSE;
            /* Synchronize server data */
            THR_BrPrefixAttrSync(mThrInstanceId);
            /* Enable LED for 802.15.4 tx activity */
            gEnable802154TxLed = TRUE;
            //APP_SendIPv6Addr(NULL);
            (void)NWKU_SendMsg(APP_SendIPv6Addr, NULL, mpAppThreadMsgQueue);
            break;

        case gThrEv_GeneralInd_RequestRouterId_c:
            gEnable802154TxLed = FALSE;
            break;

        case gThrEv_GeneralInd_ConnectingDeffered_c:
            APP_SetMode(mThrInstanceId, gDeviceMode_Configuration_c);
            gEnable802154TxLed = FALSE;
            App_UpdateStateLeds(gDeviceState_NwkOperationPending_c);
            break;

        case gThrEv_GeneralInd_ConnectingFailed_c:
        case gThrEv_GeneralInd_Disconnected_c:
            APP_SetMode(mThrInstanceId, gDeviceMode_Configuration_c);
            App_UpdateStateLeds(gDeviceState_NwkFailure_c);
            break;

        case gThrEv_GeneralInd_DeviceIsLeader_c:
            App_UpdateStateLeds(gDeviceState_Leader_c);
            gEnable802154TxLed = TRUE;
#if !LARGE_NETWORK
            /* Auto start commissioner for the partition for demo purposes */
            MESHCOP_StartCommissioner(pEventParams->thrInstId);
#endif
            break;

        case gThrEv_GeneralInd_DeviceIsRouter_c:
            App_UpdateStateLeds(gDeviceState_ActiveRouter_c);
            gEnable802154TxLed = TRUE;

#if UDP_ECHO_PROTOCOL
            ECHO_ProtocolInit(mpAppThreadMsgQueue);
#endif
            break;

        case gThrEv_GeneralInd_DevIsREED_c:
            App_UpdateStateLeds(gDeviceState_NwkConnected_c);
            gEnable802154TxLed = TRUE;
            break;

#if gLpmIncluded_d
        case gThrEv_GeneralInd_AllowDeviceToSleep_c:
            PWR_AllowDeviceToSleep();
            break;

        case gThrEv_GeneralInd_DisallowDeviceToSleep_c:
            PWR_DisallowDeviceToSleep();
            break;
#endif
        default:
            break;
    }

    /* Free event buffer */
    MEM_BufferFree(pEventParams->pEventData);
    MEM_BufferFree(pEventParams);
}

/*!*************************************************************************************************
\fn     void APP_Commissioning_Handler(void *param)
\brief  This function is used to handle Commissioning events in synchronous mode.
\param  [in]    param    Pointer to Commissioning event
***************************************************************************************************/
void APP_Commissioning_Handler
(
    void *param
)
{
    thrEvmParams_t *pEventParams = (thrEvmParams_t *)param;

    switch(pEventParams->code)
    {
        /* Joiner Events */
        case gThrEv_MeshCop_JoinerDiscoveryStarted_c:
            break;
        case gThrEv_MeshCop_JoinerDiscoveryFailed_c:
            break;
        case gThrEv_MeshCop_JoinerDiscoveryFailedFiltered_c:
            break;
        case gThrEv_MeshCop_JoinerDiscoverySuccess_c:
            break;
        case gThrEv_MeshCop_JoinerDtlsSessionStarted_c:
            App_UpdateStateLeds(gDeviceState_JoiningOrAttaching_c);
            break;
        case gThrEv_MeshCop_JoinerDtlsError_c:
        case gThrEv_MeshCop_JoinerError_c:
            App_UpdateStateLeds(gDeviceState_FactoryDefault_c);
            break;
        case gThrEv_MeshCop_JoinerAccepted_c:
            break;

        /* Commissioner Events(event set applies for all Commissioners: on-mesh, external, native) */
        case gThrEv_MeshCop_CommissionerPetitionStarted_c:
            break;
        case gThrEv_MeshCop_CommissionerPetitionAccepted_c:
        {
            uint8_t aDefaultEui[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            thrOctet32_t defaultPskD = THR_PSK_D;

            MESHCOP_AddExpectedJoiner(mThrInstanceId, aDefaultEui, defaultPskD.aStr, defaultPskD.length, TRUE);
            MESHCOP_SyncSteeringData(mThrInstanceId, gMeshcopEuiMaskAllFFs_c);
            break;
        }
        case gThrEv_MeshCop_CommissionerPetitionRejected_c:
            break;
        case gThrEv_MeshCop_CommissionerPetitionError_c:
            break;
        case gThrEv_MeshCop_CommissionerKeepAliveSent_c:
            break;
        case gThrEv_MeshCop_CommissionerError_c:
            break;
        case gThrEv_MeshCop_CommissionerJoinerDtlsSessionStarted_c:
            break;
        case gThrEv_MeshCop_CommissionerJoinerDtlsError_c:
            break;
        case gThrEv_MeshCop_CommissionerJoinerAccepted_c:
            break;
        case gThrEv_MeshCop_CommissionerNwkDataSynced_c:
            break;
    }

    /* Free event buffer */
    MEM_BufferFree(pEventParams);
}

/*!*************************************************************************************************
\fn     void App_RestoreLeaderLedCb(void *param)
\brief  Called in Application state to restore leader LED.

\param  [in]    param    Not used
***************************************************************************************************/
void App_RestoreLeaderLedCb
(
    void *param
)
{
    (void)NWKU_SendMsg(App_RestoreLeaderLed, NULL, mpAppThreadMsgQueue);
}

/*==================================================================================================
Private functions
==================================================================================================*/
/*!*************************************************************************************************
\private
\fn     static void APP_InitCoapDemo(void)
\brief  Initialize CoAP demo.
***************************************************************************************************/
static void APP_InitCoapDemo
(
    void
)
{
    coapRegCbParams_t cbParams[] =  {{APP_CoapLedCb,  (coapUriPath_t *)&gAPP_LED_URI_PATH},
    								{APP_CoapTextCb, (coapUriPath_t *)&gAPP_TEXT_URI_PATH},
    								 {APP_CoapAddrCb, (coapUriPath_t *)&gAPP_ADDR_URI_PATH}};
    /* Register Services in COAP by creating a COAP instance*/
    coapStartUnsecParams_t coapParams = {COAP_DEFAULT_PORT, AF_INET6};
    mAppCoapInstId = COAP_CreateInstance(NULL, &coapParams, gIpIfSlp0_c, (coapRegCbParams_t *)cbParams,
                                         NumberOfElements(cbParams));
}


/*!*************************************************************************************************
\private
\fn     static void App_UpdateLedState(appDeviceState_t deviceState)
\brief  Called when Application state and LEDs must be updated.

\param  [in]    deviceState    The current device state
***************************************************************************************************/
static void App_UpdateStateLeds
(
    appDeviceState_t deviceState
)
{
    /* If the user presses a button different than the LED off button, reset timestamp */
    if((gpaThrAttr[mThrInstanceId]->devRole == gThrDevRole_Leader_c) &&
       (APP_GetState(mThrInstanceId) != gDeviceState_AppLedOff_c) &&
       (leaderLedTimestamp != 0))
    {
        leaderLedTimestamp = 0;
    }

    APP_SetState(mThrInstanceId, deviceState);
    Led_SetState(APP_GetMode(mThrInstanceId), APP_GetState(mThrInstanceId));
}


/*!*************************************************************************************************
\private
\fn     static void APP_JoinEventsHandler(thrEvCode_t evCode)
\brief  This function is used to the handle join failed event.

\param  [in]    evCode    Event code
***************************************************************************************************/
static void APP_JoinEventsHandler
(
    thrEvCode_t evCode
)
{
    if(mJoiningIsAppInitiated)
    {
        if(evCode == gThrEv_NwkJoinCnf_Failed_c)
        {
            if(gbRetryInterrupt && !gbCreateNetwork)
            {
                mJoiningIsAppInitiated = TRUE;

                /* Retry to join the network */
                if(THR_NwkJoin(mThrInstanceId, THR_APP_JOIN_DISCOVERY_METHOD) != gThrStatus_Success_c)
                {
                    /* User can treat join failure according to their application */
                	shell_printf("DMMMMM\n");
                }
                return;
            }
            else if(gbCreateNetwork)
            {
                /* Create the network */
                (void)THR_NwkCreate(mThrInstanceId);
            }
            mJoiningIsAppInitiated = FALSE;
        }
        else if(evCode == gThrEv_NwkJoinCnf_Success_c)
        {
            mJoiningIsAppInitiated = FALSE;
        }
    }
}


/*==================================================================================================
  Coap Demo functions:
==================================================================================================*/
/*!*************************************************************************************************
\private
\fn     static void APP_CoapGenericCallback(sessionStatus sessionStatus, void *pData,
                                            coapSession_t *pSession, uint32_t dataLen)
\brief  This function is the generic callback function for CoAP message.

\param  [in]    sessionStatus   Status for CoAP session
\param  [in]    pData           Pointer to CoAP message payload
\param  [in]    pSession        Pointer to CoAP session
\param  [in]    dataLen         Length of CoAP payload
***************************************************************************************************/
static void APP_CoapGenericCallback
(
    coapSessionStatus_t sessionStatus,
    void *pData,
    coapSession_t *pSession,
    uint32_t dataLen
)
{
	shell_printf("Hey what's up I'm in this shit \n");

    /* If no ACK was received, try again */
    if(sessionStatus == gCoapFailure_c)
    {

    	/*
        if(FLib_MemCmp(pSession->pUriPath->pUriPath, (coapUriPath_t *)&gAPP_LED_URI_PATH.pUriPath,
                       pSession->pUriPath->length))
        {
            //(void)NWKU_SendMsg(APP_ProcessLedCmd(pCommand, dataLen), NULL, mpAppThreadMsgQueue);
        }
        */
    }
}

/*!*************************************************************************************************
\private
\fn     static void APP_SendLedCommand(uint8_t *pCommand, uint8_t dataLen)
\brief  This function is used to send a Led command to gCoapDestAddress.

\param  [in]    pCommand   Pointer to command data
\param  [in]    dataLen    Data length
***************************************************************************************************/
static void APP_SendLedCommand
(
    uint8_t *pCommand,
    uint8_t dataLen
)
{
    ifHandle_t ifHandle = THR_GetIpIfPtrByInstId(mThrInstanceId);

    if(!IP_IF_IsMyAddr(ifHandle->ifUniqueId, &gCoapDestAddress))
    {
    	// When having data to send on the instance, open a session to the remote destination address
        coapSession_t *pSession = COAP_OpenSession(mAppCoapInstId);

        if(pSession)
        {
            coapMsgTypesAndCodes_t coapMessageType = gCoapMsgTypeNonPost_c;

            pSession->pCallback = NULL;
            FLib_MemCpy(&pSession->remoteAddr, &gCoapDestAddress, sizeof(ipAddr_t));
            COAP_SetUriPath(pSession,(coapUriPath_t *)&gAPP_LED_URI_PATH);

            if(!IP6_IsMulticastAddr(&gCoapDestAddress))
            {
                coapMessageType = gCoapMsgTypeConPost_c;
                pSession->pCallback = APP_CoapGenericCallback;
            }
            else
            {
                APP_ProcessLedCmd(pCommand, dataLen);
            }
            COAP_Send(pSession, coapMessageType, pCommand, dataLen);
        }
    }
    else
    {
        APP_ProcessLedCmd(pCommand, dataLen);
    }
}

/*!*************************************************************************************************
\private
\fn     static void APP_SendLedCommand(uint8_t *pCommand, uint8_t dataLen)
\brief  This function is used to send a Led command to gCoapDestAddress.

\param  [in]    pCommand   Pointer to command data
\param  [in]    dataLen    Data length
***************************************************************************************************/
static void APP_SendIPv6Addr
(
	void *pParam
)
{
    ifHandle_t ifHandle = THR_GetIpIfPtrByInstId(mThrInstanceId);
    if(!IP_IF_IsMyAddr(ifHandle->ifUniqueId, &gCoapDestAddress))
    {
    	// When having data to send on the instance, open a session to the remote destination address
        coapSession_t *pSession = COAP_OpenSession(mAppCoapInstId);

        if(pSession)
        {
            coapMsgTypesAndCodes_t coapMessageType = gCoapMsgTypeNonPost_c;

            pSession->pCallback = APP_CoapGenericCallback;
            FLib_MemCpy(&pSession->remoteAddr, &gCoapDestAddress, sizeof(ipAddr_t));
            COAP_SetUriPath(pSession,(coapUriPath_t *)&gAPP_ADDR_URI_PATH);
            /*
            if(!IP6_IsMulticastAddr(&gCoapDestAddress))
            {
                coapMessageType = gCoapMsgTypeConPost_c;
                pSession->pCallback = APP_CoapGenericCallback;
            }
            else
            {
                //APP_ProcessLedCmd(pCommand, dataLen);
            }
            */
            COAP_Send(pSession, coapMessageType, NULL, 0);
            shell_printf("SENDADDR");

        }
    }
    else
    {
        //APP_ProcessLedCmd(pCommand, dataLen);
    }
}


/*!*************************************************************************************************
\private
\fn     static void APP_SendLedRgbOn(void *pParam)
\brief  This function is used to send a Led RGB On command over the air.

\param  [in]    pParam    Not used
***************************************************************************************************/
static void APP_SendLedRgbOn
(
    void *pParam
)
{
    uint8_t aCommand[] = {"rgb r000 g000 b000"};
    uint8_t redValue, greenValue, blueValue;

    /* Red value on: 0x01 - 0xFF */
    redValue = (uint8_t)NWKU_GetRandomNoFromInterval(0x01, THR_ALL_FFs8);

    /* Green value on: 0x01 - 0xFF */
    greenValue = (uint8_t)NWKU_GetRandomNoFromInterval(0x01, THR_ALL_FFs8);

    /* Blue value on: 0x01 - 0xFF */
    blueValue = (uint8_t)NWKU_GetRandomNoFromInterval(0x01, THR_ALL_FFs8);

    NWKU_PrintDec(redValue, aCommand + 5, 3, TRUE);     //aCommand + strlen("rgb r")
    NWKU_PrintDec(greenValue, aCommand + 10, 3, TRUE);  //aCommand + strlen("rgb r000 g")
    NWKU_PrintDec(blueValue, aCommand + 15, 3, TRUE);   //aCommand + strlen("rgb r000 g000 b")

    APP_SendLedCommand(aCommand, sizeof(aCommand));
}



/*!*************************************************************************************************
\private
\fn     static void APP_SendLedRgbOff(void *pParam)
\brief  This function is used to send a Led RGB Off command over the air.

\param  [in]    pParam    Not used
***************************************************************************************************/
static void APP_SendLedRgbOff
(
    void *pParam
)
{
    uint8_t aCommand[] = {"rgb r000 g000 b000"};

    APP_SendLedCommand(aCommand, sizeof(aCommand));
}

/*!*************************************************************************************************
\private
\fn     static void APP_SendLedFlash(void *pParam)
\brief  This function is used to send a Led flash command over the air.

\param  [in]    pParam    Not used
***************************************************************************************************/
static void APP_SendLedFlash
(
    void *pParam
)
{
    uint8_t aCommand[] = {"flash"};

    APP_SendLedCommand(aCommand, sizeof(aCommand));
}

/*!*************************************************************************************************
\private
\fn     static void APP_SendLedColorWheel(void *pParam)
\brief  This function is used to send a Led color wheel command over the air.

\param  [in]    pParam    Not used
***************************************************************************************************/
static void APP_SendLedColorWheel
(
    void *pParam
)
{
    uint8_t aCommand[] = {"color wheel"};

    APP_SendLedCommand(aCommand, sizeof(aCommand));
}

/*!*************************************************************************************************
\private
\fn     static void APP_CoapLedCb(sessionStatus sessionStatus, void *pData,
                                  coapSession_t *pSession, uint32_t dataLen)
\brief  This function is the callback function for CoAP LED message.
\brief  It performs the required operations and sends back a CoAP ACK message.

\param  [in]    sessionStatus   Status for CoAP session
\param  [in]    pData           Pointer to CoAP message payload
\param  [in]    pSession        Pointer to CoAP session
\param  [in]    dataLen         Length of CoAP payload
***************************************************************************************************/
static void APP_CoapLedCb
(
    coapSessionStatus_t sessionStatus,
    void *pData,
    coapSession_t *pSession,
    uint32_t dataLen
)
{
    /* Process the command only if it is a POST method */
    if((pData) && (sessionStatus == gCoapSuccess_c) && (pSession->code == gCoapPOST_c))
    {
        APP_ProcessLedCmd(pData, dataLen);
    }

    /* Send the reply if the status is Success or Duplicate */
    if((gCoapFailure_c != sessionStatus) && (gCoapConfirmable_c == pSession->msgType))
    {
        /* Send CoAP ACK */
        COAP_Send(pSession, gCoapMsgTypeAckSuccessChanged_c, NULL, 0);
    }

}


static void APP_CoapTextCb
(
    coapSessionStatus_t sessionStatus,
    void *pData,
    coapSession_t *pSession,
    uint32_t dataLen
)
{
    /* Process the command only if it is a POST method */
    if((pData) && (sessionStatus == gCoapSuccess_c) && (pSession->code == gCoapPOST_c))
    {
        char addrStr[INET6_ADDRSTRLEN];
        char myText[dataLen];
        myText[dataLen] = '\0';
        FLib_MemCpy(myText,pData,dataLen);
        ntop(AF_INET6, &pSession->remoteAddr, addrStr, INET6_ADDRSTRLEN);
        shell_write("\r");
        shell_printf(myText);
        shell_printf("\tFrom IPv6 Address: %s\n\r", addrStr);
        shell_refresh();
    }

    /* Send the reply if the status is Success or Duplicate */
    if((gCoapFailure_c != sessionStatus) && (gCoapConfirmable_c == pSession->msgType))
    {
        /* Send CoAP ACK */
        COAP_Send(pSession, gCoapMsgTypeAckSuccessChanged_c, NULL, 0);
    }
    //tmrTimerID_t myTimerID = TMR_AllocateTimer();;
    //TMR_StartIntervalTimer(myTimerID, TmrMilliseconds(1000), (pfTmrCallBack_t)APP_SendLedRgbOn, NULL);

}

static void APP_CoapAddrCb
(
    coapSessionStatus_t sessionStatus,
    void *pData,
    coapSession_t *pSession,
    uint32_t dataLen
)
{

    /* Process the command if the sessionStatus is "success" */
    if(sessionStatus == gCoapSuccess_c)
    {
        char addrStr[INET6_ADDRSTRLEN];
        ntop(AF_INET6, &pSession->remoteAddr, addrStr, INET6_ADDRSTRLEN);
        shell_write("\r");
        shell_printf("1 new device connected! ");
        shell_printf("\tFrom IPv6 Address: %s\n\r", addrStr);
        shell_refresh();

    }

    /* Send the reply if the status is Success or Duplicate */
    if((gCoapFailure_c != sessionStatus) && (gCoapConfirmable_c == pSession->msgType))
    {
        /* Send CoAP ACK */
        COAP_Send(pSession, gCoapMsgTypeAckSuccessChanged_c, NULL, 0);
    }

}

/*!*************************************************************************************************
\private
\fn     static void APP_ProcessLedCmd(uint8_t *pCommand, uint8_t dataLen)
\brief  This function is used to process a LED command (on, off, flash, toggle, rgb, color wheel).

\param  [in]    pCommand   Pointer to command data
\param  [in]    dataLen    Data length
***************************************************************************************************/
static void APP_ProcessLedCmd
(
    uint8_t *pCommand,
    uint8_t dataLen
)
{

    /* Set mode state */
    APP_SetMode(mThrInstanceId, gDeviceMode_Application_c);
    mFirstPushButtonPressed  = FALSE;

    /* Process command */
    if(FLib_MemCmp(pCommand, "on",2))
    {
        App_UpdateStateLeds(gDeviceState_AppLedOn_c);
    }
    else if(FLib_MemCmp(pCommand, "off",3))
    {
        App_UpdateStateLeds(gDeviceState_AppLedOff_c);
    }
    else if(FLib_MemCmp(pCommand, "toggle",6))
    {
        App_UpdateStateLeds(gDeviceState_AppLedToggle_c);
    }
    else if(FLib_MemCmp(pCommand, "flash",5))
    {
        App_UpdateStateLeds(gDeviceState_AppLedFlash_c);
    }
    else if(FLib_MemCmp(pCommand, "rgb",3))
    {
        char* p = (char *)pCommand + strlen("rgb");
        uint8_t redValue = 0, greenValue = 0, blueValue = 0;
        appDeviceState_t appState = gDeviceState_AppLedRgb_c;

        dataLen -= strlen("rgb");

        while(dataLen != 0)
        {
            if(*p == 'r')
            {
                p++;
                dataLen--;
                redValue = NWKU_atoi(p);
            }

            if(*p == 'g')
            {
                p++;
                dataLen--;
                greenValue = NWKU_atoi(p);
            }

            if(*p == 'b')
            {
                p++;
                dataLen--;
                blueValue = NWKU_atoi(p);
            }
            dataLen--;
            p++;
        }

        /* Update RGB values */
#if gLedRgbEnabled_d
        Led_UpdateRgbState(redValue, greenValue, blueValue);
#else
        appState = gDeviceState_AppLedOff_c;

        if(redValue || greenValue || blueValue)
        {
            appState = gDeviceState_AppLedOn_c;
        }
#endif
        App_UpdateStateLeds(appState);
        /* If device is leader and has received a RGB LED off command and there were no previous button presses */
        if((gpaThrAttr[mThrInstanceId]->devRole == gThrDevRole_Leader_c) &&
           (!redValue && !greenValue && !blueValue) && (leaderLedTimestamp == 0))
        {
            leaderLedTimestamp = (TMR_GetTimestamp()/1000000) + gAppRestoreLeaderLedTimeout_c;
        }
    }
    else if(FLib_MemCmp(pCommand, "color wheel",11))
    {
#if gLedRgbEnabled_d
        App_UpdateStateLeds(gDeviceState_AppLedColorWheel_c);
#else
        App_UpdateStateLeds(gDeviceState_AppLedFlash_c);
#endif
    }
}

/*!*************************************************************************************************
\private
\fn     static void App_RestoreLeaderLed(void *param)
\brief  Called in Application state to restore leader LED.

\param  [in]    param    Not used
***************************************************************************************************/
static void App_RestoreLeaderLed
(
    void *param
)
{
    App_UpdateStateLeds(gDeviceState_Leader_c);
}

