/*
 * Copyright (c) 2006 Apple Computer, Inc. All Rights Reserved.
 * 
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

//
// CodeSigner - SecCodeSigner API objects
//
#include "CodeSigner.h"
#include "signer.h"
#include "reqparser.h"
#include "renum.h"
#include <security_utilities/unix++.h>
#include <security_utilities/unixchild.h>
#include <vector>

namespace Security {

__SEC_CFTYPE(SecIdentity)

namespace CodeSigning {

using namespace UnixPlusPlus;


//
// A helper for parsing out a CFDictionary signing-data specification
//
class SecCodeSigner::Parser : CFDictionary {
public:
	Parser(SecCodeSigner &signer, CFDictionaryRef parameters);
	
	bool getBool(CFStringRef key) const
	{
		if (CFBooleanRef flag = get<CFBooleanRef>(key))
			return flag == kCFBooleanTrue;
		else
			return false;
	}
};


//
// Construct a SecCodeSigner
//
SecCodeSigner::SecCodeSigner()
	: mRequirements(NULL)
{
	secdebug("signer", "%p created", this);
}


//
// Clean up a SecCodeSigner
//
SecCodeSigner::~SecCodeSigner() throw()
{
	secdebug("signer", "%p destroyed", this);
	::free((Requirements *)mRequirements);
}


//
// Parse an input parameter dictionary and set ready-to-use parameters
//
void SecCodeSigner::parameters(CFDictionaryRef paramDict)
{
	secdebug("signer", "%p loading %d parameters", this, int(CFDictionaryGetCount(paramDict)));
	Parser(*this, paramDict);
	if (!valid())
		MacOSError::throwMe(errSecCSInvalidObjectRef);
}


//
// Sign code
//
void SecCodeSigner::sign(SecStaticCode *code, SecCSFlags flags)
{
	if (!valid())
		MacOSError::throwMe(errSecCSInvalidObjectRef);
	secdebug("signer", "%p will sign %p (flags 0x%x)", this, code, flags);
	Signer(*this, code).sign(flags);
	code->resetValidity();
}


//
// ReturnDetachedSignature is called by writers or editors that try to return
// detached signature data (rather than annotate the target).
//
void SecCodeSigner::returnDetachedSignature(BlobCore *blob)
{
	assert(mDetached);
	if (CFGetTypeID(mDetached) == CFURLGetTypeID()) {
		// URL to destination file
		AutoFileDesc fd(cfString(CFURLRef(mDetached.get())), O_WRONLY | O_CREAT | O_TRUNC);
		fd.writeAll(*blob);
	} else if (CFGetTypeID(mDetached) == CFDataGetTypeID()) {
		CFDataAppendBytes(CFMutableDataRef(mDetached.get()),
			(const UInt8 *)blob, blob->length());
	} else
		assert(false);
}


//
// The actual parsing operation is done in the Parser class.
//
// Note that we need to copy or retain all incoming data. The caller has no requirement
// to keep the parameters dictionary around.
//
SecCodeSigner::Parser::Parser(SecCodeSigner &state, CFDictionaryRef parameters)
	: CFDictionary(parameters, errSecCSBadDictionaryFormat)
{
	// the signer may be an identity or null
	if (CFTypeRef signer = get<CFTypeRef>(kSecCodeSignerIdentity))
		if (CFGetTypeID(signer) == SecIdentityGetTypeID() || signer == kCFNull)
			state.mSigner = SecIdentityRef(signer);
		else
			MacOSError::throwMe(errSecCSInvalidObjectRef);
	else
		MacOSError::throwMe(errSecCSInvalidObjectRef);

	// the flags need some augmentation
	if (CFNumberRef flags = get<CFNumberRef>(kSecCodeSignerFlags)) {
		state.mCdFlagsGiven = true;
		state.mCdFlags = cfNumber<uint32_t>(flags);
	} else
		state.mCdFlagsGiven = false;

	if (CFNumberRef cmsSize = get<CFNumberRef>(CFSTR("cmssize")))
		state.mCMSSize = cfNumber<size_t>(cmsSize);
	else
		state.mCMSSize = 5000;	// likely big enough

	// signing time can be a CFDateRef or null
	if (CFTypeRef time = get<CFTypeRef>(kSecCodeSignerSigningTime))
		if (CFGetTypeID(time) == CFDateGetTypeID() || time == kCFNull)
			state.mSigningTime = CFDateRef(time);
		else
			MacOSError::throwMe(errSecCSInvalidObjectRef);
	
	if (CFStringRef ident = get<CFStringRef>(kSecCodeSignerIdentifier))
		state.mIdentifier = cfString(ident);
	
	if (CFStringRef prefix = get<CFStringRef>(kSecCodeSignerIdentifierPrefix))
		state.mIdentifierPrefix = cfString(prefix);
	
	// requirements can be binary or string (to be compiled)
	if (CFTypeRef reqs = get<CFTypeRef>(kSecCodeSignerRequirements)) {
		if (CFGetTypeID(reqs) == CFDataGetTypeID()) {		// binary form
			const Requirements *rp = (const Requirements *)CFDataGetBytePtr(CFDataRef(reqs));
			state.mRequirements = rp->clone();
		} else if (CFGetTypeID(reqs) == CFStringGetTypeID()) { // text form
			state.mRequirements = parseRequirements(cfString(CFStringRef(reqs)));
		} else
			MacOSError::throwMe(errSecCSInvalidObjectRef);
	} else
		state.mRequirements = NULL;
	
	state.mNoMachO = getBool(CFSTR("no-macho"));
	
	state.mPageSize = get<CFNumberRef>(kSecCodeSignerPageSize);
	
	// detached can be (destination) file URL or (mutable) Data to be appended-to
	if (state.mDetached = get<CFTypeRef>(kSecCodeSignerDetached)) {
		if (CFGetTypeID(state.mDetached) != CFURLGetTypeID()
			&& CFGetTypeID(state.mDetached) != CFDataGetTypeID())
			MacOSError::throwMe(errSecCSInvalidObjectRef);
	}
	
	state.mDryRun = getBool(kSecCodeSignerDryRun);

	state.mResourceRules = get<CFDictionaryRef>(kSecCodeSignerResourceRules);
	
	state.mApplicationData = get<CFDataRef>(kSecCodeSignerApplicationData);
	state.mEntitlementData = get<CFDataRef>(kSecCodeSignerEntitlements);
}


} // end namespace CodeSigning
} // end namespace Security
