//--------------------------------------------------------------------------------------------------
/** @file supervisor/apps.c
 *
 * Module that handles all Legato applications within the Supervisor.  This module also handles all
 * app related IPC messages.
 *
 *  - @ref c_apps_applications
 *  - @ref c_apps_appProcs
 *
 * @section c_apps_applications Applications
 *
 * An app can be started by either an IPC call or automatically on start-up using the
 * apps_AutoStart() API.
 *
 * When an app is started for the first time a new app container object is created which contains a
 * list link, an app stop handler reference and the app object (which is also instantiated).
 *
 * Once the app container is created the app is started.  The app container is then placed on a
 * list of active apps.
 *
 * An app can be stopped by either an IPC call, a shutdown of the framework or when the app
 * terminates either normally or if due to a fault action.
 *
 * The app's stop handler is set by the IPC handler and/or the fault monitor to take appropriate
 * actions when the app stops.  This is done because application stops are generally asynchronous.
 * For example, when an IPC commands an app to stop the IPC handler will set the app stop handler
 * then initiate the app stop by calling app_Stop().  However, the app may not stop right away
 * because all the processes in the app must first be killed and reaped.  The state of the app must
 * be checked within the SIGCHILD handler.  The SIGCHILD handler will then call the app stop handler
 * when the app has actually stopped.
 *
 * When an app has stopped it is popped of the active list and placed onto the inactive list of
 * apps.  When an app is restarted it is moved from the inactive list to the active list.  This
 * means we do not have to recreate app containers each time.  App containers are only cleaned when
 * the app is uninstalled.
 *
 * @section c_apps_appProcs Application Processes
 *
 * Generally the processes in an application are encapsulated and handled by the application class
 * in app.c.  However, to support command line control of processes within applications, references
 * to processes can be created and given to clients over the IPC API le_appProc.api.
 *
 * This API allows a client to get a reference to a configured process within an app, attached to
 * the process's standard streams, modify the process parameters (such as priority, etc.) and run
 * the process within the app.  Modifications to the process must not be persistent such that once
 * the client disconnects and the process is started normally the modified parameters are not used.
 * A configured process can only be reference by at most one client.
 *
 * The le_appProc.api also allows clients to create references to processes that are not configured
 * for the app.  This usage requires that the client provide an executable that is accessible by the
 * app.  The created process will run with default parameters (such as priority) unless specified by
 * the client.  These created processes are deleted as soon as the client disconnects so that when
 * the app is started normally only the configured processes are run.
 *
 * Copyright (C) Sierra Wireless Inc. Use of this work is subject to license.
 */

#include "legato.h"
#include "apps.h"
#include "app.h"
#include "interfaces.h"
#include "limit.h"
#include "wait.h"
#include "sysPaths.h"
#include "properties.h"
#include "smack.h"


//--------------------------------------------------------------------------------------------------
/**
 * The name of the node in the config tree that contains the list of all apps.
 *
 * If this entry in the config tree is missing or empty then no apps will be launched.
 */
//--------------------------------------------------------------------------------------------------
#define CFG_NODE_APPS_LIST                  "apps"


//--------------------------------------------------------------------------------------------------
/**
 * The name of the node in the config tree that contains the apps startManual value, used
 * to determine whether the app should be launched on system startup or if it should be
 * deferred for manual launch later.
 *
 * The startManual value is either true or false.  If true the app will not be launched on
 * startup.
 *
 * If this entry in the config tree is missing or is empty, automatic start will be used as the
 * default.
 */
//--------------------------------------------------------------------------------------------------
#define CFG_NODE_START_MANUAL               "startManual"


//--------------------------------------------------------------------------------------------------
/**
 * Handler to be called when all applications have shutdown.
 */
//--------------------------------------------------------------------------------------------------
static apps_ShutdownHandler_t AllAppsShutdownHandler = NULL;


//--------------------------------------------------------------------------------------------------
/**
 * app object container reference.  Incomplete type so that we can have the app object container
 * reference the AppStopHandler_t.
 */
//--------------------------------------------------------------------------------------------------
typedef struct _appContainerRef* AppContainerRef_t;


//--------------------------------------------------------------------------------------------------
/**
 * Prototype for app stopped handler.
 */
//--------------------------------------------------------------------------------------------------
typedef void (*AppStopHandler_t)
(
    AppContainerRef_t appContainerRef       ///< [IN] The app that stopped.
);


//--------------------------------------------------------------------------------------------------
/**
 * App object container.
 */
//--------------------------------------------------------------------------------------------------
typedef struct _appContainerRef
{
    app_Ref_t               appRef;         // Reference to the app.
    AppStopHandler_t        stopHandler;    // Handler function that gets called when the app stops.
    le_sup_ctrl_ServerCmdRef_t  stopCmdRef; // Stores the reference to the command that requested
                                            // this app be stopped.  This reference must be sent in
                                            // the response to the stop app command.
    le_dls_Link_t           link;           // Link in the list of apps.
    bool                    isActive;       // true if the app is on the active list.  false if it
                                            // is on the inactive list.
}
AppContainer_t;


//--------------------------------------------------------------------------------------------------
/**
 * Memory pool for app containers.
 */
//--------------------------------------------------------------------------------------------------
static le_mem_PoolRef_t AppContainerPool;


//--------------------------------------------------------------------------------------------------
/**
 * List of all active app containers.
 */
//--------------------------------------------------------------------------------------------------
static le_dls_List_t ActiveAppsList = LE_DLS_LIST_INIT;


//--------------------------------------------------------------------------------------------------
/**
 * List of all inactive app containers.
 */
//--------------------------------------------------------------------------------------------------
static le_dls_List_t InactiveAppsList = LE_DLS_LIST_INIT;


//--------------------------------------------------------------------------------------------------
/**
 * Application Process object container.
 */
//--------------------------------------------------------------------------------------------------
typedef struct
{
    app_Proc_Ref_t      procRef;            ///< The process reference.
    AppContainer_t*     appContainerPtr;    ///< The app container reference.
    le_msg_SessionRef_t clientRef;          ///< Stores the reference to the client that created
                                            ///  this process.
}
AppProcContainer_t;


//--------------------------------------------------------------------------------------------------
/**
 * Memory pool for application process containers.
 */
//--------------------------------------------------------------------------------------------------
static le_mem_PoolRef_t AppProcContainerPool;


//--------------------------------------------------------------------------------------------------
/**
 * Safe reference map of application processes.
 */
//--------------------------------------------------------------------------------------------------
static le_ref_MapRef_t AppProcMap;


//--------------------------------------------------------------------------------------------------
/**
 * Puts the app into the inactive list.
 */
//--------------------------------------------------------------------------------------------------
static void DeactivateAppContainer
(
    AppContainerRef_t appContainerRef           ///< [IN] App to deactivate.
)
{
    le_dls_Remove(&ActiveAppsList, &(appContainerRef->link));

    LE_INFO("Application '%s' has stopped.", app_GetName(appContainerRef->appRef));

    appContainerRef->stopHandler = NULL;

    le_dls_Queue(&InactiveAppsList, &(appContainerRef->link));

    appContainerRef->isActive = false;
}


//--------------------------------------------------------------------------------------------------
/**
 * Restarts an application.
 */
//--------------------------------------------------------------------------------------------------
static void RestartApp
(
    AppContainerRef_t appContainerRef           ///< [IN] App to restart.
)
{
    // Always reset the stop handler to so that when a process dies in the app that does not require
    // a restart it will be handled properly.
    appContainerRef->stopHandler = DeactivateAppContainer;

    // Restart the app.
    if (app_Start(appContainerRef->appRef) == LE_OK)
    {
        LE_INFO("Application '%s' restarted.", app_GetName(appContainerRef->appRef));
    }
    else
    {
        LE_CRIT("Could not restart application '%s'.", app_GetName(appContainerRef->appRef));

        DeactivateAppContainer(appContainerRef);
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Responds to the stop app command.  Also deactivates the app container for the app that just
 * stopped.
 */
//--------------------------------------------------------------------------------------------------
static void RespondToStopAppCmd
(
    AppContainerRef_t appContainerRef           ///< [IN] App that stopped.
)
{
    // Save command reference for later use.
    void* cmdRef = appContainerRef->stopCmdRef;

    DeactivateAppContainer(appContainerRef);

    // Respond to the requesting process.
    le_sup_ctrl_StopAppRespond(cmdRef, LE_OK);
}


//--------------------------------------------------------------------------------------------------
/**
 * Shuts down the next running app.
 *
 * Deletes the current app container.
 */
//--------------------------------------------------------------------------------------------------
static void ShutdownNextApp
(
    AppContainerRef_t appContainerRef           ///< [IN] App that just stopped.
)
{
    LE_INFO("Application '%s' has stopped.", app_GetName(appContainerRef->appRef));

    le_dls_Remove(&ActiveAppsList, &(appContainerRef->link));

    app_Delete(appContainerRef->appRef);

    le_mem_Release(appContainerRef);

    // Continue the shutdown process.
    apps_Shutdown();
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets an active app container by application name.
 *
 * @return
 *      A pointer to the app container if successful.
 *      NULL if the app is not found.
 */
//--------------------------------------------------------------------------------------------------
static AppContainer_t* GetActiveApp
(
    const char* appNamePtr          ///< [IN] Name of the application to get.
)
{
    le_dls_Link_t* appLinkPtr = le_dls_Peek(&ActiveAppsList);

    while (appLinkPtr != NULL)
    {
        AppContainer_t* appContainerPtr = CONTAINER_OF(appLinkPtr, AppContainer_t, link);

        if (strncmp(app_GetName(appContainerPtr->appRef), appNamePtr, LIMIT_MAX_APP_NAME_BYTES) == 0)
        {
            return appContainerPtr;
        }

        appLinkPtr = le_dls_PeekNext(&ActiveAppsList, appLinkPtr);
    }

    return NULL;
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets an inactive app container by application name.
 *
 * @return
 *      A pointer to the app container if successful.
 *      NULL if the app is not found.
 */
//--------------------------------------------------------------------------------------------------
static AppContainer_t* GetInactiveApp
(
    const char* appNamePtr          ///< [IN] Name of the application to get.
)
{
    le_dls_Link_t* appLinkPtr = le_dls_Peek(&InactiveAppsList);

    while (appLinkPtr != NULL)
    {
        AppContainer_t* appContainerPtr = CONTAINER_OF(appLinkPtr, AppContainer_t, link);

        if (strncmp(app_GetName(appContainerPtr->appRef), appNamePtr, LIMIT_MAX_APP_NAME_BYTES) == 0)
        {
            return appContainerPtr;
        }

        appLinkPtr = le_dls_PeekNext(&InactiveAppsList, appLinkPtr);
    }

    return NULL;
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets a pointer to the active app container for the app that has a process with the given PID.
 *
 * @return
 *      A pointer to the app container, if successful.
 *      NULL if the PID is not found.
 */
//--------------------------------------------------------------------------------------------------
static AppContainer_t* GetActiveAppWithProc
(
    pid_t pid
)
{
    le_dls_Link_t* appLinkPtr = le_dls_Peek(&ActiveAppsList);

    while (appLinkPtr != NULL)
    {
        AppContainer_t* appContainerPtr = CONTAINER_OF(appLinkPtr, AppContainer_t, link);

        if (app_HasTopLevelProc(appContainerPtr->appRef, pid))
        {
            return appContainerPtr;
        }

        appLinkPtr = le_dls_PeekNext(&ActiveAppsList, appLinkPtr);
    }

    return NULL;
}


//--------------------------------------------------------------------------------------------------
/**
 * Create the app container if necessary.  This function searches for the app container in the
 * active and inactive lists first, if it can't find it then it creates the app container.
 *
 * @return
 *      A pointer to the app container if successful.
 *      NULL if the app if there was an error. The resultPtr contains the error code.
 */
//--------------------------------------------------------------------------------------------------
static AppContainer_t* CreateApp
(
    const char* appNamePtr,     ///< [IN] Name of the application to launch.
    le_result_t* resultPtr      ///< [OUT] Result: LE_OK if successful the app.
                                ///                LE_NOT_FOUND if the app is not installed.
                                ///                LE_FAULT if there was some other error.
)
{
    // Check active list.
    AppContainer_t* appContainerPtr = GetActiveApp(appNamePtr);

    if (appContainerPtr != NULL)
    {
        *resultPtr = LE_OK;
        return appContainerPtr;
    }

    // Check the inactive list.
    appContainerPtr = GetInactiveApp(appNamePtr);

    if (appContainerPtr != NULL)
    {
        *resultPtr = LE_OK;
        return appContainerPtr;
    }

    // Get the configuration path for this app.
    char configPath[LIMIT_MAX_PATH_BYTES] = { 0 };

    if (le_path_Concat("/", configPath, LIMIT_MAX_PATH_BYTES,
                       CFG_NODE_APPS_LIST, appNamePtr, (char*)NULL) == LE_OVERFLOW)
    {
        LE_ERROR("App name configuration path '%s/%s' too large for internal buffers!",
                 CFG_NODE_APPS_LIST, appNamePtr);

        *resultPtr = LE_FAULT;
        return NULL;
    }

    // Check that the app has a configuration value.
    le_cfg_IteratorRef_t appCfg = le_cfg_CreateReadTxn(configPath);

    if (le_cfg_IsEmpty(appCfg, ""))
    {
        LE_ERROR("Application '%s' is not installed.", appNamePtr);
        le_cfg_CancelTxn(appCfg);

        *resultPtr = LE_NOT_FOUND;
        return NULL;
    }

    // Create the app object.
    app_Ref_t appRef = app_Create(configPath);

    if (appRef == NULL)
    {
        le_cfg_CancelTxn(appCfg);

        *resultPtr = LE_FAULT;
        return NULL;
    }

    // Create the app container for this app.
    appContainerPtr = le_mem_ForceAlloc(AppContainerPool);

    appContainerPtr->appRef = appRef;
    appContainerPtr->link = LE_DLS_LINK_INIT;
    appContainerPtr->stopHandler = NULL;

    // Add this app to the inactive list.
    le_dls_Queue(&InactiveAppsList, &(appContainerPtr->link));
    appContainerPtr->isActive = false;

    le_cfg_CancelTxn(appCfg);

    *resultPtr = LE_OK;
    return appContainerPtr;
}


//--------------------------------------------------------------------------------------------------
/**
 * Starts an app.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t StartApp
(
    AppContainer_t* appContainerPtr         ///< [IN] App to start.
)
{
    le_dls_Remove(&InactiveAppsList, &(appContainerPtr->link));

    // Reset the running apps stop handler.
    appContainerPtr->stopHandler = DeactivateAppContainer;

    // Add the app to the active list.
    le_dls_Queue(&ActiveAppsList, &(appContainerPtr->link));
    appContainerPtr->isActive = true;

    // Start the app.
    return app_Start(appContainerPtr->appRef);
}


//--------------------------------------------------------------------------------------------------
/**
 * Launch an app. Create the app container if necessary and start all the app's processes.
 *
 * @return
 *      LE_OK if successfully launched the app.
 *      LE_DUPLICATE if the app is already running.
 *      LE_NOT_FOUND if the app is not installed.
 *      LE_FAULT if the app could not be launched.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t LaunchApp
(
    const char* appNamePtr      ///< [IN] Name of the application to launch.
)
{
    // Create the app.
    le_result_t result = LE_FAULT;

    AppContainer_t* appContainerPtr = CreateApp(appNamePtr, &result);

    if (appContainerPtr == NULL)
    {
        LE_ERROR("Application '%s' cannot run.", appNamePtr);
        return result;
    }

    if (appContainerPtr->isActive)
    {
        LE_ERROR("Application '%s' is already running.", appNamePtr);
        return LE_DUPLICATE;
    }

    // Start the app.
    return StartApp(appContainerPtr);
}


//--------------------------------------------------------------------------------------------------
/**
 * Handle application fault.  Gets the application fault action for the process that terminated
 * and handle the fault.
 *
 * @return
 *      LE_OK if the fault was handled.
 *      LE_FAULT if the fault could not be handled.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t HandleAppFault
(
    AppContainer_t* appContainerPtr,    ///< [IN] Application container reference.
    pid_t procPid,                      ///< [IN] Pid of the process that changed state.
    int procExitStatus                  ///< [IN] Return status of the process given by wait().
)
{
    // Get the fault action.
    FaultAction_t faultAction = FAULT_ACTION_IGNORE;

    app_SigChildHandler(appContainerPtr->appRef, procPid, procExitStatus, &faultAction);

    // Handle the fault.
    switch (faultAction)
    {
        case FAULT_ACTION_IGNORE:
            // Do nothing.
            break;

        case FAULT_ACTION_RESTART_APP:
            if (app_GetState(appContainerPtr->appRef) != APP_STATE_STOPPED)
            {
                // Stop the app if it hasn't already stopped.
                app_Stop(appContainerPtr->appRef);
            }

            // Set the handler to restart the app when the app stops.
            appContainerPtr->stopHandler = RestartApp;
            break;

        case FAULT_ACTION_STOP_APP:
            if (app_GetState(appContainerPtr->appRef) != APP_STATE_STOPPED)
            {
                // Stop the app if it hasn't already stopped.
                app_Stop(appContainerPtr->appRef);
            }
            break;

        case FAULT_ACTION_REBOOT:
            return LE_FAULT;

        default:
            LE_FATAL("Unexpected fault action %d.", faultAction);
    }

    // Check if the app has stopped.
    if ( (app_GetState(appContainerPtr->appRef) == APP_STATE_STOPPED) &&
         (appContainerPtr->stopHandler != NULL) )
    {
        // The application has stopped.  Call the app stop handler.
        appContainerPtr->stopHandler(appContainerPtr);
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Deletes all application process containers for either an application or a client.
 */
//--------------------------------------------------------------------------------------------------
static void DeleteAppProcs
(
    app_Ref_t appRef,                       ///< Apps to delete from. NULL if not used.
    le_msg_SessionRef_t clientRef           ///< Client to delete from.  NULL if not used.
)
{
    // Iterate over the safe references to find all application process containers for this client.
    le_ref_IterRef_t iter = le_ref_GetIterator(AppProcMap);

    while (le_ref_NextNode(iter) == LE_OK)
    {
        // Get the app process container.
        // WARNING: Casting away the const from le_ref_GetValue() and le_ref_GetSafeRef() so we can
        //          delete the data and the safe reference.  Should these functions be changed to
        //          not return a const value pointer?
        AppProcContainer_t* appProcContainerPtr = (AppProcContainer_t*)le_ref_GetValue(iter);

        LE_ASSERT(appProcContainerPtr != NULL);

        if ( ((appRef != NULL) && (appProcContainerPtr->appContainerPtr->appRef == appRef)) ||
             ((clientRef != NULL) && (appProcContainerPtr->clientRef == clientRef)) )
        {
            // Delete the safe reference.
            void* safeRef = (void*)le_ref_GetSafeRef(iter);
            LE_ASSERT(safeRef != NULL);

            le_ref_DeleteRef(AppProcMap, safeRef);

            // Delete the app proc.
            app_DeleteProc(appProcContainerPtr->appContainerPtr->appRef,
                           appProcContainerPtr->procRef);

            // Free the container.
            le_mem_Release(appProcContainerPtr);
        }
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Deletes all application process containers for the client with the given session reference.
 */
//--------------------------------------------------------------------------------------------------
static void DeleteClientAppProcs
(
    le_msg_SessionRef_t sessionRef,         ///< Session reference of the client.
    void*               contextPtr          ///< Not used.
)
{
    DeleteAppProcs(NULL, sessionRef);
}


//--------------------------------------------------------------------------------------------------
/**
 * Deletes an inactive app object.
 */
//--------------------------------------------------------------------------------------------------
static void DeletesInactiveApp
(
    const char* appName,  ///< App being removed.
    void* contextPtr      ///< Context for this function.  Not used.
)
{
    // Find the app.
    AppContainer_t* appContainerPtr = GetInactiveApp(appName);

    if (appContainerPtr != NULL)
    {
        le_dls_Remove(&InactiveAppsList, &(appContainerPtr->link));

        // Delete any app procs containers in this app.
        DeleteAppProcs(appContainerPtr->appRef, NULL);

        // Delete the app object and container.
        app_Delete(appContainerPtr->appRef);

        le_mem_Release(appContainerPtr);

        LE_DEBUG("Deleted app %s.", appName);
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Deletes all inactive app objects.
 */
//--------------------------------------------------------------------------------------------------
static void DeletesAllInactiveApp
(
    void
)
{
    le_dls_Link_t* appLinkPtr = le_dls_Pop(&InactiveAppsList);

    while (appLinkPtr != NULL)
    {
        AppContainer_t* appContainerPtr = CONTAINER_OF(appLinkPtr, AppContainer_t, link);

        // Delete any app procs containers in this app.
        DeleteAppProcs(appContainerPtr->appRef, NULL);

        // Delete the app object and container.
        app_Delete(appContainerPtr->appRef);

        le_mem_Release(appContainerPtr);

        appLinkPtr = le_dls_Pop(&InactiveAppsList);
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Checks whether an app's process is reference by any clients.
 */
//--------------------------------------------------------------------------------------------------
static bool IsAppProcAlreadyReferenced
(
    app_Proc_Ref_t appProcRef               ///< [IN] App process reference.
)
{
    // Iterate over the safe references to find all application process containers.
    le_ref_IterRef_t iter = le_ref_GetIterator(AppProcMap);

    while (le_ref_NextNode(iter) == LE_OK)
    {
        // Get the app process container.
        AppProcContainer_t* appProcContainerPtr = (AppProcContainer_t*)le_ref_GetValue(iter);

        LE_ASSERT(appProcContainerPtr != NULL);

        if (appProcContainerPtr->procRef == appProcRef)
        {
            return true;
        }
    }

    return false;
}


//--------------------------------------------------------------------------------------------------
/**
 * Checks process name.
 */
//--------------------------------------------------------------------------------------------------
static bool IsProcNameValid
(
    const char* procNamePtr         ///< [IN] Process name.
)
{
    if ( (procNamePtr == NULL) || (strcmp(procNamePtr, "") == 0) )
    {
        LE_ERROR("Process name cannot be empty.");
        return false;
    }

    if (strstr(procNamePtr, "/") != NULL)
    {
        LE_ERROR("Process name contains illegal character '/'.");
        return false;
    }

    return true;
}


//--------------------------------------------------------------------------------------------------
/**
 * Checks app name.
 */
//--------------------------------------------------------------------------------------------------
static bool IsAppNameValid
(
    const char* appNamePtr          ///< [IN] App name.
)
{
    if ( (appNamePtr == NULL) || (strcmp(appNamePtr, "") == 0) )
    {
        LE_ERROR("App name cannot be empty.");
        return false;
    }

    if (strstr(appNamePtr, "/") != NULL)
    {
        LE_ERROR("App name contains illegal character '/'.");
        return false;
    }

    return true;
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets the application name of the process with the specified PID.
 *
 * @return
 *      LE_OK if the application name was successfully found.
 *      LE_OVERFLOW if the application name could not fit in the provided buffer.
 *      LE_NOT_FOUND if the process is not part of an application.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t GetAppNameFromPid
(
    int pid,                ///< [IN] PID of the process.
    char* bufPtr,           ///< [OUT] Buffer to hold the name of the app.
    size_t bufSize          ///< [IN] Size of the buffer.
)
{
    // Get the SMACK label for the process.
    char smackLabel[LIMIT_MAX_SMACK_LABEL_BYTES];

    le_result_t result = smack_GetProcLabel(pid, smackLabel, sizeof(smackLabel));

    if (result != LE_OK)
    {
        return result;
    }

    // Strip the prefix from the label.
    if (strncmp(smackLabel, SMACK_APP_PREFIX, sizeof(SMACK_APP_PREFIX)-1) == 0)
    {
        return le_utf8_Copy(bufPtr, &(smackLabel[strlen(SMACK_APP_PREFIX)]), bufSize, NULL);
    }

    return LE_NOT_FOUND;
}


//--------------------------------------------------------------------------------------------------
/**
 * Initialize the applications system.
 */
//--------------------------------------------------------------------------------------------------
void apps_Init
(
    void
)
{
    app_Init();

    // Create memory pools.
    AppContainerPool = le_mem_CreatePool("appContainers", sizeof(AppContainer_t));
    AppProcContainerPool = le_mem_CreatePool("appProcContainers", sizeof(AppProcContainer_t));

    AppProcMap = le_ref_CreateMap("AppProcs", 5);

    le_instStat_AddAppUninstallEventHandler(DeletesInactiveApp, NULL);
    le_instStat_AddAppInstallEventHandler(DeletesInactiveApp, NULL);

    le_msg_AddServiceCloseHandler(le_appProc_GetServiceRef(), DeleteClientAppProcs, NULL);
}


//--------------------------------------------------------------------------------------------------
/**
 * Initiates the shut down of all the applications.  The shut down sequence happens asynchronously.
 * A shut down handler should be set using apps_SetShutdownHandler() to be
 * notified when all applications actually shut down.
 */
//--------------------------------------------------------------------------------------------------
void apps_Shutdown
(
    void
)
{
    // Deletes all inactive apps first.
    DeletesAllInactiveApp();

    // Get the first app to stop.
    le_dls_Link_t* appLinkPtr = le_dls_Peek(&ActiveAppsList);

    if (appLinkPtr != NULL)
    {
        AppContainer_t* appContainerPtr = CONTAINER_OF(appLinkPtr, AppContainer_t, link);

        // Set the stop handler that will continue to stop all apps and the framework.
        appContainerPtr->stopHandler = ShutdownNextApp;

        // Stop the first app.  This will kick off the chain of callback handlers that will stop
        // all apps.
        app_Stop(appContainerPtr->appRef);

        // If the application has already stopped then call its stop handler here.  Otherwise the
        // stop handler will be called from the SigChildHandler() when the app actually stops.
        if (app_GetState(appContainerPtr->appRef) == APP_STATE_STOPPED)
        {
            appContainerPtr->stopHandler(appContainerPtr);
        }
    }
    else if (AllAppsShutdownHandler != NULL)
    {
        AllAppsShutdownHandler();
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets the shutdown handler to be called when all the applications have shutdown.
 */
//--------------------------------------------------------------------------------------------------
void apps_SetShutdownHandler
(
    apps_ShutdownHandler_t shutdownHandler      ///< [IN] Shut down handler.  Can be NULL.
)
{
    AllAppsShutdownHandler = shutdownHandler;
}


//--------------------------------------------------------------------------------------------------
/**
 * Start all applications marked as 'auto' start.
 */
//--------------------------------------------------------------------------------------------------
void apps_AutoStart
(
    void
)
{
    // Read the list of applications from the config tree.
    le_cfg_IteratorRef_t appCfg = le_cfg_CreateReadTxn(CFG_NODE_APPS_LIST);

    if (le_cfg_GoToFirstChild(appCfg) != LE_OK)
    {
        LE_WARN("No applications installed.");

        le_cfg_CancelTxn(appCfg);

        return;
    }

    do
    {
        // Check the start mode for this application.
        if (!le_cfg_GetBool(appCfg, CFG_NODE_START_MANUAL, false))
        {
            // Get the app name.
            char appName[LIMIT_MAX_APP_NAME_BYTES];

            if (le_cfg_GetNodeName(appCfg, "", appName, sizeof(appName)) == LE_OVERFLOW)
            {
                LE_ERROR("AppName buffer was too small, name truncated to '%s'.  "
                         "Max app name in bytes, %d.  Application not launched.",
                         appName, LIMIT_MAX_APP_NAME_BYTES);
            }
            else
            {
                // Launch the application now.  No need to check the return code because there is
                // nothing we can do about errors.
                LaunchApp(appName);
            }
        }
    }
    while (le_cfg_GoToNextSibling(appCfg) == LE_OK);

    le_cfg_CancelTxn(appCfg);
}


//--------------------------------------------------------------------------------------------------
/**
 * The SIGCHLD handler for the applications.  This should be called from the Supervisor's SIGCHILD
 * handler.
 *
 * @note
 *      This function will reap the child if the child is an application process, otherwise the
 *      child will remain unreaped.
 *
 * @return
 *      LE_OK if the signal was handled without incident.
 *      LE_NOT_FOUND if the pid is not an application process.  The child will not be reaped.
 *      LE_FAULT if the signal indicates a failure of one of the applications which requires a
 *               system restart.
 */
//--------------------------------------------------------------------------------------------------
le_result_t apps_SigChildHandler
(
    pid_t pid               ///< [IN] Pid of the process that produced the SIGCHLD.
)
{
    // Get the name of the application this process belongs to from the dead process's SMACK
    // label.  Must do this before we reap the process, or the SMACK label will be unavailable.
    char appName[LIMIT_MAX_APP_NAME_BYTES] = "";
    le_result_t result = GetAppNameFromPid(pid, appName, sizeof(appName));

    LE_FATAL_IF(result == LE_OVERFLOW, "App name '%s...' is too long.", appName);

    if (result == LE_FAULT)
    {
        LE_CRIT("Could not get app name for child process %d.", pid);
        return LE_NOT_FOUND;
    }

    AppContainer_t* appContainerPtr = NULL;

    if (result == LE_NOT_FOUND)
    {
        // It's possible that we killed an app process before it had a chance to set
        // its own SMACK label.  So, search the apps for the PID.
        appContainerPtr = GetActiveAppWithProc(pid);

        if (appContainerPtr == NULL)
        {
            return LE_NOT_FOUND;
        }
    }
    else
    {
        // Got the app name for the process.  Now get the app object by name.
        appContainerPtr = GetActiveApp(appName);

        if (appContainerPtr == NULL)
        {
            // There is an app name but the app container can't be found.  This can happen if
            // non-direct descendant app proceses are zombies (died but not yet reaped) when the app
            // was deactivated.
            LE_INFO("Reaping app process (PID %d) for stopped app %s.", pid, appName);

            wait_ReapChild(pid);

            return LE_OK;
        }
    }

    // This child process is an application process.
    // Reap the child now.
    int status = wait_ReapChild(pid);

    // Handle any faults that the child process state change my have caused.
    return HandleAppFault(appContainerPtr, pid, status);
}


//--------------------------------------------------------------------------------------------------
/**
 * Starts an app.  This function is called by the event loop when a separate process requests to
 * start an app.
 *
 * @note
 *   The result code for this command should be sent back to the requesting process via
 *   le_sup_ctrl_StartAppRespond().  The possible result codes are:
 *
 *      LE_OK if the app is successfully started.
 *      LE_DUPLICATE if the app is already running.
 *      LE_NOT_FOUND if the app is not installed.
 *      LE_FAULT if there was an error and the app could not be launched.
 */
//--------------------------------------------------------------------------------------------------
void le_sup_ctrl_StartApp
(
    le_sup_ctrl_ServerCmdRef_t cmdRef,  ///< [IN] Command reference that must be passed to this
                                        ///       command's response function.
    const char* appName                 ///< [IN] Name of the application to start.
)
{
    if (!IsAppNameValid(appName))
    {
        LE_KILL_CLIENT("Invalid app name.");
        return;
    }

    LE_DEBUG("Received request to start application '%s'.", appName);

    le_sup_ctrl_StartAppRespond(cmdRef, LaunchApp(appName));
}


//--------------------------------------------------------------------------------------------------
/**
 * Stops an app. This function is called by the event loop when a separate process requests to stop
 * an app.
 *
 * @note
 *   The result code for this command should be sent back to the requesting process via
 *   le_sup_ctrl_StopAppRespond(). The possible result codes are:
 *
 *      LE_OK if successful.
 *      LE_NOT_FOUND if the app could not be found.
 */
//--------------------------------------------------------------------------------------------------
void le_sup_ctrl_StopApp
(
    le_sup_ctrl_ServerCmdRef_t cmdRef,  ///< [IN] Command reference that must be passed to this
                                        ///       command's response function.
    const char* appName                 ///< [IN] Name of the application to stop.
)
{
    if (!IsAppNameValid(appName))
    {
        LE_KILL_CLIENT("Invalid app name.");
        return;
    }

    LE_DEBUG("Received request to stop application '%s'.", appName);

    // Get the app object.
    AppContainer_t* appContainerPtr = GetActiveApp(appName);

    if (appContainerPtr == NULL)
    {
        LE_WARN("Application '%s' is not running and cannot be stopped.", appName);

        le_sup_ctrl_StopAppRespond(cmdRef, LE_NOT_FOUND);
        return;
    }

    // Save this commands reference in this app.
    appContainerPtr->stopCmdRef = cmdRef;

    // Set the handler to be called when this app stops.  This handler will also respond to the
    // process that requested this app be stopped.
    appContainerPtr->stopHandler = RespondToStopAppCmd;

    // Stop the process.  This is an asynchronous call that returns right away.
    app_Stop(appContainerPtr->appRef);

    // If the application has already stopped then call its stop handler here.  Otherwise the stop
    // handler will be called from the SigChildHandler() when the app actually stops.
    if (app_GetState(appContainerPtr->appRef) == APP_STATE_STOPPED)
    {
        appContainerPtr->stopHandler(appContainerPtr);
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets the state of the specified application.  The state of unknown applications is STOPPED.
 *
 * @return
 *      The state of the specified application.
 */
//--------------------------------------------------------------------------------------------------
le_appInfo_State_t le_appInfo_GetState
(
    const char* appName
        ///< [IN]
        ///< Name of the application.
)
{
    if (!IsAppNameValid(appName))
    {
        LE_KILL_CLIENT("Invalid app name.");
        return LE_APPINFO_STOPPED;
    }

    // Search the list of apps.
    le_dls_Link_t* appLinkPtr = le_dls_Peek(&ActiveAppsList);

    while (appLinkPtr != NULL)
    {
        AppContainer_t* appContainerPtr = CONTAINER_OF(appLinkPtr, AppContainer_t, link);

        if (strncmp(app_GetName(appContainerPtr->appRef), appName, LIMIT_MAX_APP_NAME_BYTES) == 0)
        {
            switch (app_GetState(appContainerPtr->appRef))
            {
                case APP_STATE_STOPPED:
                    return LE_APPINFO_STOPPED;

                case APP_STATE_RUNNING:
                    return LE_APPINFO_RUNNING;

                default:
                    LE_FATAL("Unrecognized app state.");
            }
        }

        appLinkPtr = le_dls_PeekNext(&ActiveAppsList, appLinkPtr);
    }

    return LE_APPINFO_STOPPED;
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets the state of the specified process in an application.  This function only works for
 * configured processes that the Supervisor starts directly.
 *
 * @return
 *      The state of the specified process.
 */
//--------------------------------------------------------------------------------------------------
le_appInfo_ProcState_t le_appInfo_GetProcState
(
    const char* appName,
        ///< [IN]
        ///< Name of the application.

    const char* procName
        ///< [IN]
        ///< Name of the process.
)
{
    if (!IsAppNameValid(appName))
    {
        LE_KILL_CLIENT("Invalid app name.");
        return LE_APPINFO_PROC_STOPPED;
    }

    if (!IsProcNameValid(procName))
    {
        LE_KILL_CLIENT("Invalid process name.");
        return LE_APPINFO_PROC_STOPPED;
    }

    // Search the list of apps.
    le_dls_Link_t* appLinkPtr = le_dls_Peek(&ActiveAppsList);

    while (appLinkPtr != NULL)
    {
        AppContainer_t* appContainerPtr = CONTAINER_OF(appLinkPtr, AppContainer_t, link);

        if (strncmp(app_GetName(appContainerPtr->appRef), appName, LIMIT_MAX_APP_NAME_BYTES) == 0)
        {
            switch (app_GetProcState(appContainerPtr->appRef, procName))
            {
                case APP_PROC_STATE_STOPPED:
                    return LE_APPINFO_PROC_STOPPED;

                case APP_PROC_STATE_RUNNING:
                    return LE_APPINFO_PROC_RUNNING;

                default:
                    LE_FATAL("Unrecognized proc state.");
            }
        }

        appLinkPtr = le_dls_PeekNext(&ActiveAppsList, appLinkPtr);
    }

    return LE_APPINFO_PROC_STOPPED;
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets the application name of the process with the specified PID.
 *
 * @return
 *      LE_OK if the application name was successfully found.
 *      LE_OVERFLOW if the application name could not fit in the provided buffer.
 *      LE_NOT_FOUND if the process is not part of an application.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_appInfo_GetName
(
    int32_t pid,
        ///< [IN]
        ///< PID of the process.

    char* appName,
        ///< [OUT]
        ///< Application name

    size_t appNameNumElements
        ///< [IN]
)
{
    return GetAppNameFromPid(pid, appName, appNameNumElements);
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets the application hash as a hexidecimal string.  The application hash is a unique hash of the
 * current version of the application.
 *
 * @return
 *      LE_OK if the application has was successfully retrieved.
 *      LE_OVERFLOW if the application hash could not fit in the provided buffer.
 *      LE_NOT_FOUND if the application is not installed.
 *      LE_FAULT if there was an error.
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_appInfo_GetHash
(
    const char* appName,
        ///< [IN]
        ///< Application name.

    char* hashStr,
        ///< [OUT]
        ///< Hash string.

    size_t hashStrNumElements
        ///< [IN]
)
{
#define APP_INFO_FILE                       "info.properties"
#define KEY_STR_MD5                         "app.md5"

    if (!IsAppNameValid(appName))
    {
        LE_KILL_CLIENT("Invalid app name.");
        return LE_FAULT;
    }

    // Get the path to the app's info file.
    char infoFilePath[LIMIT_MAX_PATH_BYTES] = APPS_INSTALL_DIR;
    LE_ERROR_IF(le_path_Concat("/",
                               infoFilePath,
                               sizeof(infoFilePath),
                               appName,
                               APP_INFO_FILE,
                               NULL) != LE_OK,
                "Path to app %s's %s is too long.", appName, APP_INFO_FILE);

    // Check if the file exists.
    struct stat statBuf;

    if (stat(infoFilePath, &statBuf) == -1)
    {
        if (errno == ENOENT)
        {
            return LE_NOT_FOUND;
        }

        LE_ERROR("Could not stat file '%s'.  %m.", infoFilePath);
        return LE_FAULT;
    }

    // Get the md5 hash for the app's info.properties file.
    le_result_t result = properties_GetValueForKey(infoFilePath,
                                                   KEY_STR_MD5,
                                                   hashStr,
                                                   hashStrNumElements);

    switch(result)
    {
        case LE_OK:
        case LE_OVERFLOW:
            return result;

        default:
            return LE_FAULT;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * A watchdog has timed out. This function determines the watchdogAction to take and applies it.
 * The action to take is first delegated to the app (and proc layers) and actions not handled by
 * or not appropriate for lower layers are handled here.
 */
//--------------------------------------------------------------------------------------------------
void le_sup_wdog_WatchdogTimedOut
(
    le_sup_wdog_ServerCmdRef_t cmdRef,
    uint32_t userId,
    uint32_t procId
)
{
    le_sup_wdog_WatchdogTimedOutRespond(cmdRef);
    LE_INFO("Handling watchdog expiry for: userId %d, procId %d", userId, procId);

    // Search for the process in the list of apps.
    le_dls_Link_t* appLinkPtr = le_dls_Peek(&ActiveAppsList);

    while (appLinkPtr != NULL)
    {
        AppContainer_t* appContainerPtr = CONTAINER_OF(appLinkPtr, AppContainer_t, link);

        wdog_action_WatchdogAction_t watchdogAction = WATCHDOG_ACTION_NOT_FOUND;

        LE_FATAL_IF(appContainerPtr == NULL, "Got a NULL AppPtr from CONTAINER_OF");

        if (app_WatchdogTimeoutHandler(appContainerPtr->appRef, procId, &watchdogAction) == LE_OK)
        {
            // Handle the fault.
            switch (watchdogAction)
            {
                case WATCHDOG_ACTION_NOT_FOUND:
                    // This case should already have been dealt with in lower layers, should never
                    // get here.
                    LE_FATAL("Unhandled watchdog action not found caught by supervisor.");

                case WATCHDOG_ACTION_IGNORE:
                case WATCHDOG_ACTION_HANDLED:
                    // Do nothing.
                    break;

                case WATCHDOG_ACTION_REBOOT:
                    ///< @todo Need to use a reboot API here that actually reboots the entire module
                    ///        rather than just the framework so that possibly connected peripherals
                    ///        get reset as well.  So, for now we will just log an error message and
                    ///        restart the app.
                    LE_CRIT("Watchdog action requires a reboot but a module reboot is not yet supported. \
restarting the app instead.");

                case WATCHDOG_ACTION_RESTART_APP:
                    if (app_GetState(appContainerPtr->appRef) != APP_STATE_STOPPED)
                    {
                        // Stop the app if it hasn't already stopped.
                        app_Stop(appContainerPtr->appRef);
                    }

                    // Set the handler to restart the app when the app stops.
                    appContainerPtr->stopHandler = RestartApp;
                    break;

                case WATCHDOG_ACTION_STOP_APP:
                    if (app_GetState(appContainerPtr->appRef) != APP_STATE_STOPPED)
                    {
                        // Stop the app if it hasn't already stopped.
                        app_Stop(appContainerPtr->appRef);
                    }
                    break;

                // This should never happen
                case WATCHDOG_ACTION_ERROR:
                    LE_FATAL("Unhandled watchdog action error caught by supervisor.");

                // This should never happen
                default:
                    LE_FATAL("Unknown watchdog action %d.", watchdogAction);
            }

            // Check if the app has stopped.
            if ( (app_GetState(appContainerPtr->appRef) == APP_STATE_STOPPED) &&
                 (appContainerPtr->stopHandler != NULL) )
            {
                // The application has stopped.  Call the app stop handler.
                appContainerPtr->stopHandler(appContainerPtr);
            }

            // Stop searching the other apps.
            break;
        }

        appLinkPtr = le_dls_PeekNext(&ActiveAppsList, appLinkPtr);
    }

    if (appLinkPtr == NULL)
    {
        // We exhausted the app list without taking any action for this process
        LE_CRIT("Process pid:%d was not started by the framework. No watchdog action can be taken", procId);
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Creates a process in an app.  This function can be used to create and subsequently start a
 * process in an application that the application normally would not start on its own.  This
 * function does not actually start the process, use Start() to start the process.
 *
 * If the specified process name matches a name in the app's list of configured processes then
 * runtime parameters such as environment variables, priority, etc. will be taken from the
 * configuration database.  Otherwise default parameters will be used.
 *
 * Parameters can be overridden by the other functions in this API such as AddArg(), SetPriority(),
 * etc.
 *
 * If the executable path is empty and the process name matches a configured process then the
 * configured executable is used.  Otherwise the specified executable path is used.
 *
 * Either the process name or the executable path may be empty but not both.
 *
 * It is an error to call this function on a configured process that is already running.
 *
 * @return
 *      Reference to the application process object if successful.
 *      NULL if there was an error.
 */
//--------------------------------------------------------------------------------------------------
le_appProc_RefRef_t le_appProc_Create
(
    const char* appName,
        ///< [IN] Name of the app.

    const char* procName,
        ///< [IN] Name of the process.

    const char* execPath
        ///< [IN] Path to the executable file.
)
{
    // Check inputs.
    if (!IsAppNameValid(appName))
    {
        LE_KILL_CLIENT("Invalid app name.");
        return NULL;
    }

    if (procName == NULL)
    {
        LE_KILL_CLIENT("Invalid process name.");
        return NULL;
    }

    if (execPath == NULL)
    {
        LE_KILL_CLIENT("Exec path cannot be NULL.");
        return NULL;
    }

    // Ifgen does not allow NULL pointers to strings.  Translate empty strings to NULLs.
    const char* procNamePtr;
    if (strcmp(procName, "") == 0)
    {
        procNamePtr = NULL;
    }
    else
    {
        procNamePtr = procName;
    }

    const char* execPathPtr;
    if (strcmp(execPath, "") == 0)
    {
        execPathPtr = NULL;
    }
    else
    {
        execPathPtr = execPath;
    }

    if ( (procNamePtr == NULL) && (execPathPtr == NULL) )
    {
        LE_KILL_CLIENT("Process name and executable path cannot both be empty.");
        return NULL;
    }

    // Create the app if it doesn't already exist.
    le_result_t result;

    AppContainer_t* appContainerPtr = CreateApp(appName, &result);

    if (appContainerPtr == NULL)
    {
        return NULL;
    }

    // Create the app process for this app.
    app_Proc_Ref_t procRef = app_CreateProc(appContainerPtr->appRef, procNamePtr, execPathPtr);

    if (procRef == NULL)
    {
        return NULL;
    }

    // Check that we don't already have a reference to this process.
    if (IsAppProcAlreadyReferenced(procRef))
    {
        LE_KILL_CLIENT("Process is already referenced by a client.");
        return NULL;
    }

    // Create the app proc container to store stuff like the client session reference.
    AppProcContainer_t* appProcContainerPtr = le_mem_ForceAlloc(AppProcContainerPool);

    appProcContainerPtr->appContainerPtr = appContainerPtr;
    appProcContainerPtr->procRef = procRef;
    appProcContainerPtr->clientRef = le_appProc_GetClientSessionRef();

    // Get a safe reference for this app proc.
    return le_ref_CreateRef(AppProcMap, appProcContainerPtr);
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets the file descriptor that the application process's standard in should be attached to.
 *
 * By default the standard in is directed to /dev/null.
 *
 * If there is an error this function will kill the calling process
 */
//--------------------------------------------------------------------------------------------------
void le_appProc_SetStdIn
(
    le_appProc_RefRef_t appProcRef,
        ///< [IN] Application process to start.

    int stdInFd
        ///< [IN] File descriptor to use as the app proc's standard in.
)
{
    AppProcContainer_t* appProcContainerPtr = le_ref_Lookup(AppProcMap, appProcRef);

    if (appProcContainerPtr == NULL)
    {
        LE_KILL_CLIENT("Invalid application process reference.");
        return;
    }

    app_SetProcStdIn(appProcContainerPtr->procRef, stdInFd);
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets the file descriptor that the application process's standard out should be attached to.
 *
 * By default the standard out is directed to the logs.
 *
 * If there is an error this function will kill the calling process
 */
//--------------------------------------------------------------------------------------------------
void le_appProc_SetStdOut
(
    le_appProc_RefRef_t appProcRef,
        ///< [IN] Application process to start.

    int stdOutFd
        ///< [IN] File descriptor to use as the app proc's standard out.
)
{
    AppProcContainer_t* appProcContainerPtr = le_ref_Lookup(AppProcMap, appProcRef);

    if (appProcContainerPtr == NULL)
    {
        LE_KILL_CLIENT("Invalid application process reference.");
        return;
    }

    app_SetProcStdOut(appProcContainerPtr->procRef, stdOutFd);
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets the file descriptor that the application process's standard err should be attached to.
 *
 * By default the standard err is directed to the logs.
 *
 * If there is an error this function will kill the calling process
 */
//--------------------------------------------------------------------------------------------------
void le_appProc_SetStdErr
(
    le_appProc_RefRef_t appProcRef,
        ///< [IN] Application process to start.

    int stdErrFd
        ///< [IN] File descriptor to use as the app proc's standard error.
)
{
    AppProcContainer_t* appProcContainerPtr = le_ref_Lookup(AppProcMap, appProcRef);

    if (appProcContainerPtr == NULL)
    {
        LE_KILL_CLIENT("Invalid application process reference.");
        return;
    }

    app_SetProcStdErr(appProcContainerPtr->procRef, stdErrFd);
}


//--------------------------------------------------------------------------------------------------
/**
 * Add handler function for EVENT 'le_appProc_Stop'
 *
 * Process stopped event.
 */
//--------------------------------------------------------------------------------------------------
le_appProc_StopHandlerRef_t le_appProc_AddStopHandler
(
    le_appProc_RefRef_t appProcRef,
        ///< [IN] Application process to start.

    le_appProc_StopHandlerFunc_t handlerPtr,
        ///< [IN]

    void* contextPtr
        ///< [IN]
)
{
    AppProcContainer_t* appProcContainerPtr = le_ref_Lookup(AppProcMap, appProcRef);

    if (appProcContainerPtr == NULL)
    {
        LE_KILL_CLIENT("Invalid application process reference.");
        return NULL;
    }

    app_SetProcStopHandler(appProcContainerPtr->procRef, handlerPtr, contextPtr);

    // There is only one handler for each proc so just return the appProcRef which can be used to
    // find the handler.
    return (le_appProc_StopHandlerRef_t)appProcRef;
}


//--------------------------------------------------------------------------------------------------
/**
 * Remove handler function for EVENT 'le_appProc_Stop'
 */
//--------------------------------------------------------------------------------------------------
void le_appProc_RemoveStopHandler
(
    le_appProc_StopHandlerRef_t addHandlerRef
        ///< [IN]
)
{
    AppProcContainer_t* appProcContainerPtr = le_ref_Lookup(AppProcMap, addHandlerRef);

    if (appProcContainerPtr == NULL)
    {
        // Client may have already been deleted.
        return;
    }

    // Clear the handler.
    app_SetProcStopHandler(appProcContainerPtr->procRef, NULL, NULL);
}


//--------------------------------------------------------------------------------------------------
/**
 * Adds a command line argument to the application process.
 *
 * If the application process is a configured process adding any argument means no arguments from
 * the configuration database will be used.
 *
 * Adding an empty argument validates the argument list but does not acutally add an argument.  This
 * is useful for overriding the configured arguments list with an empty argument list.
 *
 * If there is an error this function will kill the calling client.
 */
//--------------------------------------------------------------------------------------------------
void le_appProc_AddArg
(
    le_appProc_RefRef_t appProcRef,
        ///< [IN] Application process to start.

    const char* arg
        ///< [IN] Argument to add.
)
{
    AppProcContainer_t* appProcContainerPtr = le_ref_Lookup(AppProcMap, appProcRef);

    if (appProcContainerPtr == NULL)
    {
        LE_KILL_CLIENT("Invalid application process reference.");
        return;
    }

    if (app_AddArgs(appProcContainerPtr->procRef, arg) != LE_OK)
    {
        LE_KILL_CLIENT("Argument '%s' is too long.", arg);
        return;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Deletes and invalidates the cmd-line arguments to a process.  This means the process will only
 * use arguments from the config if available.
 */
//--------------------------------------------------------------------------------------------------
void le_appProc_ClearArgs
(
    le_appProc_RefRef_t appProcRef
        ///< [IN] Application process to start.
)
{
    AppProcContainer_t* appProcContainerPtr = le_ref_Lookup(AppProcMap, appProcRef);

    if (appProcContainerPtr == NULL)
    {
        LE_KILL_CLIENT("Invalid application process reference.");
        return;
    }

    app_ClearArgs(appProcContainerPtr->procRef);
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets the application process's priority.
 *
 * The priority string must be either 'idle','low', 'medium', 'high', 'rt1', 'rt2'...'rt32'.
 *
 * If there is an error this function will kill the calling client.
 */
//--------------------------------------------------------------------------------------------------
void le_appProc_SetPriority
(
    le_appProc_RefRef_t appProcRef,
        ///< [IN] Application process to start.

    const char* priority
        ///< [IN] Priority for the application process.
)
{
    AppProcContainer_t* appProcContainerPtr = le_ref_Lookup(AppProcMap, appProcRef);

    if (appProcContainerPtr == NULL)
    {
        LE_KILL_CLIENT("Invalid application process reference.");
        return;
    }

    le_result_t result = app_SetProcPriority(appProcContainerPtr->procRef, priority);

    if (result == LE_OVERFLOW)
    {
        LE_KILL_CLIENT("Priority string '%s' is too long.", priority);
        return;
    }

    if (result == LE_FAULT)
    {
        LE_KILL_CLIENT("Priority string '%s' is invalid.", priority);
        return;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Clears the application process's priority and use either the configured priority or the default.
 */
//--------------------------------------------------------------------------------------------------
void le_appProc_ClearPriority
(
    le_appProc_RefRef_t appProcRef
        ///< [IN] Application process to start.
)
{
    AppProcContainer_t* appProcContainerPtr = le_ref_Lookup(AppProcMap, appProcRef);

    if (appProcContainerPtr == NULL)
    {
        LE_KILL_CLIENT("Invalid application process reference.");
        return;
    }

    LE_ASSERT(app_SetProcPriority(appProcContainerPtr->procRef, NULL) == LE_OK);
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets the application process's fault action.
 */
//--------------------------------------------------------------------------------------------------
void le_appProc_SetFaultAction
(
    le_appProc_RefRef_t appProcRef,
        ///< [IN] Application process to start.

    le_appProc_FaultAction_t action
        ///< [IN] Priority for the application process.
)
{
    AppProcContainer_t* appProcContainerPtr = le_ref_Lookup(AppProcMap, appProcRef);

    if (appProcContainerPtr == NULL)
    {
        LE_KILL_CLIENT("Invalid application process reference.");
        return;
    }

    FaultAction_t faultAction = FAULT_ACTION_NONE;

    switch (action)
    {
        case LE_APPPROC_FAULT_ACTION_IGNORE:
            faultAction = FAULT_ACTION_IGNORE;
            break;

        case LE_APPPROC_FAULT_ACTION_RESTART_PROC:
            faultAction = FAULT_ACTION_RESTART_PROC;
            break;

        case LE_APPPROC_FAULT_ACTION_RESTART_APP:
            faultAction = FAULT_ACTION_RESTART_APP;
            break;

        case LE_APPPROC_FAULT_ACTION_STOP_APP:
            faultAction = FAULT_ACTION_STOP_APP;
            break;

        case LE_APPPROC_FAULT_ACTION_REBOOT:
            faultAction = FAULT_ACTION_REBOOT;
            break;

        default:
            LE_KILL_CLIENT("Invalid fault action.");
            return;
    }

    app_SetFaultAction(appProcContainerPtr->procRef, faultAction);
}


//--------------------------------------------------------------------------------------------------
/**
 * Clears the application process's fault action and use either the configured fault action or the
 * default.
 */
//--------------------------------------------------------------------------------------------------
void le_appProc_ClearFaultAction
(
    le_appProc_RefRef_t appProcRef
        ///< [IN] Application process to start.
)
{
    AppProcContainer_t* appProcContainerPtr = le_ref_Lookup(AppProcMap, appProcRef);

    if (appProcContainerPtr == NULL)
    {
        LE_KILL_CLIENT("Invalid application process reference.");
        return;
    }

    app_SetFaultAction(appProcContainerPtr->procRef, FAULT_ACTION_NONE);
}


//--------------------------------------------------------------------------------------------------
/**
 * Starts the application process.  If the application was not running this function will start it
 * first.
 *
 * @return
 *      LE_OK if successful.
 *      LE_FAULT if there was some other error.
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_appProc_Start
(
    le_appProc_RefRef_t appProcRef
        ///< [IN] Application process to start.
)
{
    AppProcContainer_t* appProcContainerPtr = le_ref_Lookup(AppProcMap, appProcRef);

    if (appProcContainerPtr == NULL)
    {
        LE_KILL_CLIENT("Invalid application process reference.");
        return LE_FAULT;
    }

    // Start the app if it isn't already running.
    if (app_GetState(appProcContainerPtr->appContainerPtr->appRef) != APP_STATE_RUNNING)
    {
        le_result_t result = StartApp(appProcContainerPtr->appContainerPtr);

        if (result != LE_OK)
        {
            return LE_FAULT;
        }
    }

    // Start the process.
    return app_StartProc(appProcContainerPtr->procRef);
}


//--------------------------------------------------------------------------------------------------
/**
 * Deletes the application process object.
 */
//--------------------------------------------------------------------------------------------------
void le_appProc_Delete
(
    le_appProc_RefRef_t appProcRef
        ///< [IN] Application process to start.
)
{
    AppProcContainer_t* appProcContainerPtr = le_ref_Lookup(AppProcMap, appProcRef);

    if (appProcContainerPtr == NULL)
    {
        LE_KILL_CLIENT("Invalid application process reference.");
        return;
    }

    // Remove the safe reference.
    le_ref_DeleteRef(AppProcMap, appProcRef);

    app_DeleteProc(appProcContainerPtr->appContainerPtr->appRef, appProcContainerPtr->procRef);

    le_mem_Release(appProcContainerPtr);
}
