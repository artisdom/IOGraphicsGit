/*
 * Copyright (c) 1998-2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>

#define IOFRAMEBUFFER_PRIVATE
#include <IOKit/graphics/IOGraphicsPrivate.h>
#include <IOKit/graphics/IODisplay.h>
#include <IOKit/ndrvsupport/IOMacOSVideo.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/pwr_mgt/IOPM.h>

/*
    We further divide the actual display panel brightness levels into four
    IOKit power states which we export to our superclass.
    
    In the lowest state, the display is off.  This state consists of only one
    of the brightness levels, the lowest. In the next state it is in the dimmest
    usable state. The highest state consists of all the brightness levels.
    
    The display has no state or configuration or programming that would be
    saved/restored over power state changes, and the driver does not register
    with the superclass as an interested driver.
    
    This driver doesn't have much to do. It changes between the four power state
    brightnesses on command from the superclass, and it notices which brightness
    level the user has set.
    
    The only smart thing it does is keep track of which of the brightness levels
    the user has selected, and it never exceeds that on command from the display
    device object.
*/

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#define super IODisplay

OSDefineMetaClassAndStructors(IOBacklightDisplay, IODisplay)

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

enum {
    kIOBacklightDisplayMaxUsableState  = kIODisplayMaxPowerState - 1
};

#define kIOBacklightUserBrightnessKey   "IOBacklightUserBrightness"

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// probe
//

IOService * IOBacklightDisplay::probe( IOService * provider, SInt32 * score )
{
    IOFramebuffer *     framebuffer;
    IOService *         ret = 0;
    UInt32              displayType;
    uintptr_t           connectFlags;

    do
    {
        if (!gIOFBHaveBacklight)
            continue;

        if (!super::probe(provider, score))
            continue;

        framebuffer = (IOFramebuffer *) getConnection()->getFramebuffer();

        for (IOItemCount idx = 0; idx < framebuffer->getConnectionCount(); idx++)
        {
            if (kIOReturnSuccess != framebuffer->getAttributeForConnection(idx,
                    kConnectionFlags, &connectFlags))
                continue;
            if (0 == (kIOConnectionBuiltIn & connectFlags))
                continue;
            if (kIOReturnSuccess != framebuffer->getAppleSense(idx, NULL, NULL, NULL, &displayType))
                continue;
            if ((kPanelTFTConnect != displayType)
                    && (kGenericLCD != displayType)
                    && (kPanelFSTNConnect != displayType))
                continue;

            ret = this;         // yes, we will control the panel
            break;
        }
    }
    while (false);

    return (ret);
}

void IOBacklightDisplay::stop( IOService * provider )
{
    return (super::stop(provider));
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// initForPM
//
// This method overrides the one in IODisplay to do PowerBook-only
// power management of the display.

void IOBacklightDisplay::initPowerManagement( IOService * provider )
{
    static const IOPMPowerState ourPowerStates[kIODisplayNumPowerStates] = {
        // version,
        //   capabilityFlags,      outputPowerCharacter, inputPowerRequirement,
        { 1, 0,                                     0, 0,           0,0,0,0,0,0,0,0 },
        { 1, 0,                                     0, 0,           0,0,0,0,0,0,0,0 },
        { 1, IOPMDeviceUsable,                      0, kIOPMPowerOn, 0,0,0,0,0,0,0,0 },
        { 1, IOPMDeviceUsable | IOPMMaxPerformance, 0, kIOPMPowerOn, 0,0,0,0,0,0,0,0 }
        // staticPower, unbudgetedPower, powerToAttain, timeToAttain, settleUpTime,
        // timeToLower, settleDownTime, powerDomainBudget
    };

    OSNumber *     num;
    OSDictionary * displayParams;    

    displayParams = OSDynamicCast(OSDictionary, copyProperty(gIODisplayParametersKey));
    if (!displayParams
      || !getIntegerRange(displayParams, gIODisplayBrightnessKey,
                         &fCurrentBrightness, &fMinBrightness, &fMaxBrightness))
    {
        fMinBrightness = 0;
        fMaxBrightness = 255;
        fCurrentBrightness = fMaxBrightness;
    }

    if (fCurrentBrightness < (fMinBrightness + 2))
        fCurrentBrightness = fMinBrightness + 2;

    if ((num = OSDynamicCast(OSNumber,
                             getPMRootDomain()->getProperty(kIOBacklightUserBrightnessKey))))
    {
        fCurrentUserBrightness = num->unsigned32BitValue();
        if (fCurrentUserBrightness != fCurrentBrightness)
        {
            doIntegerSet(displayParams, gIODisplayBrightnessKey, fCurrentUserBrightness);
        }
    }
    else
        fCurrentUserBrightness = fCurrentBrightness;

    if (displayParams)
        displayParams->release();
        
    fMaxBrightnessLevel[0] = 0;
    fMaxBrightnessLevel[1] = fMinBrightness;
    fMaxBrightnessLevel[2] = fMinBrightness + 1;
    fMaxBrightnessLevel[3] = fMaxBrightness;

    // Check initial state of "DisplaySleepUsesDim"
    OSObject * obj = getPMRootDomain()->copyPMSetting(
                    const_cast<OSSymbol *>(gIOFBPMSettingDisplaySleepUsesDimKey));
    if ((num = OSDynamicCast(OSNumber, obj)) && (!num->unsigned32BitValue()))
    {
        // display stays at full power until display sleep
        fMaxBrightnessLevel[2] = fMaxBrightness;
    }
    if (obj)
        obj->release();

    // initialize superclass variables
    PMinit();
    // attach into the power management hierarchy
    provider->joinPMtree(this);

    // register ourselves with policy-maker (us)
    registerPowerDriver(this, (IOPMPowerState *) ourPowerStates, kIODisplayNumPowerStates);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// setPowerState
//

IOReturn IOBacklightDisplay::setPowerState( unsigned long powerState, IOService * whatDevice )
{
    IOReturn    ret = IOPMAckImplied;
    SInt32      value;

    if (powerState >= kIODisplayNumPowerStates)
        return (IOPMAckImplied);

    IOFramebuffer * framebuffer = (IOFramebuffer *) getConnection()->getFramebuffer();

    framebuffer->fbLock();

    if (isInactive())
    {
        framebuffer->fbUnlock();
        return (IOPMAckImplied);
    }

    fCurrentPowerState = powerState;

    value = fMaxBrightnessLevel[fCurrentPowerState];
    if (value > fCurrentUserBrightness)
        value = fCurrentUserBrightness;

    OSObject * obj;
    if ((!powerState) && (obj = copyProperty(kIOHibernatePreviewActiveKey, gIOServicePlane)))
    {
        obj->release();
    }
    else
    {
        //if(gIOFBSystemPower)
            setBrightness( value );
    }

    framebuffer->fbUnlock();

    return (ret);
}

void IOBacklightDisplay::makeDisplayUsable( void )
{
    if (kIODisplayMaxPowerState == fCurrentPowerState)
        setPowerState(fCurrentPowerState, this);
    super::makeDisplayUsable();
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

// Alter the backlight brightness by user request.

bool IOBacklightDisplay::doIntegerSet( OSDictionary * params,
                                       const OSSymbol * paramName, UInt32 value )
{
    if ((paramName == gIODisplayParametersCommitKey) 
        && (kIODisplayMaxPowerState != fCurrentPowerState))
        return (true);

    if (paramName != gIODisplayBrightnessKey)
        return (super::doIntegerSet(params, paramName, value));
    else
    {
        getPMRootDomain()->setProperty(kIOBacklightUserBrightnessKey, fCurrentUserBrightness, 32);
        fCurrentUserBrightness = value;
        return (setBrightness(value));
    }
}

bool IOBacklightDisplay::doUpdate( void )
{
    OSDictionary * displayParams;
    bool           ok = true;

    ok = super::doUpdate();

    if (fDisplayPMVars
        && (displayParams = OSDynamicCast(OSDictionary, copyProperty(gIODisplayParametersKey))))
    {
        setParameter(displayParams, gIODisplayBrightnessKey, fCurrentUserBrightness);	
        displayParams->release();
    }

    return (ok);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

// setBrightness

bool IOBacklightDisplay::setBrightness( SInt32 value )
{
    OSDictionary * displayParams;
    bool           ret = true;

    displayParams = OSDynamicCast(OSDictionary, copyProperty(gIODisplayParametersKey));
    if (!displayParams)
        return (false);

#if DEBUG
    if (!getControllerWorkLoop()->inGate())
        OSReportWithBacktrace("IOBacklightDisplay::setBrightness !inGate\n");
#endif

#if 0
    UInt32      newState;

    // We make sure the brightness is not above the maximum
    // brightness level of our current power state.  If it
    // is too high, we ask the device to raise power.

    for (newState = 0; newState < kIODisplayNumPowerStates; newState++)
    {
        if (value <= fMaxBrightnessLevel[newState])
            break;
    }
    if (newState >= kIODisplayNumPowerStates)
        return (false);

    if (newState != fCurrentPowerState)
    {
        // request new state
        if (IOPMNoErr != changePowerStateToPriv(newState))
            value = fCurrentBrightness;
    }

    if (value != fCurrentBrightness)
#endif
    {
        fCurrentBrightness = value;
        if (value <= fMinBrightness)
            value = 0;
        ret = super::doIntegerSet(displayParams, gIODisplayBrightnessKey, value);
    }

    displayParams->release();

    return (ret);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// maxCapabilityForDomainState
//
// This simple device needs only power.  If the power domain is supplying
// power, the display can go to its highest state.  If there is no power
// it can only be in its lowest state, which is off.

unsigned long IOBacklightDisplay::maxCapabilityForDomainState( IOPMPowerFlags domainState )
{
    if (domainState & IOPMPowerOn)
        return (kIODisplayMaxPowerState);
    else
        return (0);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// initialPowerStateForDomainState
//
// The power domain may be changing state.  If power is on in the new
// state, that will not affect our state at all.  If domain power is off,
// we can attain only our lowest state, which is off.

unsigned long IOBacklightDisplay::initialPowerStateForDomainState( IOPMPowerFlags domainState )
{
    if (domainState & IOPMPowerOn)
        return (kIODisplayMaxPowerState);
    else
        return (0);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// powerStateForDomainState
//
// The power domain may be changing state.  If power is on in the new
// state, that will not affect our state at all.  If domain power is off,
// we can attain only our lowest state, which is off.

unsigned long IOBacklightDisplay::powerStateForDomainState( IOPMPowerFlags domainState )
{
    if (domainState & IOPMPowerOn)
        return (kIODisplayMaxPowerState);
    else
        return (0);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

OSMetaClassDefineReservedUnused(IOBacklightDisplay, 0);
OSMetaClassDefineReservedUnused(IOBacklightDisplay, 1);
OSMetaClassDefineReservedUnused(IOBacklightDisplay, 2);
OSMetaClassDefineReservedUnused(IOBacklightDisplay, 3);
OSMetaClassDefineReservedUnused(IOBacklightDisplay, 4);
OSMetaClassDefineReservedUnused(IOBacklightDisplay, 5);
OSMetaClassDefineReservedUnused(IOBacklightDisplay, 6);
OSMetaClassDefineReservedUnused(IOBacklightDisplay, 7);
OSMetaClassDefineReservedUnused(IOBacklightDisplay, 8);
OSMetaClassDefineReservedUnused(IOBacklightDisplay, 9);

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class AppleBacklightDisplay : public IOBacklightDisplay
{
    OSDeclareDefaultStructors(AppleBacklightDisplay)

protected:
    IOInterruptEventSource * fDeferredEvents;
    uint8_t                  fClamshellSlept;

public:
    virtual bool start( IOService * provider );
    virtual void stop( IOService * provider );
    // IODisplay
    virtual void initPowerManagement( IOService * );
    virtual bool setBrightness( SInt32 value );
    virtual IOReturn framebufferEvent( IOFramebuffer * framebuffer,
                                       IOIndex event, void * info );

private:
    static void _deferredEvent( OSObject * target,
                                IOInterruptEventSource * evtSrc, int intCount );
};

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#undef super
#define super IOBacklightDisplay

OSDefineMetaClassAndStructors(AppleBacklightDisplay, IOBacklightDisplay)

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

bool AppleBacklightDisplay::start( IOService * provider )
{
    if (!super::start(provider))
        return (false);

    fDeferredEvents = IOInterruptEventSource::interruptEventSource(this, _deferredEvent);
    if (fDeferredEvents)
        getConnection()->getFramebuffer()->getControllerWorkLoop()->addEventSource(fDeferredEvents);

    getConnection()->getFramebuffer()->setProperty(kIOFBBuiltInKey, this, 0);

    return (true);
}

void AppleBacklightDisplay::stop( IOService * provider )
{
    if (fDeferredEvents)
    {
        getConnection()->getFramebuffer()->getControllerWorkLoop()->removeEventSource(fDeferredEvents);
        fDeferredEvents->release();
        fDeferredEvents = 0;
    }

    return (super::stop(provider));
}

void AppleBacklightDisplay::initPowerManagement( IOService * provider )
{
    super::initPowerManagement( provider );
}

void AppleBacklightDisplay::_deferredEvent( OSObject * target,
        IOInterruptEventSource * evtSrc, int intCount )
{
    AppleBacklightDisplay * self = (AppleBacklightDisplay *) target;

    // may be in the right power state already, but wrong brightness because
    // of the clamshell state at setPowerState time.
    if (self->fCurrentPowerState)
    {
        SInt32 brightness = self->fMaxBrightnessLevel[self->fCurrentPowerState];
        if (brightness > self->fCurrentUserBrightness)
            brightness = self->fCurrentUserBrightness;
        self->setBrightness( brightness );
    }
}

IOReturn AppleBacklightDisplay::framebufferEvent( IOFramebuffer * framebuffer,
        IOIndex event, void * info )
{
    SInt32                  value;

    if ((kIOFBNotifyDidWake == event) && (info))
    {
        value = fMaxBrightnessLevel[kIODisplayMaxPowerState];
        if (value > fCurrentUserBrightness)
            value = fCurrentUserBrightness;

        setBrightness( value );
    }
    else if (kIOFBNotifyClamshellChange == event)
    {
        if (kOSBooleanTrue == info)
        {
#if LCM_HWSLEEP
            fConnection->getFramebuffer()->changePowerStateTo(0);
#endif
            setBrightness(0);
            fClamshellSlept = true;
        }
        else if (fClamshellSlept)
        {
            fClamshellSlept = false;
        }
        // may be in the right power state already, but wrong brightness because
        // of the clamshell state at setPowerState time.
        if (fDeferredEvents)
            fDeferredEvents->interruptOccurred(0, 0, 0);
    }
    else if (kIOFBNotifyProbed == event)
    {
        if (fDeferredEvents)
            fDeferredEvents->interruptOccurred(0, 0, 0);
    }
    else if (kIOFBNotifyDisplayDimsChange == event)
    {
        UInt16 newLevel;
        if (info)
        {
            // display will dim before sleep
            newLevel = fMinBrightness + 1;
        }
        else
        {
            // display will not dim before sleep
            newLevel = fMaxBrightness;
        }

        // Set the new brightness value for the 2nd highest power state
        // either full brightness or dim
        if (newLevel != fMaxBrightnessLevel[2])
        {
            fMaxBrightnessLevel[2] = newLevel;
            if (2 == fCurrentPowerState)
                setBrightness(newLevel);
        }
    }

    return (super::framebufferEvent( framebuffer, event, info ));
}

bool AppleBacklightDisplay::setBrightness( SInt32 value )
{
#if DEBUG
    IOLog("brightness[%d,%d] %d\n", (int) fCurrentPowerState, (int) gIOFBLastClamshellState, (int) value);
#endif
        if (fClamshellSlept)
            value = 0;

    return (super::setBrightness(value));
}
