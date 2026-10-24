// arc-wasm: the "account" service, a lean IAccountManager (topic userspace,
// docs/topics/userspace.md): a device with no accounts and no authenticators.
// Without it AccountManager has a null interface (Settings'
// AuthenticatorHelper.updateAuthDescriptions throws NullPointerException).
// Queries answer empty; adding or changing accounts throws
// (arcwasm.server.base.AccountManagerBase).
package arcwasm.server;

import android.accounts.Account;
import android.accounts.AuthenticatorDescription;
import android.accounts.IAccountManagerResponse;
import android.os.Bundle;
import android.os.RemoteException;

import arcwasm.server.base.AccountManagerBase;

public final class AccountManagerService extends AccountManagerBase {
    private static final Account[] NONE = new Account[0];

    @Override public AuthenticatorDescription[] getAuthenticatorTypes() {
        return new AuthenticatorDescription[0];
    }
    @Override public Account[] getAccounts(String type) { return NONE; }
    @Override public Account[] getAccountsForPackage(String packageName, int uid) { return NONE; }
    @Override public Account[] getAccountsByTypeForPackage(String type, String packageName) {
        return NONE;
    }
    @Override public Account[] getAccountsAsUser(String type, int userId) { return NONE; }

    @Override
    public void getAccountsByFeatures(IAccountManagerResponse response, String type,
            String[] features) throws RemoteException {
        Bundle result = new Bundle();
        result.putParcelableArray(android.accounts.AccountManager.KEY_ACCOUNTS, NONE);
        response.onResult(result);
    }
}
