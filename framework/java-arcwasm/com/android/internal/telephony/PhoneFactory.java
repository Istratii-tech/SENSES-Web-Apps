package com.android.internal.telephony;

import android.os.Message;
import android.telephony.ServiceState;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;

/**
 * arc-wasm (topic userspace): frameworks/opt/telephony's PhoneFactory for
 * Settings 4.4 (About phone > Status and friends). The default phone is one
 * without a radio or SIM: power off, no numbers, requests never answered.
 */
public final class PhoneFactory {
    private static Phone sPhone;

    public static synchronized Phone getDefaultPhone() {
        if (sPhone == null) {
            sPhone = (Phone) Proxy.newProxyInstance(Phone.class.getClassLoader(),
                    new Class<?>[] { Phone.class }, new NoRadio());
        }
        return sPhone;
    }

    private static final class NoRadio implements InvocationHandler {
        private IccCard mIccCard;

        @Override
        public Object invoke(Object proxy, Method method, Object[] args) {
            Class<?> type = method.getReturnType();
            if (method.getDeclaringClass() == Object.class) {
                if (method.getName().equals("equals")) {
                    return proxy == args[0];
                }
                if (method.getName().equals("hashCode")) {
                    return System.identityHashCode(proxy);
                }
                return "PhoneFactory.NoRadio";
            }
            if (type == ServiceState.class) {
                ServiceState state = new ServiceState();
                state.setStateOff();
                return state;
            }
            if (type == IccCard.class) {
                synchronized (this) {
                    if (mIccCard == null) {
                        mIccCard = (IccCard) Proxy.newProxyInstance(IccCard.class.getClassLoader(),
                                new Class<?>[] { IccCard.class }, this);
                    }
                    return mIccCard;
                }
            }
            if (method.getName().equals("getPhoneName")) {
                return "GSM";
            }
            if (method.getName().equals("getPhoneType")) {
                return PhoneConstants.PHONE_TYPE_NONE;
            }
            if (type == boolean.class) {
                return false;
            }
            if (type == int.class) {
                return 0;
            }
            if (type == long.class) {
                return 0L;
            }
            return null;
        }
    }
}
