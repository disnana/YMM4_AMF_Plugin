#pragma once

#include "AmfNative.h"

// A finite timeout allows the output worker to observe stop requests. This is
// an upper wait budget, not a sleep added after every encoded frame.
constexpr int32_t kAmfOutputWaitTimeoutMs = 10;

// Callbacks return AMF_RESULT (0 = AMF_OK). Kept independent of an AMF device so
// unsupported/rejected properties and rollback can be exercised on CPU-only CI.
template<class Probe, class SetTimeout, class GetTimeout>
bool ConfigureAmfOutputWait(bool requested, Probe probe, SetTimeout setTimeout,
    GetTimeout getTimeout, AmfOutputWaitStatus& status)
{
    status = { 1, requested ? 1u : 0u, 0, AmfOutputWaitDisabled, -1, -1, 0, 0 };
    if (!requested) return true; // Do not touch legacy encoder properties.

    bool supported = false;
    status.capabilityResult = probe(supported);
    if (status.capabilityResult != 0)
    {
        status.reason = AmfOutputWaitCapsUnavailable;
        return true;
    }
    if (!supported)
    {
        status.reason = AmfOutputWaitUnsupported;
        return true;
    }

    status.propertyResult = setTimeout(kAmfOutputWaitTimeoutMs);
    if (status.propertyResult != 0) status.reason = AmfOutputWaitPropertyRejected;
    else
    {
        int64_t actual = -1;
        status.propertyResult = getTimeout(actual);
        if (status.propertyResult != 0) status.reason = AmfOutputWaitReadbackFailed;
        else if (actual != kAmfOutputWaitTimeoutMs) status.reason = AmfOutputWaitReadbackMismatch;
        else
        {
            status.reason = AmfOutputWaitEnabled;
            status.timeoutMs = kAmfOutputWaitTimeoutMs;
            return true;
        }
    }

    // A rejected/partly applied setting must not leave an unknown blocking wait
    // behind while reporting legacy polling. Fail initialization if reset fails.
    int64_t resetValue = -1;
    if (setTimeout(0) != 0 || getTimeout(resetValue) != 0 || resetValue != 0)
    {
        status.reason = AmfOutputWaitResetFailed;
        return false;
    }
    return true;
}

inline const char* AmfOutputWaitReasonName(uint32_t reason)
{
    switch (reason)
    {
    case AmfOutputWaitDisabled: return "disabled";
    case AmfOutputWaitEnabled: return "enabled";
    case AmfOutputWaitCapsUnavailable: return "capability_unavailable";
    case AmfOutputWaitUnsupported: return "unsupported";
    case AmfOutputWaitPropertyRejected: return "property_rejected";
    case AmfOutputWaitReadbackFailed: return "readback_failed";
    case AmfOutputWaitReadbackMismatch: return "readback_mismatch";
    case AmfOutputWaitResetFailed: return "reset_failed";
    case AmfOutputWaitInitializationChanged: return "initialization_changed";
    default: return "unknown";
    }
}

static_assert(sizeof(AmfOutputWaitStatus) == 40, "Output-wait status ABI mismatch");
