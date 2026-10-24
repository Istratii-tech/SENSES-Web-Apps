package com.android.internal.telephony;

import android.os.Message;
import android.telephony.ServiceState;

/**
 * arc-wasm (topic userspace): the part of frameworks/opt/telephony's Phone
 * that Settings 4.4 uses (Status, RadioInfo, BandMode, IccLockSettings).
 * PhoneFactory.getDefaultPhone() implements it as a phone without a radio.
 */
public interface Phone {
    String getPhoneName();
    int getPhoneType();
    ServiceState getServiceState();
    String getDeviceId();
    String getImei();
    String getMeid();
    String getLine1Number();
    String getIccSerialNumber();
    String getCdmaMin();
    String getCdmaPrlVersion();
    int getLteOnCdmaMode();
    IccCard getIccCard();
    boolean isDnsCheckDisabled();
    void disableDnsCheck(boolean b);
    void getDataCallList(Message response);
    void getNeighboringCids(Message response);
    void getPreferredNetworkType(Message response);
    void setPreferredNetworkType(int networkType, Message response);
    void getSmscAddress(Message result);
    void setSmscAddress(String address, Message result);
    void queryAvailableBandMode(Message response);
    void setBandMode(int bandMode, Message response);
    void setCellInfoListRate(int rateInMillis);
    void setRadioPower(boolean power);
}
