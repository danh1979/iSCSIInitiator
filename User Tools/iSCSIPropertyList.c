/*!
 * @author		Nareg Sinenian
 * @file		iSCSIPropertyList.c
 * @version		1.0
 * @copyright	(c) 2013-2015 Nareg Sinenian. All rights reserved.
 * @brief		Provides user-space library functions to read and write
 *              daemon configuration property list
 */

#include "iSCSIPropertyList.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFPreferences.h>
#include <CoreFoundation/CFPropertyList.h>

/*! A cached version of the targets dictionary. */
CFMutableDictionaryRef targetsCache = NULL;

/*! Flag that indicates whether the targets cache was modified. */
Boolean targetNodesCacheModified = false;

/*! A cached version of the discovery dictionary. */
CFMutableDictionaryRef discoveryCache = NULL;

/*! Flag that indicates whether the discovery cache was modified. */
Boolean discoveryCacheModified = false;

/*! A cached version of the initiator dictionary. */
CFMutableDictionaryRef initiatorCache = NULL;

/*! Flag that indicates whether the initiator cache was modified. */
Boolean initiatorNodeCacheModified = false;

/*! App ID. */
CFStringRef kiSCSIPKAppId = CFSTR(CF_PREFERENCES_APP_ID);

/*! Preference key name for iSCSI targets dictionary (holds all targets). */
CFStringRef kiSCSIPKTargetsKey = CFSTR("Target Nodes");

/*! Preference key name for iSCSI target dictionary (specific to each). */
CFStringRef kiSCSIPKTargetKey = CFSTR("Target Data");

/*! Preference key name for iSCSI discovery dictionary. */
CFStringRef kiSCSIPKDiscoveryKey = CFSTR("SendTargets Discovery");

/*! Preference key name for iSCSI initiator dictionary. */
CFStringRef kiSCSIPKInitiatorKey = CFSTR("Initiator Node");


/*! Preference key name for iSCSI session configuration (specific to each target). */
CFStringRef kiSCSIPKSessionCfgKey = CFSTR("Session Configuration");

/*! Preference key name for iSCSI portals dictionary (specific to each target). */
CFStringRef kiSCSIPKPortalsKey = CFSTR("Portals");


/*! Preference key name for iSCSI portal dictionary (specific to each). */
CFStringRef kiSCSIPKPortalKey = CFSTR("Portal Data");

/*! Preference key name for iSCSI connection configuration information. */
CFStringRef kiSCSIPKConnectionCfgKey = CFSTR("Connection Configuration");

/*! Preference key name for iSCSI authentication. */
CFStringRef kiSCSIPKAuthKey = CFSTR("Authentication");

/*! Preference key value for no authentication. */
CFStringRef kiSCSIPVAuthNone = CFSTR("None");

/*! Preference key value for CHAP authentication. */
CFStringRef kiSCSIPVAuthCHAP = CFSTR("CHAP");

/*! Preference key name for iSCSI initiator name. */
CFStringRef kiSCSIPKInitiatorIQN = CFSTR("Name");

/*! Preference key name for iSCSI initiator alias. */
CFStringRef kiSCSIPKInitiatorAlias = CFSTR("Alias");

/*! The iSCSI service name to use when storing CHAP information in the
 *  OS X keychain. */
CFStringRef kiSCSISecCHAPServiceName = CFSTR("iSCSI CHAP");



/*! Helper function. Writes a shared secret associated with a particular
 *  iSCSI node (either initiator or target) to the system keychain. An entry
 *  for the node is created if it does not exist. If it does exist, the shared
 *  secret for is updated.
 *  @param nodeIQN the iSCSI qualified name of the target or initiator. 
 *  @param userIQN the
 *  @param sharedSecret the shared secret to store. */
void iSCSIPLSetCHAPSecretForNode(CFStringRef nodeIQN,
                                 CFStringRef user,
                                 CFStringRef sharedSecret)
{
    SecKeychainRef sysKeychain = NULL;
    SecKeychainItemRef item = NULL;
    OSStatus status;
    SecAccessRef initialAccess = NULL;

    // Get the system keychain and unlock it (prompts user if required)
    status = SecKeychainCopyDomainDefault(kSecPreferencesDomainSystem,&sysKeychain);

    if(status == errSecSuccess)
        status = SecKeychainUnlock(sysKeychain,0,NULL,false);

    if(status == errSecSuccess)
        status = SecAccessCreate(CFSTR(""),0,&initialAccess);

    // Add the shared secret to the keychain
    if(status == errSecSuccess) {

        SecKeychainAttribute attributes[] = {
            {kSecLabelItemAttr,(UInt32)CFStringGetLength(nodeIQN),(void *)CFStringGetCStringPtr(nodeIQN,kCFStringEncodingASCII)},
            {kSecAccountItemAttr,(UInt32)CFStringGetLength(user),(void *)CFStringGetCStringPtr(user,kCFStringEncodingASCII)},
            {kSecServiceItemAttr,(UInt32)CFStringGetLength(kiSCSISecCHAPServiceName),(void *)CFStringGetCStringPtr(kiSCSISecCHAPServiceName,kCFStringEncodingASCII)},
            {kSecDescriptionItemAttr,(UInt32)CFStringGetLength(kiSCSISecCHAPServiceName),(void *)CFStringGetCStringPtr(kiSCSISecCHAPServiceName,kCFStringEncodingASCII)}
        };

        SecKeychainAttributeList attrList = { sizeof(attributes)/sizeof(attributes[0]), attributes };

        SecKeychainItemCreateFromContent(
                kSecGenericPasswordItemClass,
                &attrList,
                (UInt32)CFStringGetLength(sharedSecret),
                (const void *)CFStringGetCStringPtr(sharedSecret,kCFStringEncodingASCII),
                sysKeychain,
                initialAccess,
                &item);
    }
}

/*! Helper function. Copies a shared secret associated with a particular
 *  iSCSI node (either initiator or target) to the system keychain.
 *  @param nodeIQN the iSCSI qualified name of the target or initiator.
 *  @return the shared secret for the specified node. */
errno_t iSCSIPLCopyCHAPSecretForNode(CFStringRef nodeIQN,
                                     CFStringRef * user,
                                     CFStringRef * sharedSecret)
{
    SecKeychainRef sysKeychain;
    OSStatus status;
    CFDictionaryRef resultsDict = NULL;

    // Get the system keychain and unlock it (prompts user if required)
    status = SecKeychainCopyDomainDefault(kSecPreferencesDomainSystem,&sysKeychain);

    if(status == errSecSuccess) {

        // Setup query dictionary to find the CHAP user and shared key
        const void * queryKeys[] = {
            kSecClass,
            kSecReturnAttributes,
            kSecReturnData,
            kSecAttrLabel
        };

        const void * queryValues[] = {
            kSecClassGenericPassword,
            kCFBooleanTrue,
            kCFBooleanTrue,
            nodeIQN
        };

        CFDictionaryRef queryDict = CFDictionaryCreate(
            kCFAllocatorDefault,queryKeys,queryValues,4,
            &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);

        status = SecItemCopyMatching(queryDict,(CFTypeRef *)&resultsDict);
    }

    // Extract CHAP user and shared secret
    if(status == errSecSuccess)
    {
        // Extract the shared secret from the results dictionary
        CFDataRef secretData = CFDictionaryGetValue(resultsDict,kSecValueData);

        *sharedSecret = CFStringCreateFromExternalRepresentation(
            kCFAllocatorDefault,secretData,kCFStringEncodingASCII);

        *user = CFStringCreateCopy(kCFAllocatorDefault,CFDictionaryGetValue(resultsDict,kSecAttrAccount));

    }

    return status;
}

/*! Retrieves a mutable dictionary for the specified key. 
 *  @param key the name of the key, which can be either kiSCSIPKTargetsKey,
 *  kiSCSIPKDiscoveryKey, or kiSCSIPKInitiatorKey.
 *  @return mutable dictionary with list of properties for the specified key. */
CFMutableDictionaryRef iSCSIPLCopyPropertyDict(CFStringRef key)
{
    // Retrieve the desired property from the property list
    CFDictionaryRef propertyList = CFPreferencesCopyValue(key,kiSCSIPKAppId,
                                                          kCFPreferencesAnyUser,
                                                          kCFPreferencesCurrentHost);
    
    if(!propertyList)
        return NULL;
    
    // Create a deep copy to make the dictionary mutable
    CFMutableDictionaryRef mutablePropertyList = (CFMutableDictionaryRef)CFPropertyListCreateDeepCopy(
        kCFAllocatorDefault,propertyList,kCFPropertyListMutableContainersAndLeaves);
    
    // Release original retrieved property
    CFRelease(propertyList);
    return mutablePropertyList;
}

/*! Creates a mutable dictionary for the targets key.
 *  @return a mutable dictionary for the targets key. */
CFMutableDictionaryRef iSCSIPLCreateTargetsDict()
{
    return CFDictionaryCreateMutable(kCFAllocatorDefault,
                                     0,
                                     &kCFTypeDictionaryKeyCallBacks,
                                     &kCFTypeDictionaryValueCallBacks);
}

/*! Creates a mutable dictionary for the discovery key.
 *  @return a mutable dictionary for the discovery key. */
CFMutableDictionaryRef iSCSIPLCreateDiscoveryDict()
{
    return CFDictionaryCreateMutable(kCFAllocatorDefault,
                                     0,
                                     &kCFTypeDictionaryKeyCallBacks,
                                     &kCFTypeDictionaryValueCallBacks);
}



/*! Creates a mutable dictionary for the initiator key.
 *  @return a mutable dictionary for the initiator key. */
CFMutableDictionaryRef iSCSIPLCreateInitiatorDict()
{
    CFStringRef kInitiatorIQN = CFSTR("");
    CFStringRef kInitiatorAlias = CFSTR("");
    
    CFStringRef keys[] = { kiSCSIPKInitiatorAlias,kiSCSIPKInitiatorIQN };
    CFStringRef values[] = { kInitiatorAlias,kInitiatorIQN };
    
    CFDictionaryRef initiatorPropertylist = CFDictionaryCreate(kCFAllocatorDefault,
                                                               (const void **)keys,
                                                               (const void **)values,
                                                               sizeof(keys)/sizeof(CFStringRef),
                                                               &kCFTypeDictionaryKeyCallBacks,
                                                               &kCFTypeDictionaryValueCallBacks);
//// TODO: fix memory leak
    return CFDictionaryCreateMutableCopy(kCFAllocatorDefault,0,initiatorPropertylist);
}

CFMutableDictionaryRef iSCSIPLGetInitiator(Boolean createIfMissing)
{
    if(createIfMissing && !initiatorCache)
        initiatorCache = iSCSIPLCreateInitiatorDict();

    return initiatorCache;
}

CFMutableDictionaryRef iSCSIPLGetTargets(Boolean createIfMissing)
{
    if(createIfMissing && !targetsCache)
        targetsCache = iSCSIPLCreateTargetsDict();

    return targetsCache;
}

CFMutableDictionaryRef iSCSIPLGetTargetInfo(CFStringRef targetIQN,
                                            Boolean createIfMissing)
{
    // Get list of targets
    CFMutableDictionaryRef targetsList = iSCSIPLGetTargets(createIfMissing);
    
    if(targetsList)
    {
        if(createIfMissing && CFDictionaryGetCountOfKey(targetsList,targetIQN) == 0)
        {
            CFMutableDictionaryRef targetInfo = CFDictionaryCreateMutable(
                kCFAllocatorDefault,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
            
            CFDictionarySetValue(targetsList,targetIQN,targetInfo);
            CFRelease(targetInfo);
        }
        
        return (CFMutableDictionaryRef)CFDictionaryGetValue(targetsList,targetIQN);
    }
    return NULL;
}


CFMutableDictionaryRef iSCSIPLGetPortalsList(CFStringRef targetIQN,
                                             Boolean createIfMissing)
{
    // Get the target information dictionary
    CFMutableDictionaryRef targetInfo = iSCSIPLGetTargetInfo(targetIQN,createIfMissing);

    
    if(targetInfo)
    {
        if(createIfMissing && CFDictionaryGetCountOfKey(targetInfo,kiSCSIPKPortalsKey) == 0)
        {
            CFMutableDictionaryRef portalsList = CFDictionaryCreateMutable(
                kCFAllocatorDefault,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
            
            CFDictionarySetValue(targetInfo,kiSCSIPKPortalsKey,portalsList);
            CFRelease(portalsList);
        }
        return (CFMutableDictionaryRef)CFDictionaryGetValue(targetInfo,kiSCSIPKPortalsKey);
    }
    return NULL;
}

CFMutableDictionaryRef iSCSIPLGetPortalInfo(CFStringRef targetIQN,
                                            CFStringRef portalAddress,
                                            Boolean createIfMissing)
{
    // Get list of portals for this target
    CFMutableDictionaryRef portalsList = iSCSIPLGetPortalsList(targetIQN,createIfMissing);

    // If list was valid (target exists), get the dictionary with portal
    // information, including portal (address) information, configuration,
    // and authentication sub-dictionaries
    if(portalsList)
    {
       if(createIfMissing && CFDictionaryGetCountOfKey(portalsList,portalAddress) == 0)
       {
           CFMutableDictionaryRef portalInfo = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                                         0,
                                                                         &kCFTypeDictionaryKeyCallBacks,
                                                                         &kCFTypeDictionaryValueCallBacks);
           
           CFDictionarySetValue(portalInfo,kiSCSIPKAuthKey,CFSTR(""));
           CFDictionarySetValue(portalInfo,kiSCSIPKConnectionCfgKey,CFSTR(""));
           CFDictionarySetValue(portalInfo,kiSCSIPKPortalKey,CFSTR(""));
           
           CFDictionarySetValue(portalsList,portalAddress,portalInfo);
           CFRelease(portalInfo);

       }
       return (CFMutableDictionaryRef)CFDictionaryGetValue(portalsList,portalAddress);
    }
    return NULL;
}

iSCSISessionConfigRef iSCSIPLCopySessionConfig(CFStringRef targetIQN)
{
    // Get the target information dictionary
    CFMutableDictionaryRef targetInfo = iSCSIPLGetTargetInfo(targetIQN,false);
    
    if(targetInfo)
        return iSCSISessionConfigCreateWithDictionary(CFDictionaryGetValue(targetInfo,kiSCSIPKSessionCfgKey));
    
    return NULL;
}

void iSCSIPLSetSessionConfig(CFStringRef targetIQN,
                                      iSCSISessionConfigRef sessCfg)
{
    // Get the target information dictionary
    CFMutableDictionaryRef targetInfo = iSCSIPLGetTargetInfo(targetIQN,true);
    CFDictionaryRef sessCfgDict = iSCSISessionConfigCreateDictionary(sessCfg);
    
    CFDictionarySetValue(targetInfo,kiSCSIPKSessionCfgKey,sessCfgDict);
    CFRelease(sessCfgDict);
    
    targetNodesCacheModified = true;
}

iSCSIPortalRef iSCSIPLCopyPortalForTarget(CFStringRef targetIQN,
                                          CFStringRef portalAddress)
{
    // Get the dictionary containing information about the portal
    CFMutableDictionaryRef portalInfo = iSCSIPLGetPortalInfo(targetIQN,portalAddress,false);
    
    if(portalInfo)
        return iSCSIPortalCreateWithDictionary(CFDictionaryGetValue(portalInfo,kiSCSIPKPortalKey));
    
    return NULL;
}

iSCSITargetRef iSCSIPLCopyTarget(CFStringRef targetIQN)
{
    iSCSITargetRef target = NULL;

    // Get the dictionary containing information about the target
    CFDictionaryRef targetInfo = iSCSIPLGetTargetInfo(targetIQN,false);

    if(targetInfo)
        target = CFDictionaryGetValue(targetInfo,kiSCSIPKTargetKey);

    return target;
}

iSCSIConnectionConfigRef iSCSIPLCopyConnectionConfig(CFStringRef targetIQN,CFStringRef portalAddress)
{
    // Get the dictionary containing information about the portal
    CFMutableDictionaryRef portalInfo = iSCSIPLGetPortalInfo(targetIQN,portalAddress,false);
    
    if(portalInfo)
        return iSCSIConnectionConfigCreateWithDictionary(CFDictionaryGetValue(portalInfo,kiSCSIPKConnectionCfgKey));

    return NULL;
}

/*! Copies an authentication object associated with a particular target.
 *  @param targetIQN the target name.
 *  @return the authentication object. */
iSCSIAuthRef iSCSIPLCopyAuthenticationForTarget(CFStringRef targetIQN)
{
    CFMutableDictionaryRef targetInfo = iSCSIPLGetTargetInfo(targetIQN,false);

    iSCSIAuthRef auth = NULL;

    if(targetInfo) {
        CFStringRef authMethod = CFDictionaryGetValue(targetInfo,kiSCSIPKAuthKey);

        if(CFStringCompare(authMethod,kiSCSIPVAuthCHAP,0) == kCFCompareEqualTo) {
            CFStringRef user, sharedSecret;
            if(iSCSIPLCopyCHAPSecretForNode(targetIQN,&user,&sharedSecret) == errSecSuccess)
                auth = iSCSIAuthCreateCHAP(user,sharedSecret);
            else
                auth = iSCSIAuthCreateNone();
        }
        else
            auth = iSCSIAuthCreateNone();
    }
    return auth;
}

/*! Sets an authentication object associated with a particular target.
 *  @param targetIQN the target name.
 *  @param auth the connection configuration object to set. */
void iSCSIPLSetAuthenticationForTarget(CFStringRef targetIQN,
                                       iSCSIAuthRef targetAuth)
{
    CFMutableDictionaryRef targetInfo = iSCSIPLGetTargetInfo(targetIQN,false);

    if(iSCSIAuthGetMethod(targetAuth) == kiSCSIAuthMethodNone)
        CFDictionarySetValue(targetInfo,kiSCSIPKAuthKey,kiSCSIPVAuthNone);
    else {
        CFStringRef user, sharedSecret;
        CFDictionarySetValue(targetInfo,kiSCSIPKAuthKey,kiSCSIPVAuthCHAP);
        iSCSIAuthGetCHAPValues(targetAuth,&user,&sharedSecret);
        iSCSIPLSetCHAPSecretForNode(targetIQN,user,sharedSecret);
    }

    targetNodesCacheModified = true;
}

/*! Copies an authentication object associated the intiator.
 *  @return the authentication object. */
iSCSIAuthRef iSCSIPLCopyAuthenticationForInitiator()
{
    CFMutableDictionaryRef initiatorInfo = iSCSIPLGetInitiator(false);

    iSCSIAuthRef auth = NULL;

    if(initiatorInfo) {
        CFStringRef authMethod = CFDictionaryGetValue(initiatorInfo,kiSCSIPKAuthKey);

        if(CFStringCompare(authMethod,kiSCSIPVAuthCHAP,0) == kCFCompareEqualTo) {
            CFStringRef user, sharedSecret;

            CFStringRef initiatorIQN = iSCSIPLCopyInitiatorIQN();
            if(iSCSIPLCopyCHAPSecretForNode(initiatorIQN,&user,&sharedSecret) == errSecSuccess)
                auth = iSCSIAuthCreateCHAP(user,sharedSecret);
            else
                auth = iSCSIAuthCreateNone();
            CFRelease(initiatorIQN);
        }
        else
            auth = iSCSIAuthCreateNone();
    }
    return auth;
}

/*! Sets an authentication object associated the initiator.
 *  @param auth the authenticaiton object. */
void iSCSIPLSetAuthenticationForInitiator(iSCSIAuthRef initiatorAuth)
{
    CFMutableDictionaryRef initiatorInfo = iSCSIPLGetInitiator(true);

    if(iSCSIAuthGetMethod(initiatorAuth) == kiSCSIAuthMethodNone)
        CFDictionarySetValue(initiatorInfo,kiSCSIPKAuthKey,kiSCSIPVAuthNone);
    else {
        CFStringRef user, sharedSecret;
        CFDictionarySetValue(initiatorInfo,kiSCSIPKAuthKey,kiSCSIPVAuthCHAP);
        iSCSIAuthGetCHAPValues(initiatorAuth,&user,&sharedSecret);

        CFStringRef initiatorIQN = iSCSIPLCopyInitiatorIQN();
        iSCSIPLSetCHAPSecretForNode(initiatorIQN,user,sharedSecret);
        CFRelease(initiatorIQN);
    }

    initiatorNodeCacheModified = true;
}

void iSCSIPLSetConnectionConfig(CFStringRef targetIQN,
                                CFStringRef portalAddress,
                                iSCSIConnectionConfigRef connCfg)
{
    // Get the dictionary containing information about the portal
    CFMutableDictionaryRef portalInfo = iSCSIPLGetPortalInfo(targetIQN,portalAddress,true);
    
    // Set the authentication object
    CFDictionaryRef connCfgDict = iSCSIConnectionConfigCreateDictionary(connCfg);
    CFDictionarySetValue(portalInfo,kiSCSIPKConnectionCfgKey,connCfgDict);
    CFRelease(connCfgDict);
    
    targetNodesCacheModified = true;
}

void iSCSIPLSetPortalForTarget(CFStringRef targetIQN,
                               iSCSIPortalRef portal)
{
    // Get the dictionary containing information about the portal
    CFMutableDictionaryRef portalInfo = iSCSIPLGetPortalInfo(targetIQN,iSCSIPortalGetAddress(portal),true);
    
    // Set the authentication object
    CFDictionaryRef portalDict = iSCSIPortalCreateDictionary(portal);
    CFDictionarySetValue(portalInfo,kiSCSIPKPortalKey,portalDict);
    CFRelease(portalDict);
    
    targetNodesCacheModified = true;
}

void iSCSIPLRemovePortalForTarget(CFStringRef targetIQN,
                                  CFStringRef portalAddress)
{
    CFMutableDictionaryRef portalsList = iSCSIPLGetPortalsList(targetIQN,false);
    
    if(!portalsList)
        return;
    
    CFDictionaryRemoveValue(portalsList,portalAddress);
    
    targetNodesCacheModified = true;
}

void iSCSIPLSetTarget(iSCSITargetRef target)
{
    // Get the target information dictionary
    CFMutableDictionaryRef targetInfo = iSCSIPLGetTargetInfo(iSCSITargetGetIQN(target),true);
    CFDictionaryRef targetDict = iSCSITargetCreateDictionary(target);
    CFDictionarySetValue(targetInfo,kiSCSIPKTargetKey,targetDict);
    CFRelease(targetDict);

    targetNodesCacheModified = true;
}

void iSCSIPLRemoveTarget(CFStringRef targetIQN)
{
    CFMutableDictionaryRef targetsList = iSCSIPLGetTargets(false);
    
    if(!targetsList)
        return;
    
    CFDictionaryRemoveValue(targetsList,targetIQN);
    
    targetNodesCacheModified = true;
}

/*! Copies the initiator name from the iSCSI property list to a CFString object.
 *  @return the initiator name. */
CFStringRef iSCSIPLCopyInitiatorIQN()
{
    if(!initiatorCache)
        return NULL;
    
    // Lookup and copy the initiator name from the dictionary
    CFStringRef initiatorIQN = CFStringCreateCopy(
        kCFAllocatorDefault,CFDictionaryGetValue(initiatorCache,kiSCSIPKInitiatorIQN));
    
    return initiatorIQN;
}

/*! Sets the initiator name in the iSCSI property list.
 *  @param initiatorIQN the initiator name to set. */
void iSCSIPLSetInitiatorIQN(CFStringRef initiatorIQN)
{
    if(!initiatorCache)
        initiatorCache = iSCSIPLCreateInitiatorDict();
    
    // Update initiator name
    CFDictionarySetValue(initiatorCache,kiSCSIPKInitiatorIQN,initiatorIQN);
    
    initiatorNodeCacheModified = true;
}

/*! Copies the initiator alias from the iSCSI property list to a CFString object.
 *  @return the initiator alias. */
CFStringRef iSCSIPLCopyInitiatorAlias()
{
    if(!initiatorCache)
        return NULL;
    
    // Lookup and copy the initiator alias from the dictionary
    CFStringRef initiatorAlias = CFStringCreateCopy(
        kCFAllocatorDefault,CFDictionaryGetValue(initiatorCache,kiSCSIPKInitiatorAlias));
    
    return initiatorAlias;
}

/*! Sets the initiator alias in the iSCSI property list.
 *  @param initiatorAlias the initiator alias to set. */
void iSCSIPLSetInitiatorAlias(CFStringRef initiatorAlias)
{
    if(!initiatorCache)
        initiatorCache = iSCSIPLCreateInitiatorDict();
    
    // Update initiator alias
    CFDictionarySetValue(initiatorCache,kiSCSIPKInitiatorAlias,initiatorAlias);
    
    initiatorNodeCacheModified = true;
}

/*! Gets whether a target is defined in the property list.
 *  @param targetIQN the name of the target.
 *  @return true if the target exists, false otherwise. */
Boolean iSCSIPLContainsTarget(CFStringRef targetIQN)
{
    CFDictionaryRef targetsList = iSCSIPLGetTargets(false);
    return CFDictionaryContainsKey(targetsList,targetIQN);
}

/*! Gets whether a portal is defined in the property list.
 *  @param targetIQN the name of the target.
 *  @param portalAddress the name of the portal.
 *  @return true if the portal exists, false otherwise. */
Boolean iSCSIPLContainsPortalForTarget(CFStringRef targetIQN,
                              CFStringRef portalAddress)
{
    CFDictionaryRef portalsList = iSCSIPLGetPortalsList(targetIQN,false);
    return (portalsList && CFDictionaryContainsKey(portalsList,portalAddress));
}


/*! Creates an array of target names (fully qualified IQN or EUI names)
 *  defined in the property list.
 *  @return an array of target names. */
CFArrayRef iSCSIPLCreateArrayOfTargets()
{
    CFDictionaryRef targetsList = iSCSIPLGetTargets(false);
    
    if(!targetsList)
        return NULL;

    const CFIndex keyCount = CFDictionaryGetCount(targetsList);
    
    if(keyCount == 0)
        return NULL;
  
    const void * keys[keyCount];
    CFDictionaryGetKeysAndValues(targetsList,keys,NULL);
    
    return CFArrayCreate(kCFAllocatorDefault,keys,keyCount,&kCFTypeArrayCallBacks);
}

/*! Creates an array of portal names for a given target.
 *  @param targetIQN the name of the target (fully qualified IQN or EUI name).
 *  @return an array of portal names for the specified target. */
CFArrayRef iSCSIPLCreateArrayOfPortals(CFStringRef targetIQN)
{
    CFMutableDictionaryRef portalsList = iSCSIPLGetPortalsList(targetIQN,false);
    
    if(!portalsList)
        return NULL;
    
    const CFIndex keyCount = CFDictionaryGetCount(portalsList);
    
    if(keyCount == 0)
        return NULL;
    
    const void * keys[keyCount];
    CFDictionaryGetKeysAndValues(portalsList,keys,NULL);
    
    return CFArrayCreate(kCFAllocatorDefault,keys,keyCount,&kCFTypeArrayCallBacks);
}

/*! Adds a discovery record to the property list.
 *  @param discoveryRecord the record to add. */
void iSCSIPLAddDiscoveryRecord(iSCSIDiscoveryRecRef discoveryRecord)
{
    // Iterate over the dictionary and add keys to the existing cache
    CFDictionaryRef discoveryDict = iSCSIDiscoveryRecCreateDictionary(discoveryRecord);

    if(!discoveryDict)
        return;
    
    if(!discoveryCache)
        discoveryCache = iSCSIPLCreateDiscoveryDict();
    
    const CFIndex count = CFDictionaryGetCount(discoveryDict);
    const void * keys[count];
    const void * values[count];
    CFDictionaryGetKeysAndValues(discoveryDict,keys,values);
    
    for(CFIndex idx = 0; idx < count; idx++) {
        CFDictionarySetValue(discoveryCache,keys[idx],values[idx]);
    }
    
    CFRelease(discoveryDict);
    discoveryCacheModified = true;
}

/*! Retrieves the discovery record from the property list.
 *  @return the cached discovery record. */
iSCSIDiscoveryRecRef iSCSIPLCopyDiscoveryRecord()
{
    if(!discoveryCache)
        return NULL;
    
    return iSCSIDiscoveryRecCreateWithDictionary(discoveryCache);
}

/*! Clears the discovery record. */
void iSCSIPLClearDiscoveryRecord()
{
    CFRelease(discoveryCache);
    discoveryCache = NULL;
    discoveryCacheModified = true;
}

/*! Synchronizes the intitiator and target settings cache with the property
 *  list on the disk. */
void iSCSIPLSynchronize()
{
    // If we have modified our targets dictionary, we write changes back and
    // otherwise we'll read in the latest.
    if(targetNodesCacheModified)
        CFPreferencesSetValue(kiSCSIPKTargetsKey,targetsCache,kiSCSIPKAppId,
                              kCFPreferencesAnyUser,kCFPreferencesCurrentHost );
    
    if(initiatorNodeCacheModified)
        CFPreferencesSetValue(kiSCSIPKInitiatorKey,initiatorCache,kiSCSIPKAppId,
                              kCFPreferencesAnyUser,kCFPreferencesCurrentHost );
    
    if(discoveryCacheModified)
        CFPreferencesSetValue(kiSCSIPKDiscoveryKey,discoveryCache,kiSCSIPKAppId,
                              kCFPreferencesAnyUser,kCFPreferencesCurrentHost);

    CFPreferencesAppSynchronize(kiSCSIPKAppId);
    
    if(!targetNodesCacheModified)
    {
        // Free old cache if present
        if(targetsCache)
            CFRelease(targetsCache);
        
        // Refresh cache from preferences
        targetsCache = iSCSIPLCopyPropertyDict(kiSCSIPKTargetsKey);
    }
    
    if(!initiatorNodeCacheModified)
    {
        // Free old cache if present
        if(initiatorCache)
            CFRelease(initiatorCache);
        
        // Refresh cache from preferences
        initiatorCache = iSCSIPLCopyPropertyDict(kiSCSIPKInitiatorKey);
    }
    
    if(!discoveryCacheModified)
    {
        // Free old cache if present
        if(discoveryCache)
            CFRelease(discoveryCache);
        
        // Refresh cache from preferences
        discoveryCache = iSCSIPLCopyPropertyDict(kiSCSIPKDiscoveryKey);
    }

    initiatorNodeCacheModified = targetNodesCacheModified = discoveryCacheModified = false;
}
