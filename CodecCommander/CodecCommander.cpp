/*
 *  Released under "The GNU General Public License (GPL-2.0)"
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or (at your 
 *  option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but 
 *  WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 *  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along 
 *  with this program; if not, write to the Free Software Foundation, Inc., 
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#include "CodecCommander.h"

//REVIEW: avoids problem with Xcode 5.1.0 where -dead_strip eliminates these required symbols
#include <libkern/OSKextLib.h>
void* _org_rehabman_dontstrip_[] =
{
	(void*)&OSKextGetCurrentIdentifier,
	(void*)&OSKextGetCurrentLoadTag,
	(void*)&OSKextGetCurrentVersionString,
};

// Define variables for EAPD state updating
UInt8 eapdCapableNodes[MAX_EAPD_NODES];

bool eapdPoweredDown, coldBoot;
UInt8 hdaCurrentPowerState, hdaPrevPowerState;

// Define usable power states
static IOPMPowerState powerStateArray[ kPowerStateCount ] =
{
    { 1,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,kIOPMDeviceUsable, kIOPMDoze, kIOPMDoze, 0,0,0,0,0,0,0,0 },
    { 1,kIOPMDeviceUsable, IOPMPowerOn, IOPMPowerOn, 0,0,0,0,0,0,0,0 }
};

OSDefineMetaClassAndStructors(CodecCommander, IOService)

/******************************************************************************
 * CodecCommander::init - parse kernel extension Info.plist
 ******************************************************************************/
bool CodecCommander::init(OSDictionary *dictionary)
{
    DEBUG_LOG("CodecCommander: Initializing\n");
    
    if (!super::init(dictionary))
        return false;
		
	mConfiguration = new Configuration(dictionary);
	
    mWorkLoop = NULL;
    mTimer = NULL;
    
    eapdPoweredDown = true;
    coldBoot = true; // assume booting from cold since hibernate is broken on most hacks
    hdaCurrentPowerState = 0x0; // assume hda codec has no power at cold boot
    hdaPrevPowerState = hdaCurrentPowerState; //and previous state was the same
	
    return true;
}

/******************************************************************************
 * CodecCommander::start - start kernel extension and init PM
 ******************************************************************************/
bool CodecCommander::start(IOService *provider)
{
    IOLog("CodecCommander: Version 2.2.1 starting.\n");

    if (!provider || !super::start(provider))
	{
		DEBUG_LOG("CodecCommander: Error loading kernel extension.\n");
		return false;
	}
    
    // Retrieve HDEF device from IORegistry
	IORegistryEntry *hdaDeviceEntry = IORegistryEntry::fromPath(mConfiguration->getHDADevicePath());
	
    if (hdaDeviceEntry != NULL)
    {
		mIntelHDA = new IntelHDA(hdaDeviceEntry, PIO, mConfiguration->getCodecNumber());
		OSSafeRelease(hdaDeviceEntry);
    }
    else
    {
        DEBUG_LOG("CodecCommander: Device \"%s\" is unreachable, start aborted.\n", mConfiguration->getHDADevicePath());
        return false;
    }
	
	if (mConfiguration->getUpdateNodes())
	{
		IOSleep(mConfiguration->getSendDelay()); // need to wait a bit until codec can actually respond to immediate verbs
		int k = 0; // array index
	
		// Fetch Pin Capabilities from the range of nodes
		DEBUG_LOG("CodecCommander: Getting EAPD supported node list (limited to %d)\n", MAX_EAPD_NODES);
	
		for (int nodeId = mIntelHDA->getStartingNode(); nodeId <= mIntelHDA->getTotalNodes(); nodeId++)
		{
			UInt32 response = mIntelHDA->sendCommand(nodeId, HDA_VERB_GET_PARAM, HDA_PARM_PINCAP);
		
			if (response == -1)
			{
				DEBUG_LOG("CodecCommander: Failed to retrieve pin capabilities for node 0x%02x.\n", nodeId);
				continue;
			}

			// if bit 16 is set in pincap - node supports EAPD
			if (HDA_PINCAP_IS_EAPD_CAPABLE(response))
			{
				eapdCapableNodes[k] = nodeId;
				k++;
				IOLog("CodecCommander: NID=0x%02x supports EAPD, will update state after sleep\n", nodeId);
			}
		}
	
/*
		IOLog("CodecCommander:: Set 0x0A result: 0x%08x\n", mIntelHDA->sendCommand(0x0A, HDA_VERB_SET_AMP_GAIN, HDA_PARM_AMP_GAIN_SET(0x80, 0, 1, 1, 1, 0, 1)));
		IOLog("CodecCommander:: Set 0x0B result: 0x%08x\n", mIntelHDA->sendCommand(0x0B, HDA_VERB_SET_AMP_GAIN, HDA_PARM_AMP_GAIN_SET(0x80, 0, 1, 1, 1, 0, 1)));
		IOLog("CodecCommander:: Set 0x0C result: 0x%08x\n", mIntelHDA->sendCommand(0x0C, HDA_VERB_SET_AMP_GAIN, HDA_PARM_AMP_GAIN_SET(0x80, 0, 1, 1, 1, 0, 1)));
	
		for (int nodeId = mIntelHDA->getStartingNode(); nodeId <= mIntelHDA->getTotalNodes(); nodeId++)
		{
			UInt16 payload = HDA_PARM_AMP_GAIN_GET(0, 1, 1);//AC_AMP_GET_OUTPUT | AC_AMP_GET_LEFT;
			UInt32 response = mIntelHDA->sendCommand(nodeId, HDA_VERB_GET_AMP_GAIN, payload);
		
			if (response == -1)
			{
				DEBUG_LOG("Failed to retrieve amp gain settings for node 0x%02x.\n", nodeId);
				continue;
			}
		
			IOLog("CodecCommander:: [Amp Gain] Node: 0x%04x, Response: 0x%08x\n", nodeId, response);
		}
 */
	}
	
	// Execute any custom commands registered for initialization
	handleStateChange(kStateInit);
	
    // notify about extra feature requests
    if (mConfiguration->getCheckInfinite())
        DEBUG_LOG("CodecCommander: Infinite workloop requested, will start now!\n");
    
    // init power state management & set state as PowerOn
    PMinit();
    registerPowerDriver(this, powerStateArray, kPowerStateCount);
	provider->joinPMtree(this);
    
    // setup workloop and timer
    mWorkLoop = IOWorkLoop::workLoop();
    mTimer = IOTimerEventSource::timerEventSource(this,
                                                  OSMemberFunctionCast(IOTimerEventSource::Action, this,
                                                  &CodecCommander::onTimerAction));
    if (!mWorkLoop || !mTimer)
        stop(provider);;
    
    if (mWorkLoop->addEventSource(mTimer) != kIOReturnSuccess)
        stop(provider);
    
	this->registerService(0);
    return true;
}

/******************************************************************************
 * CodecCommander::stop & free - stop and free kernel extension
 ******************************************************************************/
void CodecCommander::stop(IOService *provider)
{
    DEBUG_LOG("CodecCommander: Stopping...\n");
    
    // if workloop is active - release it
    mTimer->cancelTimeout();
    mWorkLoop->removeEventSource(mTimer);
    OSSafeReleaseNULL(mTimer);// disable outstanding calls
    OSSafeReleaseNULL(mWorkLoop);

	// Free IntelHDA engine
	mIntelHDA->~IntelHDA();
	
    PMstop();
    super::stop(provider);
}

#ifdef DEBUG
void CodecCommander::free(void)
{
	super::free();
}
#endif

/******************************************************************************
 * CodecCommander::parseCodecPowerState - get codec power state from IOReg
 ******************************************************************************/
void CodecCommander::parseCodecPowerState()
{
	// monitor power state of hda audio codec
	IORegistryEntry *hdaDriverEntry = IORegistryEntry::fromPath(mConfiguration->getHDADriverPath());
	
	if (hdaDriverEntry != NULL)
	{
		OSNumber *powerState = OSDynamicCast(OSNumber, hdaDriverEntry->getProperty("IOAudioPowerState"));

		if (powerState != NULL)
		{
			hdaCurrentPowerState = powerState->unsigned8BitValue();

			// if hda codec changed power state
			if (hdaCurrentPowerState != hdaPrevPowerState)
			{
				DEBUG_LOG("CodecCommander: cc - power state transition from %d to %d recorded\n", hdaPrevPowerState, hdaCurrentPowerState);
				
				// store current power state as previous state for next workloop cycle
				hdaPrevPowerState = hdaCurrentPowerState;
				// notify about codec power loss state
				if (hdaCurrentPowerState == 0x0)
				{
					DEBUG_LOG("CodecCommander: HDA codec lost power\n");
					handleStateChange(kStateSleep); // power down EAPDs properly
					eapdPoweredDown = true;
					coldBoot = false; //codec entered fugue state or sleep - no longer a cold boot
				}
			}
		}
		else
			DEBUG_LOG("CodecCommander: IOAudioPowerState unknown\n");
		
		hdaDriverEntry->release();
	}
	else
		DEBUG_LOG("CodecCommander: %s is unreachable\n", mConfiguration->getHDADriverPath());
}

/******************************************************************************
 * CodecCommander::onTimerAction - repeats the action each time timer fires
 ******************************************************************************/
void CodecCommander::onTimerAction()
{
    // check if hda codec is powered - we are monitoring ocurrences of fugue state
    parseCodecPowerState();
	
    // if no power after semi-sleep (fugue) state and power was restored - set EAPD bit
	if (eapdPoweredDown && hdaCurrentPowerState != 0x0)
    {
        DEBUG_LOG("CodecCommander: cc: --> hda codec power restored\n");
		handleStateChange(kStateWake);
    }
    
    mTimer->setTimeoutMS(mConfiguration->getInterval());
}

/******************************************************************************
 * CodecCommander::handleStateChange - handles transitioning from one state to another, i.e. sleep --> wake
 ******************************************************************************/
void CodecCommander::handleStateChange(CodecCommanderState newState)
{
	switch (newState)
	{
		case kStateSleep:
			if (mConfiguration->getUpdateNodes())
				setEAPD(0x0);
			
			customCommands(newState);
			break;
		case kStateWake:
			if (mConfiguration->getUpdateNodes())
				setEAPD(0x02);
			
			customCommands(newState);
			break;
		case kStateInit:
			customCommands(newState);
			break;
	}
	
}

/******************************************************************************
 * CodecCommander::customCommands - fires all configured custom commands
 ******************************************************************************/
void CodecCommander::customCommands(CodecCommanderState newState)
{
	CustomCommand* customCommands = mConfiguration->getCustomCommands();
	
	for (int i = 0; i < MAX_CUSTOM_COMMANDS; i++)
	{
		CustomCommand customCommand = customCommands[i];
		
		if (customCommand.Command == 0)
			break;
		
		if ((customCommand.OnInit == (newState == kStateInit)) ||
		   (customCommand.OnWake == (newState == kStateWake)) ||
		   (customCommand.OnSleep == (newState == kStateSleep)))
		{
			DEBUG_LOG("CodecCommander: cc: --> custom command 0x%08x\n", customCommand.Command);
			mIntelHDA->sendCommand(customCommand.Command);
		}
	}
}

/******************************************************************************
 * CodecCommander::setOutputs - set EAPD status bit on SP/HP
 ******************************************************************************/
void CodecCommander::setEAPD(UInt8 logicLevel)
{
    // delay by at least 100ms, otherwise first immediate command won't be received
    // some codecs will produce loud pop when EAPD is enabled too soon, need custom delay until codec inits
    if (mConfiguration->getSendDelay() < 100)
        IOSleep(100);
    else
        IOSleep(mConfiguration->getSendDelay());
	
    // for nodes supporting EAPD bit 1 in logicLevel defines EAPD logic state: 1 - enable, 0 - disable
    for (int i = 0; i < MAX_EAPD_NODES; i++)
	{
        if (eapdCapableNodes[i] != 0)
			mIntelHDA->sendCommand(eapdCapableNodes[i], HDA_VERB_EAPDBTL_SET, logicLevel);
    }
	
	eapdPoweredDown = false;
}

/******************************************************************************
 * CodecCommander::performCodecReset - reset function group and set power to D3
 *****************************************************************************/

void CodecCommander::performCodecReset()
{
    /*
     Reset is created by sending two Function Group resets, potentially separated 
     by an undefined number of idle frames, but no other valid commands.
     This Function Group “Double” reset shall do a full initialization and reset 
     most settings to their power on defaults.
     
     This function can be used to reset codec on dekstop boards, for example H87-HD3,
     to overcome audio loss and jack sense problem after sleep with AppleHDA v2.6.0+
     */

    if (!coldBoot)
	{
        DEBUG_LOG("CodecCommander: cc: --> resetting codec\n");
		mIntelHDA->sendCommand(1, HDA_VERB_RESET, HDA_PARM_NULL);
        IOSleep(100); // define smaller delays ????
		mIntelHDA->sendCommand(1, HDA_VERB_RESET, HDA_PARM_NULL);
        IOSleep(100);
        // forcefully set power state to D3
		mIntelHDA->sendCommand(1, HDA_VERB_SET_PSTATE, HDA_PARM_PS_D3_HOT);
		
        eapdPoweredDown = true;
        DEBUG_LOG("CodecCommander: cc: --> hda codec power restored\n");
    }
}

/******************************************************************************
 * CodecCommander::setPowerState - set active power state
 ******************************************************************************/
IOReturn CodecCommander::setPowerState(unsigned long powerStateOrdinal, IOService *policyMaker)
{

    if (kPowerStateSleep == powerStateOrdinal)
	{
        DEBUG_LOG("CodecCommander: cc: --> asleep\n");
		handleStateChange(kStateSleep); // set EAPD logic level 0 to cause EAPD to power off properly
        eapdPoweredDown = true;  // now it's powered down for sure
        coldBoot = false;
	}
	else if (kPowerStateNormal == powerStateOrdinal)
	{
        DEBUG_LOG("CodecCommander: cc: --> awake\n");

        // issue codec reset at wake and cold boot
        performCodecReset();
        
        if (eapdPoweredDown)
            // set EAPD bit at wake or cold boot
			handleStateChange(kStateWake);

        // if infinite checking requested
        if (mConfiguration->getCheckInfinite())
		{
            // if checking infinitely then make sure to delay workloop
            if (coldBoot)
                mTimer->setTimeoutMS(20000); // create a nasty 20sec delay for AudioEngineOutput to initialize
            // if we are waking it will be already initialized
            else
                mTimer->setTimeoutMS(100); // so fire timer for workLoop almost immediately

            DEBUG_LOG("CodecCommander: cc: --> workloop started\n");
        }
    }
    
    return IOPMAckImplied;
}

/******************************************************************************
 * CodecCommander::executeCommand - Execute an external command
 ******************************************************************************/
UInt32 CodecCommander::executeCommand(UInt32 command)
{
	if (mIntelHDA)
		return mIntelHDA->sendCommand(command);
	
	return -1;
}
