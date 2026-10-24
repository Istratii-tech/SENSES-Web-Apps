package com.android.internal.telephony;

import android.os.Message;

/**
 * arc-wasm (topic userspace): the part of frameworks/opt/telephony's IccCard
 * that Settings 4.4 uses (IccLockSettings). PhoneFactory's phone has none.
 */
public interface IccCard {
    boolean getIccLockEnabled();
    void setIccLockEnabled(boolean enabled, String password, Message onComplete);
    void changeIccLockPassword(String oldPassword, String newPassword, Message onComplete);
    boolean hasIccCard();
}
